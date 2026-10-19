// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "convert.hpp"

#include "cli_common.hpp"
#include "export.hpp"
#include "inspect.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/config.hpp>
#include <esses/base/priority.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/formats/ld_importer.hpp>
#include <esses/formats/registry.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/model/source.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace esses::cli {

namespace {

using nlohmann::json;

// A path compared as the file system would: absolute, normalised, case-folded on Windows (NTFS names are
// case-insensitive by default).
std::string comparable(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec) {
        abs = p;
    }
    std::string s = u8(abs.lexically_normal());
#if defined(ESSES_OS_WINDOWS)
    std::replace(s.begin(), s.end(), '\\', '/');
    s = caseFold(s);
#endif
    return s;
}

bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (comparable(a) == comparable(b)) {
        return true;
    }
    std::error_code ec;
    const bool both = std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec);
    return both && std::filesystem::equivalent(a, b, ec) && !ec;
}

std::filesystem::path withExtension(std::filesystem::path p, const char* extension) {
    p.replace_extension(extension);
    return p;
}

// The samples of `series` whose time lies in [start, end) (time from 0 at the first of them), as a fresh
// in-memory column of the same dtype; nullopt when the dtype has no in-memory form here (I64).
struct Slice {
    store::UniformSeries series;
    std::int64_t samples = 0;
};

Result<std::optional<Slice>> sliceUniform(const store::UniformSeries& series, Micros start, Micros end,
                                          store::ChunkCache& cache, const std::string& name) {
    const store::SampleIdx n = series.size();
    const auto firstAtOrAfter = [&series, n](Micros t) {
        store::SampleIdx i = std::clamp<store::SampleIdx>(series.indexAt(t), 0, n);
        while (i > 0 && series.timeAt(i - 1) >= t) {
            --i;
        }
        while (i < n && series.timeAt(i) < t) {
            ++i;
        }
        return i;
    };
    const store::SampleIdx i0 = firstAtOrAfter(start);
    const store::SampleIdx i1 = std::max(i0, firstAtOrAfter(end));
    const auto count = static_cast<std::size_t>(i1 - i0);
    const store::Column& col = series.values;
    std::vector<double> values(count);
    std::vector<bool> valid(count, true);
    for (store::SampleIdx c = i0; c < i1;) {
        const store::SampleIdx chunkEnd =
            std::min(i1, store::chunkFirstSample(c / store::SampleIdx{store::kChunkSamples} + 1));
        auto h = col.require(c, chunkEnd - 1, Priority::Background, CancelToken{});
        if (!h) {
            return Failure{h.error()};
        }
        for (store::SampleIdx i = c; i < chunkEnd; ++i) {
            const auto k = static_cast<std::size_t>(i - i0);
            values[k] = h->at(i);
            valid[k] = h->validAt(i);
        }
        c = chunkEnd;
    }
    store::ColumnOptions co;
    co.cache = &cache;
    std::shared_ptr<store::InMemoryProvider> provider;
    switch (col.dtype()) {
    case store::Dtype::F32: {
        std::vector<float> f(count);
        for (std::size_t k = 0; k < count; ++k) {
            f[k] = static_cast<float>(values[k]);
        }
        provider = store::InMemoryProvider::fromF32(f, valid, "convert:" + name);
        break;
    }
    case store::Dtype::F64:
        provider = store::InMemoryProvider::fromF64(values, valid, "convert:" + name);
        break;
    case store::Dtype::I32: {
        std::vector<std::int32_t> v(count);
        for (std::size_t k = 0; k < count; ++k) {
            const double x = values[k];
            // An invalid integer sample may hold anything; clamp before the conversion (never UB).
            v[k] = std::isfinite(x) && x >= -2147483648.0 && x <= 2147483647.0
                       ? static_cast<std::int32_t>(std::llround(x))
                       : 0;
        }
        provider = store::InMemoryProvider::fromI32(v, valid, "convert:" + name);
        break;
    }
    case store::Dtype::I64:
        return std::optional<Slice>{};
    }
    Slice out;
    out.series = store::UniformSeries{series.rate, 0, store::Column(std::move(provider), co)};
    out.samples = static_cast<std::int64_t>(count);
    return std::optional<Slice>(std::move(out));
}

// The layer cut to [start, end): every uniform channel sliced as above, the markers in [start, end] moved by
// -start (the synthesised start and end of log are rebuilt by the reader), no ranges (the reader's rules
// rebuild them), duration end - start.
Result<model::DataLayer> sliceLayer(const model::DataLayer& layer, const std::vector<model::ChannelId>& ids,
                                    Micros start, Micros end, store::ChunkCache& cache, json& skipped) {
    model::DataLayer out;
    out.name = layer.name;
    out.origin = layer.origin;
    out.duration = end - start;
    out.enumTables = layer.enumTables;
    for (const model::ChannelId& id : ids) {
        const model::Channel* c = layer.findById(id);
        if (c == nullptr) {
            continue;
        }
        const auto* uniform = std::get_if<store::UniformSeries>(&c->series);
        if (uniform == nullptr) {
            skipped.push_back(json{{"channel", c->desc.name}, {"reason", "timestamped"}});
            continue;
        }
        Result<std::optional<Slice>> slice = sliceUniform(*uniform, start, end, cache, c->desc.name);
        if (!slice) {
            return Failure{slice.error()};
        }
        std::optional<Slice> kept = std::move(slice).value();
        if (!kept.has_value()) {
            skipped.push_back(json{{"channel", c->desc.name}, {"reason", "dtype"}});
            continue;
        }
        model::Channel ch;
        ch.desc = c->desc;
        ch.series = std::move(kept->series);
        out.addChannel(std::move(ch));
    }
    const auto moved = [start, end](const std::vector<model::Marker>& from) {
        std::vector<model::Marker> to;
        for (const model::Marker& m : from) {
            // The marker at the range's end stays (a lap's closing beacon belongs to the lap).
            if (m.cls == model::MarkerClass::SOL || m.cls == model::MarkerClass::EOL || m.t < start ||
                m.t > end) {
                continue;
            }
            model::Marker k = m;
            k.t -= start;
            if (k.originalT) {
                *k.originalT -= start;
            }
            to.push_back(std::move(k));
        }
        return to;
    };
    out.markers = moved(layer.markers);
    out.baseMarkers = moved(layer.baseMarkers);
    return out;
}

json notesJson(const std::vector<formats::ImportLogEntry>& notes) {
    json out = json::array();
    for (const formats::ImportLogEntry& e : notes) {
        json n{{"code", e.code},
               {"level", std::string(formats::importLogLevelName(e.level))},
               {"message", e.message}};
        if (e.ctx.channel) {
            n["channel"] = *e.ctx.channel;
        }
        out.push_back(std::move(n));
    }
    return out;
}

} // namespace

bool convertWouldOverwriteInput(const std::filesystem::path& input, const std::filesystem::path& output,
                                bool ldOutput) {
    const std::vector<std::filesystem::path> protectedFiles{input, withExtension(input, ".ldx"),
                                                            model::SidecarDocument::pathFor(input)};
    std::vector<std::filesystem::path> written{output};
    if (ldOutput) {
        written.push_back(withExtension(output, ".ldx"));
    }
    for (const std::filesystem::path& w : written) {
        for (const std::filesystem::path& p : protectedFiles) {
            if (samePath(w, p)) {
                return true;
            }
        }
    }
    return false;
}

int runConvert(AppContext& ctx, const ConvertOptions& o, std::ostream& out, std::ostream& err) {
    const std::string ldId(formats::kLdImporterId);
    const std::string legacyCsvId(formats::kLegacyCsvWriterId);
    const std::string csvId(formats::kGenericCsvWriterId);
    if (o.format != csvId && o.format != legacyCsvId && o.format != ldId) {
        err << "esses-cli convert: --format is " << csvId << ", " << legacyCsvId << " or " << ldId << "\n";
        return 2;
    }
    if (!o.output) {
        err << "esses-cli convert: -o <file> is required\n";
        return 2;
    }
    const bool ld = o.format == ldId;
    if (convertWouldOverwriteInput(o.input, *o.output, ld)) {
        err << "esses-cli convert: refusing to overwrite the input: " << u8(*o.output)
            << (ld ? " (or its .ldx)" : "") << " is " << u8(o.input) << ", its companion or its sidecar\n";
        return 2;
    }
    std::optional<Rate> gridRate;
    if (!o.rate.empty()) {
        if (ld) {
            err << "esses-cli convert: --rate: the " << ldId
                << " writer keeps each channel's own rate and does not resample in 0.1\n";
            return 2;
        }
        Result<Rate> rate = parseRate(o.rate);
        if (!rate || Rate{1'000'000, 1} < *rate) {
            err << "esses-cli convert: --rate '" << o.rate
                << "': " << (rate ? std::string("the grid is at most 1 MHz") : rate.error().message)
                << " (a number of Hz or num/den)\n";
            return 2;
        }
        gridRate = *rate;
    }
    formats::WriterOptions wopts;
    if (!o.writerOptions.empty()) {
        Result<model::OpaqueJson> j = model::OpaqueJson::parse(o.writerOptions);
        if (!j || !j->isObject()) {
            err << "esses-cli convert: --writer-options is a JSON object, e.g. "
                << R"({"ld":{"dialect":"m1"}})" << "\n";
            return 2;
        }
        wopts.options = std::move(j).value();
    }
    Result<formats::ImportOptions> importOptions = makeImportOptions(o.importerId, o.optionsJson);
    if (!importOptions) {
        err << "esses-cli convert: " << describe(importOptions.error()) << "\n";
        return 2;
    }
    Result<const units::UnitRegistry*> unitsResult = ctx.units();
    if (!unitsResult) {
        err << "esses-cli convert: " << describe(unitsResult.error()) << "\n";
        return 2;
    }
    const units::UnitRegistry* units = *unitsResult; // null with --no-units

    Result<formats::OpenedSource> opened =
        openFile(ctx, o.input, *importOptions, units != nullptr, !o.noSidecar, true);
    if (!opened) {
        err << "esses-cli convert: " << describe(opened.error()) << "\n";
        return 1;
    }
    const std::shared_ptr<const model::DataSource> source = opened->source;
    const model::DataLayer* layer = source->layer(o.layer);
    if (layer == nullptr) {
        err << "esses-cli convert: no layer '" << o.layer << "' in " << u8(o.input.filename()) << "\n";
        return 1;
    }
    const model::Range* range = nullptr;
    if (!o.range.empty()) {
        std::string rangePath;
        range = findRange(*layer, o.range, rangePath);
        if (range == nullptr) {
            err << "esses-cli convert: no range '" << o.range << "' in layer " << layer->name
                << " (a range is <group path>/<number>, e.g. \"Outings:Laps/3\")\n";
            return 1;
        }
    }

    // The channels, in the order asked (else the layer's).
    std::vector<model::ChannelId> ids;
    json names = json::array();
    if (o.channels.empty()) {
        for (const model::Channel& c : layer->channels) {
            ids.push_back(c.desc.id);
            names.push_back(c.desc.name);
        }
    } else {
        for (const std::string& name : o.channels) {
            const model::Channel* c = layer->find(name);
            if (c == nullptr) {
                c = layer->findLogged(name);
            }
            if (c == nullptr) {
                err << "esses-cli convert: no channel '" << name << "' in " << u8(o.input.filename()) << "\n";
                return 1;
            }
            ids.push_back(c->desc.id);
            names.push_back(c->desc.name);
        }
    }

    formats::ExportView view;
    view.layer = layer->name;
    view.resampleRate = gridRate;
    json skipped = json::array();
    if (ld && range != nullptr) {
        // The .ld writer writes whole layers: cut the layer to the range here.
        const Micros start = std::max<Micros>(range->start, 0);
        const Micros end = std::min<Micros>(range->end, layer->duration);
        Result<model::DataLayer> cut =
            sliceLayer(*layer, ids, start, std::max(start, end), ctx.cache(), skipped);
        if (!cut) {
            err << "esses-cli convert: " << describe(cut.error()) << "\n";
            return 1;
        }
        model::DataLayer cutLayer = std::move(cut).value();
        auto sliced = std::make_shared<model::DataSource>(*source);
        for (model::DataLayer& l : sliced->layers) {
            if (l.name == layer->name) {
                l = std::move(cutLayer);
                break; // layer names are unique
            }
        }
        view.source = sliced;
        // Every channel the cut kept, in order (the ids are unchanged).
        for (const model::ChannelId& id : ids) {
            if (const model::DataLayer* l = sliced->layer(layer->name); l != nullptr && l->findById(id)) {
                view.channels.push_back(id);
            }
        }
    } else {
        view.source = source;
        view.channels = ids;
        if (range != nullptr) {
            view.start = range->start;
            view.end = range->end;
        }
    }

    std::unique_ptr<formats::IWriter> writer = ld                  ? formats::makeLdWriter(units)
                                               : o.format == csvId ? formats::makeGenericCsvWriter(units)
                                                                   : formats::makeLegacyCsvWriter(units);
    Result<formats::WriteReport> report =
        writer->write(*o.output, view, wopts, formats::Progress{}, CancelToken{});
    if (!report) {
        err << "esses-cli convert: " << describe(report.error()) << "\n";
        return 1;
    }
    for (const formats::ImportLogEntry& e : report->notes) {
        err << "esses-cli convert: " << e.code << ": " << e.message
            << (e.ctx.channel ? " (" + *e.ctx.channel + ")" : std::string()) << "\n";
        if (e.code == "ld.channel_skipped" && e.ctx.channel) {
            skipped.push_back(json{{"channel", *e.ctx.channel}, {"reason", e.code}});
        }
    }
    // The channels written: the ones asked for, without the skipped ones.
    json written = json::array();
    for (const json& name : names) {
        const bool wasSkipped = std::any_of(skipped.begin(), skipped.end(),
                                            [&name](const json& s) { return s.at("channel") == name; });
        if (!wasSkipped) {
            written.push_back(name);
        }
    }
    if (o.json) {
        json files = json::array();
        for (const std::filesystem::path& f : report->files) {
            files.push_back(u8(f));
        }
        json doc{{"input", u8(o.input)},      {"output", u8(*o.output)}, {"format", o.format},
                 {"files", std::move(files)}, {"bytes", report->bytes},  {"channels", written},
                 {"skipped", skipped},        {"range", o.range},        {"notes", notesJson(report->notes)}};
        Result<std::string> text = canonicalText(doc);
        if (!text) {
            err << "esses-cli convert: " << describe(text.error()) << "\n";
            return 2;
        }
        out << *text;
    } else {
        out << u8(*o.output) << ": " << written.size() << " channels";
        if (!skipped.empty()) {
            out << " (" << skipped.size() << " skipped)";
        }
        out << ", " << report->bytes << " bytes\n";
    }
    return 0;
}

} // namespace esses::cli
