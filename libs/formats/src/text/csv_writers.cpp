// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The CSV writers (05 section 6 "Writer"): every exported channel resampled onto one grid (ExportView::
// resampleRate, by default the highest uniform rate in the view capped at 1 kHz) with the channel's
// interpolation through store::Resampler, Time from 0 at the start of the range.
//   motec.csv  the observed skeleton of the legacy export: eleven key/value metadata lines (05 section 6 item
//   1),
//              two blank lines, names row, units row, two blank lines, data; every field double-quoted, CRLF,
//              '.' decimal; invalid samples as "".
//   csv        a plain table: names row, units row, data; LF; fields quoted only when they need it.
// Values are written in the channel's stored (base) unit unless options {"csv": {"units": {"<channel>":
// "<unit>"}}} name a display unit the registry converts to. Written through AtomicFile (05 section 1.3 rule
// 10).
#include <esses/base/atomic_file.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/formats/csv.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/source.hpp>
#include <esses/store/resampler.hpp>
#include <esses/units/unit_registry.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace esses::formats {
namespace {

using json = nlohmann::json;
using Level = ImportLogEntry::Level;

constexpr std::string_view kOptionsSchema = R"({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "CSV writer options",
  "type": "object",
  "properties": {
    "csv": {
      "type": "object",
      "properties": {
        "units": {"type": "object", "additionalProperties": {"type": "string"}}
      },
      "additionalProperties": false
    }
  },
  "additionalProperties": false
})";

constexpr SampleIdx kBlock = 65536;

std::string shortest(double v, bool single) {
    char buf[64];
    std::to_chars_result r{};
    if (single) {
        r = std::to_chars(buf, buf + sizeof buf, static_cast<float>(v));
    } else {
        r = std::to_chars(buf, buf + sizeof buf, v);
    }
    return {buf, static_cast<std::size_t>(r.ptr - buf)};
}

std::string general(double v, int precision) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::general, precision);
    return {buf, static_cast<std::size_t>(r.ptr - buf)};
}

// Locale-independent fixed notation (snprintf would follow LC_NUMERIC).
std::string fixed(double v, int decimals) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, decimals);
    return {buf, static_cast<std::size_t>(r.ptr - buf)};
}

struct Out {
    const model::Channel* channel = nullptr;
    units::Converter fromBase; // base -> written unit
    std::string unitLabel;
    bool single = true; // F32 channel: shortest float text
    std::vector<double> values;
};

class CsvWriter final : public IWriter {
public:
    CsvWriter(bool legacy, const units::UnitRegistry* units) : legacy_(legacy), units_(units) {}

    std::string id() const override {
        return std::string(legacy_ ? kLegacyCsvImporterId : kGenericCsvWriterId);
    }
    std::vector<std::string> extensions() const override { return {".csv"}; }
    std::optional<model::OpaqueJson> optionsSchema() const override {
        auto j = model::OpaqueJson::parse(kOptionsSchema);
        return j ? std::optional<model::OpaqueJson>(std::move(j).value()) : std::nullopt;
    }

    Result<WriteReport> write(const std::filesystem::path& path, const ExportView& view,
                              const WriterOptions& options, const Progress& progress,
                              CancelToken cancel) override {
        if (!view.source) {
            return fail(ErrorCode::Invalid, "csv export: no source");
        }
        const model::DataSource& src = *view.source;
        const model::DataLayer* layer = src.layer(view.layer);
        if (layer == nullptr) {
            return fail(ErrorCode::NotFound, "csv export: no layer '" + view.layer + "'");
        }
        WriteReport report;
        std::map<std::string, std::string> displayUnits;
        if (!options.options.isNull()) {
            const json o = jsonio::fromDoc(options.options);
            if (!o.is_object()) {
                return fail(ErrorCode::Invalid, "csv export: options must be an object");
            }
            for (const auto& [key, value] : o.items()) {
                if (key != "csv" || !value.is_object()) {
                    return fail(ErrorCode::Invalid, "csv export: unknown option '" + key + "'");
                }
                for (const auto& [k2, v2] : value.items()) {
                    if (k2 != "units" || !v2.is_object()) {
                        return fail(ErrorCode::Invalid, "csv export: unknown option 'csv." + k2 + "'");
                    }
                    for (const auto& [name, unit] : v2.items()) {
                        if (!unit.is_string()) {
                            return fail(ErrorCode::Invalid, "csv export: csv.units values are unit symbols");
                        }
                        displayUnits[name] = unit.get<std::string>();
                    }
                }
            }
        }

        // Channels in view order (layer order when the view names none).
        std::vector<Out> outs;
        const auto addChannel = [&](const model::Channel& ch) {
            Out o;
            o.channel = &ch;
            o.single = store::seriesValues(ch.series).dtype() != store::Dtype::F64;
            if (units_ != nullptr && units_->contains(ch.desc.unit)) {
                o.unitLabel = std::string(units_->unit(ch.desc.unit).label());
                if (const auto it = displayUnits.find(ch.desc.name); it != displayUnits.end()) {
                    const auto target = units_->find(it->second, ch.desc.quantity);
                    if (target) {
                        auto conv = units_->converter(ch.desc.unit, *target);
                        if (conv) {
                            o.fromBase = *conv;
                            o.unitLabel = std::string(units_->unit(*target).label());
                        }
                    } else {
                        ImportLogEntry e;
                        e.level = Level::Warning;
                        e.code = "csv.unit_not_applied";
                        e.message =
                            "the display unit is not one of the channel's quantity; base units are written";
                        e.ctx.channel = ch.desc.name;
                        e.ctx.detail = it->second;
                        report.notes.push_back(std::move(e));
                    }
                }
            }
            outs.push_back(std::move(o));
        };
        if (view.channels.empty()) {
            for (const model::Channel& ch : layer->channels) {
                addChannel(ch);
            }
        } else {
            for (const model::ChannelId& id : view.channels) {
                if (const model::Channel* ch = layer->findById(id)) {
                    addChannel(*ch);
                } else {
                    ImportLogEntry e;
                    e.level = Level::Warning;
                    e.code = "csv.channel_missing";
                    e.message = "a channel of the export is not in the layer";
                    report.notes.push_back(std::move(e));
                }
            }
        }

        // The grid.
        Rate rate{0, 1};
        if (view.resampleRate && view.resampleRate->valid()) {
            rate = view.resampleRate->normalised();
        } else {
            for (const Out& o : outs) {
                if (const auto* u = std::get_if<store::UniformSeries>(&o.channel->series)) {
                    if (!rate.valid() || rate < u->rate) {
                        rate = u->rate;
                    }
                }
            }
            if (!rate.valid()) {
                rate = Rate{100, 1};
            }
            if (Rate{1000, 1} < rate) {
                rate = Rate{1000, 1};
            }
        }
        const Micros start = std::max<Micros>(view.start, 0);
        const Micros end = std::min<Micros>(view.end, layer->duration);
        if (end <= start) {
            return fail(ErrorCode::Invalid, "csv export: the range is empty");
        }
        const SampleIdx n = rate.indexAt(end - 1, start) + 1;
        // Three decimals when the period is a whole number of milliseconds, else six.
        const int timeDecimals = (std::uint64_t{rate.den} * 1000) % rate.num == 0 ? 3 : 6;

        std::string text;
        const std::string eol = legacy_ ? "\r\n" : "\n";
        const auto field = [&](std::string_view s) {
            if (legacy_) {
                text += '"';
                for (const char c : s) {
                    text += c;
                    if (c == '"') {
                        text += '"';
                    }
                }
                text += '"';
                return;
            }
            if (s.find_first_of(",\"\r\n") != std::string_view::npos) {
                text += '"';
                for (const char c : s) {
                    text += c;
                    if (c == '"') {
                        text += '"';
                    }
                }
                text += '"';
            } else {
                text += s;
            }
        };
        const auto row = [&](const std::vector<std::string>& cells) {
            for (std::size_t i = 0; i < cells.size(); ++i) {
                if (i > 0) {
                    text += ',';
                }
                field(cells[i]);
            }
            text += eol;
        };

        std::optional<std::size_t> corrDist; // its first and last values fill the distance metadata
        double startDist = std::numeric_limits<double>::quiet_NaN();
        double endDist = std::numeric_limits<double>::quiet_NaN();
        for (std::size_t c = 0; c < outs.size(); ++c) {
            if (outs[c].channel->desc.name == "Corr Dist") {
                corrDist = c;
            }
        }
        std::vector<std::string> names{"Time"};
        std::vector<std::string> unitsRow{"s"};
        for (const Out& o : outs) {
            names.push_back(o.channel->desc.name);
            unitsRow.push_back(o.unitLabel);
        }
        row(names);
        row(unitsRow);
        if (legacy_) {
            text += eol;
            text += eol;
        }

        for (SampleIdx first = 0; first < n; first += kBlock) {
            if (cancel.cancelled()) {
                return fail(ErrorCode::Cancelled, "csv export cancelled");
            }
            const SampleIdx count = std::min<SampleIdx>(kBlock, n - first);
            for (Out& o : outs) {
                o.values.assign(static_cast<std::size_t>(count), std::numeric_limits<double>::quiet_NaN());
                const store::Series& series = o.channel->series;
                auto pins =
                    store::Resampler::pinFor(series, rate, start, first, count, Priority::Background, cancel);
                if (!pins) {
                    return Failure{pins.error()};
                }
                const store::ChunkHandle* times =
                    std::holds_alternative<store::TimestampedSeries>(series) ? &pins->times : nullptr;
                auto blockView = store::Resampler::block(series, rate, start, first, count,
                                                         o.channel->desc.interp, pins->values, times);
                if (!blockView) {
                    continue; // outside the series: every sample invalid
                }
                for (SampleIdx k = 0; k < count; ++k) {
                    if (blockView->valid(k)) {
                        o.values[static_cast<std::size_t>(k)] = o.fromBase(blockView->f64(k));
                    }
                }
            }
            if (corrDist && count > 0) {
                if (first == 0) {
                    startDist = outs[*corrDist].values.front();
                }
                endDist = outs[*corrDist].values.back();
            }
            std::vector<std::string> cells(outs.size() + 1);
            for (SampleIdx k = 0; k < count; ++k) {
                const Micros t = rate.timeAt(first + k, start) - start;
                cells[0] = fixed(static_cast<double>(t) / 1e6, timeDecimals);
                for (std::size_t c = 0; c < outs.size(); ++c) {
                    const double v = outs[c].values[static_cast<std::size_t>(k)];
                    if (std::isnan(v) || std::isinf(v)) {
                        cells[c + 1].clear();
                    } else if (outs[c].fromBase.identity()) {
                        cells[c + 1] = shortest(v, outs[c].single);
                    } else {
                        cells[c + 1] = general(v, outs[c].single ? 7 : 15);
                    }
                }
                row(cells);
            }
            if (progress) {
                progress(static_cast<double>(first + count) / static_cast<double>(n));
            }
        }
        if (legacy_) {
            std::string head;
            writeLegacyHeader(head, src, rate, start, end, startDist, endDist);
            text.insert(0, head);
        }
        if (auto r = AtomicFile::write(path, std::string_view(text)); !r) {
            return Failure{r.error()};
        }
        report.bytes = text.size();
        report.files.push_back(path);
        return report;
    }

private:
    static void writeLegacyHeader(std::string& text, const model::DataSource& src, Rate rate, Micros start,
                                  Micros end, double startDist, double endDist) {
        const auto detail = [&](std::string_view id) -> std::string {
            const model::Detail* d = model::findDetail(src.details, id);
            return d != nullptr ? model::detailValueText(d->value) : std::string();
        };
        const auto q = [](std::string_view s) {
            std::string out = "\"";
            for (const char c : s) {
                out += c;
                if (c == '"') {
                    out += '"';
                }
            }
            return out + "\"";
        };
        const auto num = [](double v, int decimals) {
            return std::isfinite(v) ? fixed(v, decimals) : std::string("0");
        };
        const std::string eol = "\r\n";
        const double s0 = static_cast<double>(start) / 1e6;
        const double s1 = static_cast<double>(end) / 1e6;
        text += q("Format") + "," + q(kLegacyCsvFormatName) + ",,," + q("Workbook") + "," +
                q(detail("Export Workbook")) + eol;
        text += q("Venue") + "," + q(detail("Venue")) + ",,," + q("Worksheet") + "," +
                q(detail("Export Worksheet")) + eol;
        text += q("Vehicle") + "," + q(detail("Vehicle Id")) + ",,," + q("Vehicle Desc") + "," +
                q(detail("Vehicle Desc")) + eol;
        text += q("Driver") + "," + q(detail("Driver")) + ",,," + q("Engine ID") + "," +
                q(detail("Engine Id")) + eol;
        text += q("Device") + "," + q(detail("Device Type")) + eol;
        text += q("Comment") + "," + q(detail("Comment")) + ",,," + q("Session") + "," +
                q(detail("Session")) + eol;
        text += q("Log Date") + "," + q(detail("Log Date")) + ",,," + q("Origin Time") + "," +
                q(fixed(0.0, 3)) + "," + q("s") + eol;
        text += q("Log Time") + "," + q(detail("Log Time")) + ",,," + q("Start Time") + "," +
                q(fixed(s0, 3)) + "," + q("s") + eol;
        text += q("Sample Rate") + "," + q(fixed(rate.hz(), 3)) + "," + q("Hz") + ",," + q("End Time") + "," +
                q(fixed(s1, 3)) + "," + q("s") + eol;
        text += q("Duration") + "," + q(fixed(s1 - s0, 3)) + "," + q("s") + ",," + q("Start Distance") + "," +
                q(num(startDist, 0)) + "," + q("m") + eol;
        text += q("Range") + "," + q(detail("Export Range")) + ",,," + q("End Distance") + "," +
                q(num(endDist, 0)) + "," + q("m") + eol;
        text += eol;
        text += eol;
    }

    bool legacy_ = true;
    const units::UnitRegistry* units_ = nullptr;
};

} // namespace

std::unique_ptr<IWriter> makeLegacyCsvWriter(const units::UnitRegistry* units) {
    return std::make_unique<CsvWriter>(true, units);
}

std::unique_ptr<IWriter> makeGenericCsvWriter(const units::UnitRegistry* units) {
    return std::make_unique<CsvWriter>(false, units);
}

} // namespace esses::formats
