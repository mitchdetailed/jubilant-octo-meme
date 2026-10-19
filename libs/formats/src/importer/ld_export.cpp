// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The motec.ld IWriter (05 section 5): a DataSource layer to .ld + .ldx through the core's LdWriter and the
// .ldx DOM writer. Export only, never automatic (D-015); in M0 it writes the synthetic fixtures and the
// read -> write -> read round trips.
//
// Per channel (05 section 5 item 2):
//   * verbatim: a channel the motec.ld reader produced (its LdSourceEncoding has an origin) whose series
//     still matches that origin is written with its stored bytes, record and gap block unchanged, so
//     read -> write -> read is bit-exact for samples and validity;
//   * re-quantised: any other channel with an LdSourceEncoding: the column's values are converted back from
//     the base unit to the encoding's unit (through the UnitRegistry) and quantised with its scaling
//     (afterMul); invalid samples become gap-block intervals (NaN in float encodings, the previous valid raw
//     value in integer encodings, 05 section 5 item 5);
//   * default: no encoding: F32 -> (0x07, 4), F64 -> (0x08, 8), I32 -> (0x03, 2) when every valid value fits
//     16 bits, else (0x05, 4); identity scaling, the base unit's symbol.
// Channels the format cannot hold (timestamped, a rate that is not a whole number of Hz, Text) are skipped
// with ld.channel_skipped. Partial time ranges and resampling are not written in M0 (Unsupported).
#include "importer/ld_support.hpp"
#include "ld/ld_codes.hpp"

#include <esses/base/assert.hpp>
#include <esses/formats/ld_importer.hpp>
#include <esses/store/column.hpp>
#include <esses/store/series.hpp>
#include <esses/units/unit_registry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <system_error>
#include <utility>

namespace esses::formats {
namespace {

using namespace ldimpl;
using json = nlohmann::json;
using Level = ImportLogEntry::Level;

constexpr std::string_view kWriterOptionsSchema = R"({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "motec.ld writer options",
  "type": "object",
  "properties": {
    "ld": {
      "type": "object",
      "properties": {
        "dialect": {"enum": ["pc-layout", "m1", "c-series", "sim-exporter", "converter-legacy"]},
        "dataAlignment": {"type": "integer", "minimum": 1, "maximum": 4096},
        "verbatim": {"type": "boolean"}
      },
      "additionalProperties": false
    }
  },
  "additionalProperties": false
})";

struct WriteOpts {
    std::optional<ld::WriteDialect> dialect;
    std::optional<std::uint32_t> dataAlignment;
    bool verbatim = true;
};

Result<WriteOpts> parseWriteOptions(const model::OpaqueJson& options) {
    WriteOpts out;
    if (options.isNull()) {
        return out;
    }
    const auto invalid = [](const std::string& pointer, const std::string& message) {
        ErrorContext ctx;
        ctx.detail = pointer;
        ctx.importer = std::string(kLdImporterId);
        return fail(ErrorCode::Invalid, "writer options: " + message + " at '" + pointer + "'",
                    std::move(ctx));
    };
    const json j = json::parse(options.compact(), nullptr, false);
    if (!j.is_object()) {
        return invalid("", "expected an object");
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "ld" || !it->is_object()) {
            return invalid("/" + it.key(), it.key() != "ld" ? "unknown option" : "expected an object");
        }
        for (auto o = it->begin(); o != it->end(); ++o) {
            const std::string pointer = "/ld/" + o.key();
            if (o.key() == "dialect") {
                bool found = false;
                for (const ld::WriteDialect d :
                     {ld::WriteDialect::PcLayout, ld::WriteDialect::M1, ld::WriteDialect::CSeries,
                      ld::WriteDialect::SimExporter, ld::WriteDialect::ConverterLegacy}) {
                    if (o->is_string() && o->get<std::string>() == ld::writeDialectName(d)) {
                        out.dialect = d;
                        found = true;
                    }
                }
                if (!found) {
                    return invalid(pointer, "unknown dialect");
                }
            } else if (o.key() == "dataAlignment") {
                if (!o->is_number_integer() || o->get<std::int64_t>() < 1 || o->get<std::int64_t>() > 4096) {
                    return invalid(pointer, "expected an integer from 1 to 4096");
                }
                out.dataAlignment = static_cast<std::uint32_t>(o->get<std::int64_t>());
            } else if (o.key() == "verbatim") {
                if (!o->is_boolean()) {
                    return invalid(pointer, "expected a boolean");
                }
                out.verbatim = o->get<bool>();
            } else {
                return invalid(pointer, "unknown option");
            }
        }
    }
    return out;
}

void note(ImportLog& notes, Level level, std::string_view code, std::string message,
          std::string channel = {}) {
    ImportLogEntry::Context ctx;
    if (!channel.empty()) {
        ctx.channel = std::move(channel);
    }
    notes.add(level, std::string(code), std::move(message), std::move(ctx));
}

// The stored value whose scaled value is `v` (the inverse of the reader's afterMul scaling), unrounded.
double inverseScaled(double v, const LdEncoding& e) {
    const double s = e.scale == 0 ? 1.0 : static_cast<double>(e.scale);
    const double m = e.mul == 0 ? 1.0 : static_cast<double>(e.mul);
    const double p = std::pow(10.0, static_cast<double>(std::abs(static_cast<int>(e.dec))));
    const double rawFactor = e.dec < 0 ? p : 1.0;
    const double den = e.dec >= 0 ? s * p : s;
    return (v - static_cast<double>(e.shift)) * den / (rawFactor * m);
}

bool isFloatPair(std::uint16_t code, std::uint16_t width) {
    return (code == ld::layout::kTypeFloat && (width == 2 || width == 4 || width == 8)) ||
           (code == ld::layout::kTypeDouble && width == 8);
}

std::uint8_t interpByteOf(store::Interp i) {
    switch (i) {
    case store::Interp::Closest:
        return ld::layout::kInterpClosest;
    case store::Interp::Previous:
        return ld::layout::kInterpPrevious;
    case store::Interp::Next:
        return ld::layout::kInterpNext;
    case store::Interp::Linear:
        break;
    }
    return ld::layout::kInterpLinear;
}

// Values (in the column's unit) and validity of a whole column, chunk by chunk.
struct ColumnValues {
    std::vector<double> values;
    std::vector<bool> valid;
};

Result<ColumnValues> readColumn(const store::Column& col, const CancelToken& cancel) {
    ColumnValues out;
    const store::SampleIdx n = col.size();
    out.values.resize(static_cast<std::size_t>(n));
    out.valid.resize(static_cast<std::size_t>(n));
    for (store::SampleIdx c = 0; c < col.chunkCount(); ++c) {
        const store::SampleIdx first = store::chunkFirstSample(c);
        const store::SampleIdx last = std::min(n, first + store::SampleIdx{store::kChunkSamples}) - 1;
        auto h = col.require(first, last, Priority::Background, cancel);
        if (!h) {
            return Failure{h.error()};
        }
        for (store::SampleIdx i = first; i <= last; ++i) {
            out.valid[static_cast<std::size_t>(i)] = h->validAt(i);
            out.values[static_cast<std::size_t>(i)] = h->at(i);
        }
    }
    return out;
}

std::vector<ld::LdInterval> intervalsOf(const std::vector<bool>& valid) {
    std::vector<ld::LdInterval> out;
    std::uint64_t i = 0;
    const std::uint64_t n = valid.size();
    while (i < n) {
        if (valid[static_cast<std::size_t>(i)]) {
            ++i;
            continue;
        }
        const std::uint64_t b = i;
        while (i < n && !valid[static_cast<std::size_t>(i)]) {
            ++i;
        }
        out.push_back({b, i});
    }
    return out;
}

class LdExportWriter final : public IWriter {
public:
    explicit LdExportWriter(const units::UnitRegistry* units) : units_(units) {}

    std::string id() const override { return std::string(kLdImporterId); }
    std::vector<std::string> extensions() const override { return {".ld"}; }
    std::optional<model::OpaqueJson> optionsSchema() const override {
        auto j = model::OpaqueJson::parse(kWriterOptionsSchema);
        return j ? std::optional<model::OpaqueJson>(std::move(j).value()) : std::nullopt;
    }

    Result<WriteReport> write(const std::filesystem::path& path, const ExportView& view,
                              const WriterOptions& options, const Progress& progress,
                              CancelToken cancel) override {
        if (!view.source) {
            return fail(ErrorCode::Invalid, "motec.ld export: no source");
        }
        auto opts = parseWriteOptions(options.options);
        if (!opts) {
            return Failure{opts.error()};
        }
        const model::DataSource& src = *view.source;
        const model::DataLayer* layer = src.layer(view.layer);
        if (layer == nullptr) {
            return fail(ErrorCode::NotFound, "motec.ld export: the source has no layer '" + view.layer + "'");
        }
        if (view.resampleRate) {
            return fail(ErrorCode::Unsupported, "motec.ld export: resampling is not implemented in M0");
        }
        if (view.start > 0 || view.end < layer->duration) {
            return fail(ErrorCode::Unsupported, "motec.ld export: M0 writes whole layers only");
        }
        std::vector<const model::Channel*> channels;
        if (view.channels.empty()) {
            for (const model::Channel& c : layer->channels) {
                channels.push_back(&c);
            }
        } else {
            for (const model::ChannelId& id : view.channels) {
                const model::Channel* c = layer->findById(id);
                if (c == nullptr) {
                    return fail(ErrorCode::NotFound,
                                "motec.ld export: no channel " + id.str() + " in layer '" + view.layer + "'");
                }
                channels.push_back(c);
            }
        }

        ImportLog notes;
        const bool explicitDialect = opts->dialect.has_value();
        std::map<const ld::LdFile*, ld::LdWriteInput> originInputs; // each origin file read once
        std::shared_ptr<const ld::LdFile> mainOrigin;
        ld::LdWriteInput input;
        for (std::size_t k = 0; k < channels.size(); ++k) {
            if (cancel.cancelled()) {
                return fail(ErrorCode::Cancelled, "motec.ld export cancelled");
            }
            const model::Channel& ch = *channels[k];
            auto wc = channelFor(ch, *opts, originInputs, mainOrigin, notes, cancel);
            if (!wc) {
                return Failure{wc.error()};
            }
            if (std::optional<ld::LdWriteChannel>& written = *wc) {
                input.channels.push_back(std::move(*written));
            }
            if (progress) {
                progress(0.9 * static_cast<double>(k + 1) / static_cast<double>(channels.size()));
            }
        }

        // Device events, enumeration tables, details and the header.
        const ld::LdWriteInput* origin = mainOrigin ? &originInputs.at(mainOrigin.get()) : nullptr;
        if (view.includeMarkers) {
            if (origin != nullptr) {
                input.events = origin->events;
            } else {
                for (const model::Marker& m : layer->markers) {
                    if (m.cls == model::MarkerClass::DeviceEvent && m.prov == model::Provenance::Logged &&
                        m.flags <= 0xFFFFu) {
                        input.events.push_back(
                            {static_cast<std::uint16_t>(m.flags), static_cast<double>(m.loggedT())});
                    }
                }
            }
        }
        if (origin != nullptr) {
            input.enumTables = origin->enumTables;
        } else {
            for (const model::EnumTable& t : layer->enumTables) {
                if (t.id.v > 0xFFFFu) {
                    note(notes, Level::Warning, ld::code::kDetailSkipped,
                         "enumeration table " + std::to_string(t.id.v) +
                             " has an id above 65535; not written");
                    continue;
                }
                ld::LdEnumTable lt;
                lt.index = static_cast<std::uint16_t>(t.id.v);
                for (const model::EnumEntry& e : t.entries) {
                    ld::LdEnumEntry le;
                    le.value = static_cast<std::int16_t>(std::clamp(e.value, -32768, 32767));
                    le.label = e.label;
                    le.severity = e.severity;
                    le.extended = e.severity != 0;
                    lt.entries.push_back(std::move(le));
                }
                input.enumTables.push_back(std::move(lt));
            }
        }

        ld::LdWriteOptions wopt;
        const bool verbatimHeader = opts->verbatim && !explicitDialect && view.includeDetails;
        if (mainOrigin && opts->verbatim && !explicitDialect) {
            wopt = ld::LdWriter::optionsFromFile(*mainOrigin);
            input.details = origin->details;
        } else {
            wopt.dialect = opts->dialect.value_or(ld::WriteDialect::PcLayout);
        }
        if (verbatimHeader && wopt.rawHeader.empty() && src.prov.importerId == kLdImporterId) {
            if (const auto parts = unpackRawHeader(src.prov.rawHeader)) {
                wopt.rawHeader = parts->header;
                wopt.rawEvent = parts->event;
                wopt.rawVenue = parts->venue;
                wopt.rawVehicle = parts->vehicle;
                wopt.rawWeather = parts->weather;
            }
        }
        if (!verbatimHeader) {
            wopt.rawHeader.clear();
            wopt.rawEvent.clear();
            wopt.rawVenue.clear();
            wopt.rawVehicle.clear();
            wopt.rawWeather.clear();
        }
        if (wopt.rawHeader.empty()) {
            input.details =
                view.includeDetails ? writeDetailsOf(src.details, units_, notes) : ld::LdWriteDetails{};
        }
        if (const auto& alignment = opts->dataAlignment) {
            wopt.dataAlignment = *alignment;
        }

        WriteReport report;
        auto bytes = ld::LdWriter::write(path, input, wopt, &notes);
        if (!bytes) {
            return Failure{bytes.error()};
        }
        report.bytes += *bytes;
        report.files.push_back(path);

        // The companion (05 section 5 item 7).
        const ld::LdxDocument ldx = ldxFromSource(src, *layer, view.includeMarkers, view.includeDetails);
        std::filesystem::path ldxPath = path;
        ldxPath.replace_extension(".ldx");
        if (!ldxWorthWriting(ldx, src)) {
            // What is written is the .ld alone: a companion already beside it (an earlier export to the same
            // name, or another file's) would be read with it and lend it its markers and details (D-217).
            std::error_code ec;
            if (std::filesystem::exists(ldxPath, ec) && !std::filesystem::remove(ldxPath, ec) && ec) {
                ErrorContext ctx;
                ctx.path = ldxPath;
                ctx.importer = std::string(kLdImporterId);
                return fail(ErrorCode::Io,
                            "motec.ld export: cannot remove the earlier companion: " + ec.message(),
                            std::move(ctx));
            }
        } else {
            const std::string text = ld::writeLdx(ldx);
            if (auto r = ld::writeLdxFile(ldxPath, ldx); !r) {
                return Failure{r.error()};
            }
            report.bytes += text.size();
            report.files.push_back(ldxPath);
        }
        if (progress) {
            progress(1.0);
        }
        report.notes = notes.snapshot();
        return report;
    }

private:
    // nullopt: the channel is skipped (with a note).
    Result<std::optional<ld::LdWriteChannel>>
    channelFor(const model::Channel& ch, const WriteOpts& opts,
               std::map<const ld::LdFile*, ld::LdWriteInput>& originInputs,
               std::shared_ptr<const ld::LdFile>& mainOrigin, ImportLog& notes,
               const CancelToken& cancel) const {
        const std::string& name = ch.desc.name;
        const auto* series = std::get_if<store::UniformSeries>(&ch.series);
        if (series == nullptr) {
            note(notes, Level::Warning, ld::code::kChannelSkipped,
                 "channel '" + name + "' is timestamped; .ld channels are uniformly sampled", name);
            return std::optional<ld::LdWriteChannel>{};
        }
        const Rate rate = series->rate.normalised();
        if (!rate.valid() || rate.den != 1 || rate.num > 0xFFFFu) {
            note(notes, Level::Warning, ld::code::kChannelSkipped,
                 "channel '" + name + "' has the rate " + series->rate.toString() +
                     "; .ld needs a whole number of Hz up to 65535",
                 name);
            return std::optional<ld::LdWriteChannel>{};
        }
        if (ch.desc.kind == store::SampleKind::Text) {
            note(notes, Level::Warning, ld::code::kChannelSkipped, "channel '" + name + "' is text", name);
            return std::optional<ld::LdWriteChannel>{};
        }
        const auto* enc = dynamic_cast<const LdSourceEncoding*>(ch.desc.sourceEncoding.get());

        // Verbatim: the stored bytes of the file the channel came from.
        if (enc != nullptr && enc->origin() && enc->origin()->file && opts.verbatim) {
            const LdSourceEncoding::Origin& o = *enc->origin();
            const ld::LdChannel& oc = o.file->channels()[o.channel];
            if (rate == oc.rate() && static_cast<std::uint64_t>(series->values.size()) == oc.sampleCount) {
                auto it = originInputs.find(o.file.get());
                if (it == originInputs.end()) {
                    auto in = ld::LdWriter::inputFromFile(*o.file);
                    if (!in) {
                        return Failure{in.error()};
                    }
                    it = originInputs.emplace(o.file.get(), std::move(in).value()).first;
                }
                if (!mainOrigin) {
                    mainOrigin = o.file;
                }
                if (o.channel < it->second.channels.size()) {
                    return std::optional<ld::LdWriteChannel>(it->second.channels[o.channel]);
                }
            }
        }

        // Re-quantised or default encoding: read the column.
        auto values = readColumn(series->values, cancel);
        if (!values) {
            return Failure{values.error()};
        }
        LdEncoding e;
        bool haveEncoding = false;
        if (enc != nullptr) {
            e = enc->encoding();
            haveEncoding = true;
        } else {
            switch (series->values.dtype()) {
            case store::Dtype::F64:
                e.typeCode = ld::layout::kTypeDouble;
                e.width = 8;
                break;
            case store::Dtype::I32: {
                bool fits16 = true;
                for (std::size_t i = 0; i < values->values.size() && fits16; ++i) {
                    if (values->valid[i]) {
                        const double v = values->values[i];
                        fits16 = v >= -32768.0 && v <= 32767.0;
                    }
                }
                e.typeCode = fits16 ? ld::layout::kTypeInt3 : ld::layout::kTypeInt5;
                e.width = fits16 ? 2 : 4;
                break;
            }
            case store::Dtype::F32:
            case store::Dtype::I64:
                e.typeCode = ld::layout::kTypeFloat;
                e.width = 4;
                break;
            }
            e.dpsByte = static_cast<std::uint8_t>(std::clamp(ch.desc.dps, 0, 255));
            e.interpByte = interpByteOf(ch.desc.interp);
            if (ch.desc.enums && ch.desc.enums->v <= 0xFFFFu) {
                e.enumRef = static_cast<std::uint16_t>(ch.desc.enums->v);
            }
            if (units_ != nullptr && ch.desc.unit != units::kNoUnit && units_->contains(ch.desc.unit)) {
                e.unit = units_->unit(ch.desc.unit).symbol;
            }
        }
        e.rateHz = static_cast<std::uint16_t>(rate.num);

        // Base unit -> the encoding's unit.
        units::Converter fromBase;
        if (haveEncoding && ch.desc.unit != units::kNoUnit && ch.desc.quantity != units::kUnitless) {
            std::optional<units::UnitId> u;
            if (units_ != nullptr && !e.unit.empty()) {
                if (const auto m = units_->match(e.unit, ch.desc.quantity)) {
                    u = m->unit;
                }
            }
            if (!u) {
                return fail(ErrorCode::Invalid, "motec.ld export: channel '" + name +
                                                    "' holds base-unit values and "
                                                    "its unit '" +
                                                    e.unit + "' cannot be resolved" +
                                                    (units_ == nullptr ? " (no unit registry)" : ""));
            }
            fromBase = units_->fromBase(*u);
        }

        ld::LdWriteChannel w;
        w.name = ch.desc.loggedName.empty() ? name : ch.desc.loggedName;
        w.shortName = e.shortName;
        w.unit = e.unit;
        w.typeCode = e.typeCode;
        w.width = e.width;
        w.rateHz = e.rateHz;
        w.shift = e.shift;
        w.mul = e.mul;
        w.scale = e.scale;
        w.dec = e.dec;
        w.dpsByte = e.dpsByte;
        w.interpByte = e.interpByte;
        w.displayRange = e.displayRange;
        w.counter = e.counter;
        w.enumRef = e.enumRef;
        w.invalid = intervalsOf(values->valid);
        const bool isFloat = isFloatPair(e.typeCode, e.width);
        const std::size_t n = values->values.size();
        if (isFloat) {
            w.rawFloats.resize(n);
        } else {
            w.rawInts.resize(n);
        }
        std::int64_t held = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const bool ok = values->valid[i];
            const double vendor = fromBase(values->values[i]);
            if (isFloat) {
                w.rawFloats[i] = ok ? inverseScaled(vendor, e) : std::numeric_limits<double>::quiet_NaN();
            } else {
                if (ok) {
                    held = ld::quantiseRaw(vendor, e.shift, e.mul, e.scale, e.dec, ld::ShiftOrder::AfterMul);
                }
                w.rawInts[i] = held; // an integer gap holds the previous valid value, as loggers do
            }
        }
        return std::optional<ld::LdWriteChannel>(std::move(w));
    }

    const units::UnitRegistry* units_;
};

} // namespace

std::unique_ptr<IWriter> makeLdWriter(const units::UnitRegistry* units) {
    return std::make_unique<LdExportWriter>(units);
}

} // namespace esses::formats
