// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "inspect.hpp"

#include "cli_common.hpp"

#include <esses/analysis/beacon_pair_rule.hpp>
#include <esses/analysis/corrected_speed.hpp>
#include <esses/formats/byte_source.hpp>
#include <esses/store/series.hpp>
#include <esses/store/types.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <utility>

namespace esses::cli {

using nlohmann::json;

std::string describe(const Error& error) {
    return error.toString();
}

Result<formats::ImportOptions> makeImportOptionsJson(std::string_view importerId, const json& options) {
    formats::ImportOptions o;
    o.importerId = std::string(importerId);
    if (!options.is_null()) {
        if (!options.is_object()) {
            return fail(ErrorCode::Invalid, "importer options must be a JSON object");
        }
        auto text = canonicalText(options);
        if (!text) {
            return Failure{text.error()};
        }
        auto opaque = model::OpaqueJson::parse(*text);
        if (!opaque) {
            return Failure{opaque.error()};
        }
        o.options = std::move(opaque).value();
    }
    return o;
}

Result<formats::ImportOptions> makeImportOptions(std::string_view importerId, std::string_view optionsJson) {
    if (optionsJson.empty()) {
        return makeImportOptionsJson(importerId, json());
    }
    auto j = parseJson(optionsJson);
    if (!j) {
        return Failure{j.error()};
    }
    return makeImportOptionsJson(importerId, *j);
}

Result<ProbeOutcome> probeFile(AppContext& ctx, const std::filesystem::path& file,
                               const formats::ImportOptions& options) {
    const auto t0 = std::chrono::steady_clock::now();
    auto reg = ctx.registry();
    if (!reg) {
        return Failure{reg.error()};
    }
    ProbeOutcome out;
    const formats::IImporter* importer = nullptr;
    if (!options.importerId.empty()) {
        importer = (*reg)->find(options.importerId);
        if (importer == nullptr) {
            return fail(ErrorCode::NotFound, "no importer '" + options.importerId + "'");
        }
    } else {
        auto ranked = (*reg)->sniff(file);
        if (!ranked) {
            return Failure{ranked.error()};
        }
        if (ranked->empty()) {
            ErrorContext c;
            c.path = file;
            return fail(ErrorCode::NotFound, "no importer recognises " + u8(file), std::move(c));
        }
        importer = ranked->front().first;
        out.confidence = ranked->front().second.confidence;
        out.reason = ranked->front().second.reason;
    }
    auto source = formats::openByteSource(file);
    if (!source) {
        return Failure{source.error()};
    }
    out.importerId = std::string(importer->id());
    if (options.options.isNull()) {
        auto p = importer->probe(**source);
        if (!p) {
            return Failure{p.error()};
        }
        out.probe = std::move(p).value();
    } else {
        formats::ImportOptions o = options;
        o.importerId = std::string(importer->id());
        o.importerVersion = std::string(importer->version());
        auto reader = importer->open(*source, o);
        if (!reader) {
            return Failure{reader.error()};
        }
        out.probe = (*reader)->probe();
    }
    out.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

Result<formats::OpenedSource> openFile(AppContext& ctx, const std::filesystem::path& file,
                                       const formats::ImportOptions& options, bool withUnits,
                                       bool loadSidecar, bool rangePhase2) {
    auto reg = ctx.registry();
    if (!reg) {
        return Failure{reg.error()};
    }
    auto rules = ctx.rules();
    if (!rules) {
        return Failure{rules.error()};
    }
    formats::OpenContext context = ctx.openContext(withUnits, loadSidecar);
    context.rangeRebuild = (*rules)->rebuildFunction();
    auto opened = (*reg)->open(file, options, context);
    if (!opened || !rangePhase2) {
        return opened;
    }
    if (auto annotated = runRangePhase2(ctx, *opened); !annotated) {
        return Failure{annotated.error()};
    }
    return opened;
}

Result<void> runRangePhase2(AppContext& ctx, formats::OpenedSource& opened) {
    auto rules = ctx.rules();
    if (!rules) {
        return Failure{rules.error()};
    }
    const units::UnitRegistry* units = nullptr;
    if (!ctx.options().noUnits) {
        if (auto u = ctx.units(); u) {
            units = *u;
        }
    }
    const auto hasLaps = [&](const model::DataLayer& layer) {
        for (const auto& rule : (*rules)->rules()) {
            const auto* beacons = dynamic_cast<const analysis::BeaconPairRule*>(rule.get());
            if (beacons != nullptr && beacons->appliesTo(layer.name) &&
                layer.ranges.find(beacons->settings().produces) != nullptr) {
                return true;
            }
        }
        return false;
    };
    std::shared_ptr<model::DataSource> next;
    for (std::size_t i = 0; i < opened.source->layers.size(); ++i) {
        const model::DataLayer& layer = opened.source->layers[i];
        if (!hasLaps(layer)) {
            continue;
        }
        std::optional<store::Series> speed;
        auto corr = analysis::CorrectedSpeed::compute(analysis::CorrectedSpeedSettings{}, layer, units, {});
        if (corr) {
            store::ColumnOptions co;
            co.cache = &ctx.cache();
            speed = analysis::toUniformSeries(corr->speedMps, co, "cli:corr-speed");
        } else if (corr.error().code != ErrorCode::NotFound && corr.error().code != ErrorCode::Unsupported) {
            return Failure{corr.error()};
        }
        auto tree = (*rules)->runPhase2(*opened.source, layer.name, speed, {});
        if (!tree) {
            return Failure{tree.error()};
        }
        if (!next) {
            next = std::make_shared<model::DataSource>(*opened.source);
        }
        next->layers[i].ranges = std::move(tree).value();
    }
    if (next) {
        opened.source = std::move(next);
    }
    return ok();
}

json markerJson(const model::Marker& m) {
    json j = json::object();
    j["id"] = m.id.str();
    j["class"] = std::string(model::markerClassName(m.cls));
    j["group"] = m.group;
    j["tUs"] = m.t;
    j["name"] = m.name;
    j["comment"] = m.comment;
    if (m.flags != 0) {
        j["flags"] = m.flags;
    }
    j["prov"] = std::string(model::provenanceName(m.prov));
    return j;
}

namespace {

json rangeJson(const model::Range& r) {
    json j = json::object();
    j["id"] = r.id.str();
    j["kind"] = std::string(model::rangeKindName(r.kind));
    j["lapClass"] = std::string(model::lapClassName(r.lapClass));
    j["number"] = r.number;
    j["name"] = r.name;
    j["abbrev"] = r.abbrev;
    j["startUs"] = r.start;
    j["endUs"] = r.end;
    if (!r.startMarker.isNil()) {
        j["startMarker"] = r.startMarker.str();
    }
    if (!r.endMarker.isNil()) {
        j["endMarker"] = r.endMarker.str();
    }
    j["trusted"] = r.trusted;
    j["enabled"] = r.enabled;
    if (r.stint != 0) {
        j["stint"] = r.stint;
    }
    if (r.pitStop) {
        j["pitStop"] = true;
    }
    if (r.offsetUs != 0) {
        j["offsetUs"] = r.offsetUs;
    }
    if (r.offsetM != 0.0) {
        j["offsetM"] = r.offsetM;
    }
    return j;
}

json groupJson(const model::RangeGroup& g) {
    json j = json::object();
    j["path"] = g.path;
    j["name"] = g.name;
    json ranges = json::array();
    for (const model::Range& r : g.ranges) {
        ranges.push_back(rangeJson(r));
    }
    j["ranges"] = std::move(ranges);
    json children = json::array();
    for (const model::RangeGroup& c : g.children) {
        children.push_back(groupJson(c));
    }
    j["children"] = std::move(children);
    j["supportsTrackGeneration"] = g.supportsTrackGeneration;
    j["supportsReporting"] = g.supportsReporting;
    return j;
}

std::string rateText(const std::optional<Rate>& rate) {
    if (!rate) {
        return "timestamped";
    }
    const Rate n = rate->normalised();
    if (n.den == 1) {
        return std::to_string(n.num) + " Hz";
    }
    return n.toString();
}

const formats::ChannelInfo* probeChannel(const formats::ProbeResult& probe, std::string_view layer,
                                         std::size_t index, std::string_view loggedName) {
    for (const formats::LayerInfo& l : probe.layers) {
        if (l.name != layer) {
            continue;
        }
        if (index < l.channels.size() && l.channels[index].loggedName == loggedName) {
            return &l.channels[index];
        }
        for (const formats::ChannelInfo& c : l.channels) {
            if (c.loggedName == loggedName) {
                return &c;
            }
        }
    }
    return nullptr;
}

std::string fileNameOf(const std::filesystem::path& p) {
    return u8(p.filename());
}

std::string fixed(double v, int decimals) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(decimals) << v;
    return os.str();
}

std::string utcText(const formats::LayerInfo& l) {
    if (!l.utc) {
        return "";
    }
    model::DateTime dt;
    dt.unixUs = l.utc->unixUs;
    std::string s = dt.iso();
    if (l.utcAssumedLocal) {
        s += " (local time, zone unknown)";
    }
    return s;
}

int printError(std::ostream& err, const std::filesystem::path& file, const Error& e) {
    err << "esses-cli: " << u8(file) << ": " << describe(e) << "\n";
    return 1;
}

} // namespace

json lapsJson(const model::DataSource& source) {
    json doc = json::object();
    doc["$schema"] = "urn:esses:laps-1";
    doc["schemaVersion"] = 1;
    json layers = json::object();
    for (const model::DataLayer& l : source.layers) {
        json lj = json::object();
        lj["durationUs"] = l.duration;
        json markers = json::array();
        for (const model::Marker& m : l.markers) {
            if (m.enabled) {
                markers.push_back(markerJson(m));
            }
        }
        lj["markers"] = std::move(markers);
        json groups = json::array();
        for (const model::RangeGroup& g : l.ranges.roots) {
            groups.push_back(groupJson(g));
        }
        lj["groups"] = std::move(groups);
        layers[l.name] = std::move(lj);
    }
    doc["layers"] = std::move(layers);
    return doc;
}

json channelsJson(const formats::OpenedSource& opened, const units::UnitRegistry* units) {
    const model::DataSource& src = *opened.source;
    json doc = json::object();
    doc["file"] = fileNameOf(src.prov.path);
    doc["importerId"] = src.prov.importerId;
    json layers = json::object();
    for (const model::DataLayer& l : src.layers) {
        json rows = json::array();
        for (std::size_t i = 0; i < l.channels.size(); ++i) {
            const model::Channel& c = l.channels[i];
            json r = json::object();
            r["name"] = c.desc.name;
            r["loggedName"] = c.desc.loggedName;
            std::string unit;
            std::string quantity;
            if (units != nullptr && c.desc.unit != units::kNoUnit && units->contains(c.desc.unit)) {
                unit = units->unit(c.desc.unit).symbol;
            }
            if (units != nullptr && c.desc.quantity != units::kUnitless &&
                units->containsQuantity(c.desc.quantity)) {
                quantity = units->quantity(c.desc.quantity).name;
            }
            r["unit"] = unit;
            r["quantity"] = quantity;
            if (const formats::ChannelInfo* info = probeChannel(opened.probe, l.name, i, c.desc.loggedName)) {
                r["vendorUnit"] = info->vendorUnit;
                r["vendorDtype"] = info->vendorDtype;
            }
            if (c.desc.rate) {
                const Rate n = c.desc.rate->normalised();
                r["rate"] = json{{"num", n.num}, {"den", n.den}};
            }
            r["dtype"] = std::string(store::dtypeName(c.desc.dtype));
            r["kind"] = std::string(store::sampleKindName(c.desc.kind));
            r["interp"] = std::string(store::interpName(c.desc.interp));
            r["samples"] = store::seriesValues(c.series).size();
            r["dps"] = c.desc.dps;
            if (c.desc.enums) {
                r["enumTable"] = c.desc.enums->v;
            }
            if (!c.desc.statusText.empty()) {
                r["status"] = c.desc.statusText;
            }
            rows.push_back(std::move(r));
        }
        layers[l.name] = std::move(rows);
    }
    doc["layers"] = std::move(layers);
    return doc;
}

int runInfo(AppContext& ctx, const InspectOptions& o, std::ostream& out, std::ostream& err) {
    auto opts = makeImportOptions(o.importerId, o.optionsJson);
    if (!opts) {
        return printError(err, o.file, opts.error());
    }
    auto probed = probeFile(ctx, o.file, *opts);
    if (!probed) {
        return printError(err, o.file, probed.error());
    }
    if (o.timing) {
        err << "probe: " << fixed(probed->millis, 3) << " ms\n";
    }
    const formats::ProbeResult& p = probed->probe;
    if (o.json) {
        auto text = p.canonicalJson();
        if (!text) {
            return printError(err, o.file, text.error());
        }
        out << *text;
        return 0;
    }
    out << u8(o.file) << "\n";
    out << "  importer    " << p.importerId << " " << p.importerVersion;
    if (probed->confidence > 0) {
        out << " (confidence " << probed->confidence << ": " << probed->reason << ")";
    }
    out << "\n  dialect     " << (p.dialect.empty() ? "(none)" : p.dialect) << "\n";
    if (!p.companions.empty()) {
        out << "  companions ";
        for (const auto& c : p.companions) {
            out << " " << u8(c);
        }
        out << "\n";
    }
    for (const model::Detail& d : p.details) {
        out << "  detail      " << d.id << " = " << model::detailValueText(d.value) << "\n";
    }
    for (const formats::LayerInfo& l : p.layers) {
        out << "  layer " << l.name << ": " << l.channels.size() << " channels, "
            << fixed(static_cast<double>(l.durationUs) / 1e6, 3) << " s, " << l.markers.size() << " markers";
        if (const std::string utc = utcText(l); !utc.empty()) {
            out << ", starts " << utc;
        }
        out << "\n";
        for (const formats::ChannelInfo& c : l.channels) {
            std::string rate = "timestamped";
            if (const auto* r = std::get_if<Rate>(&c.timing)) {
                rate = rateText(*r);
            }
            out << "    " << std::setw(4) << c.index << "  " << c.loggedName << " [" << c.vendorUnit << "] "
                << rate << " " << c.vendorDtype << "->" << store::dtypeName(c.dtype) << " "
                << store::sampleKindName(c.kind) << " " << c.sampleCount << " samples\n";
        }
        for (const formats::MarkerInfo& m : l.markers) {
            out << "    marker " << model::markerClassName(m.cls) << " '" << m.name << "' at "
                << fixed(static_cast<double>(m.tUs) / 1e6, 6) << " s\n";
        }
    }
    for (const formats::ImportLogEntry& w : p.warnings) {
        out << "  " << formats::importLogLevelName(w.level) << " " << w.code << ": " << w.message;
        if (w.ctx.repeats > 1) {
            out << " (x" << w.ctx.repeats << ")";
        }
        out << "\n";
    }
    return 0;
}

int runChannels(AppContext& ctx, const InspectOptions& o, std::ostream& out, std::ostream& err) {
    auto opts = makeImportOptions(o.importerId, o.optionsJson);
    if (!opts) {
        return printError(err, o.file, opts.error());
    }
    auto units = ctx.units();
    if (!units) {
        return printError(err, o.file, units.error());
    }
    auto opened = openFile(ctx, o.file, *opts, true, !o.noSidecar, false);
    if (!opened) {
        return printError(err, o.file, opened.error());
    }
    const json doc = channelsJson(*opened, *units);
    if (o.json) {
        auto text = canonicalText(doc);
        if (!text) {
            return printError(err, o.file, text.error());
        }
        out << *text;
        return 0;
    }
    for (auto it = doc["layers"].begin(); it != doc["layers"].end(); ++it) {
        out << "layer " << it.key() << " (" << it->size() << " channels)\n";
        out << "  " << std::left << std::setw(36) << "name" << std::setw(12) << "unit" << std::setw(12)
            << "rate" << std::setw(6) << "dtype" << std::setw(10) << "kind" << std::right << std::setw(12)
            << "samples"
            << "\n";
        for (const json& r : *it) {
            std::string unit = r["unit"].get<std::string>();
            if (unit.empty() && r.contains("vendorUnit")) {
                unit = r["vendorUnit"].get<std::string>();
            }
            std::string rate = "timestamped";
            if (r.contains("rate")) {
                const auto num = r["rate"]["num"].get<std::int64_t>();
                const auto den = r["rate"]["den"].get<std::int64_t>();
                rate = den == 1 ? std::to_string(num) + " Hz"
                                : std::to_string(num) + "/" + std::to_string(den) + " Hz";
            }
            out << "  " << std::left << std::setw(36) << r["name"].get<std::string>() << std::setw(12) << unit
                << std::setw(12) << rate << std::setw(6) << r["dtype"].get<std::string>() << std::setw(10)
                << r["kind"].get<std::string>() << std::right << std::setw(12)
                << r["samples"].get<std::uint64_t>() << "\n";
        }
    }
    return 0;
}

int runLaps(AppContext& ctx, const InspectOptions& o, std::ostream& out, std::ostream& err) {
    auto opts = makeImportOptions(o.importerId, o.optionsJson);
    if (!opts) {
        return printError(err, o.file, opts.error());
    }
    auto units = ctx.units();
    if (!units) {
        return printError(err, o.file, units.error());
    }
    auto opened = openFile(ctx, o.file, *opts, true, !o.noSidecar);
    if (!opened) {
        return printError(err, o.file, opened.error());
    }
    const model::DataSource& src = *opened->source;
    if (o.json) {
        auto text = canonicalText(lapsJson(src));
        if (!text) {
            return printError(err, o.file, text.error());
        }
        out << *text;
        return 0;
    }
    for (const model::DataLayer& l : src.layers) {
        out << "layer " << l.name << ", " << fixed(static_cast<double>(l.duration) / 1e6, 3) << " s\n";
        out << "  markers:\n";
        std::size_t shown = 0;
        for (const model::Marker& m : l.markers) {
            if (!m.enabled) {
                continue;
            }
            ++shown;
            out << "    " << std::left << std::setw(12) << model::markerClassName(m.cls) << std::right
                << std::setw(14) << fixed(static_cast<double>(m.t) / 1e6, 6) << " s  " << m.name;
            if (!m.group.empty()) {
                out << "  [" << m.group << "]";
            }
            out << "  (" << model::provenanceName(m.prov) << ")\n";
        }
        if (shown == 0) {
            out << "    (none)\n";
        }
        l.ranges.forEach([&](const model::RangeGroup& g) {
            out << "  group " << g.path << ":\n";
            for (const model::Range& r : g.ranges) {
                out << "    " << model::rangeKindName(r.kind) << " " << r.number << " '" << r.name << "' "
                    << fixed(static_cast<double>(r.start) / 1e6, 6) << " - "
                    << fixed(static_cast<double>(r.end) / 1e6, 6) << " s ("
                    << fixed(static_cast<double>(r.duration()) / 1e6, 3) << " s)";
                if (r.lapClass != model::LapClass::None) {
                    out << " " << model::lapClassName(r.lapClass);
                }
                out << (r.trusted ? "" : " untrusted");
                if (r.stint != 0) {
                    out << " stint " << r.stint;
                }
                out << (r.pitStop ? " pit-stop" : "") << "\n";
            }
        });
    }
    return 0;
}

} // namespace esses::cli
