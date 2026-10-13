// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "export.hpp"

#include "cli_common.hpp"
#include "inspect.hpp"
#include "maths_cli.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/registry.hpp>
#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/maths/gps_corrections.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/maths/math_module_io.hpp>
#include <esses/maths/math_selection.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>
#include <esses/project/aliases.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/project/mappings.hpp>
#include <esses/project/project_file.hpp>

#include <fast_float/fast_float.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace esses::cli {

namespace {

using nlohmann::json;
namespace m = esses::maths;

// The documents of a project folder the export reads (02 section 6.1); absent files are empty documents.
struct ProjectDocs {
    std::filesystem::path folder;
    project::ProjectFile project;
    std::vector<m::MathModule> modules; // in project order
    model::MappingTable mappings;
    model::AliasTable aliases;
    std::map<std::string, formats::UnitMappingProfile, std::less<>> unitMaps;
    project::ChannelOverrides overrides;
    m::MathsCompat compat;
    model::OpaqueJson correctedSpeedSettings; // null: the module's defaults
    bool correctedSpeedEnabled = true;
};

bool isFile(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

Result<ProjectDocs> loadProject(const std::filesystem::path& where, const units::UnitRegistry& units,
                                std::ostream& err) {
    ProjectDocs d;
    d.folder = where.filename() == "project.json" ? where.parent_path() : where;
    std::vector<std::string> log;
    Result<project::ProjectFile> pf = project::ProjectFile::load(d.folder / "project.json", log);
    if (!pf) {
        return Failure{pf.error()};
    }
    d.project = std::move(pf).value();
    for (const std::string& id : d.project.mathsOrder) {
        const std::filesystem::path file = d.folder / "maths" / p8(id + ".maths.json");
        if (!isFile(file)) {
            err << "esses-cli export: maths module '" << id << "' has no file " << u8(file) << "\n";
            continue;
        }
        Result<m::MathModule> module = m::MathModuleIo::load(file, units, log);
        if (!module) {
            return fail(ErrorCode::Invalid, u8(file) + ": " + module.error().toString());
        }
        module->origin = m::MathModule::Origin::Global;
        d.modules.push_back(std::move(module).value());
    }
    if (const std::filesystem::path f = d.folder / "channels" / "mappings.json"; isFile(f)) {
        Result<project::Mappings> doc = project::Mappings::load(f, log);
        if (!doc) {
            return fail(ErrorCode::Invalid, u8(f) + ": " + doc.error().toString());
        }
        d.mappings = doc->toTable(units, log);
    }
    if (const std::filesystem::path f = d.folder / "channels" / "aliases.json"; isFile(f)) {
        Result<project::Aliases> doc = project::Aliases::load(f, log);
        if (!doc) {
            return fail(ErrorCode::Invalid, u8(f) + ": " + doc.error().toString());
        }
        d.aliases = doc->table;
    }
    if (const std::filesystem::path f = d.folder / "channels" / "overrides.json"; isFile(f)) {
        Result<project::ChannelOverrides> doc = project::ChannelOverrides::load(f, log);
        if (!doc) {
            return fail(ErrorCode::Invalid, u8(f) + ": " + doc.error().toString());
        }
        d.overrides = std::move(doc).value();
    }
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(d.folder / "channels" / "unitmaps", ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json") {
            continue;
        }
        Result<formats::UnitMappingProfile> map = formats::UnitMappingProfile::load(e.path(), log);
        if (!map) {
            err << "esses-cli export: " << u8(e.path()) << ": " << map.error().toString() << " (skipped)\n";
            continue;
        }
        d.unitMaps.insert_or_assign(u8(e.path().stem()), std::move(map).value());
    }
    // options.maths.compat (02 section 6.2) and builtinMaths of the corrected speed module.
    const json options =
        json::parse(d.project.options.isObject() ? d.project.options.compact() : "{}", nullptr, false);
    if (options.is_object()) {
        const auto maths = options.find("maths");
        if (maths != options.end() && maths->is_object()) {
            const auto compat = maths->find("compat");
            if (compat != maths->end() && compat->is_object()) {
                const auto minus = compat->find("unaryMinusAbovePower");
                if (minus != compat->end() && minus->is_boolean()) {
                    d.compat.unaryMinusAbovePower = minus->get<bool>();
                }
            }
        }
    }
    if (const auto it = d.project.builtinMaths.find(m::CorrectedSpeedDistanceModule::kId);
        it != d.project.builtinMaths.end()) {
        d.correctedSpeedEnabled = it->second.enabled;
        if (!it->second.settings.isNull()) {
            Result<model::OpaqueJson> s = model::OpaqueJson::parse(it->second.settings.compact());
            if (s) {
                d.correctedSpeedSettings = std::move(s).value();
            }
        }
    }
    for (const std::string& line : log) {
        err << "esses-cli export: " << line << "\n";
    }
    return d;
}

// "Outings:Laps/3": the enabled range numbered 3, else the third range of the group.
const model::Range* findRange(const model::DataLayer& layer, const std::string& spec, std::string& path) {
    const std::size_t slash = spec.rfind('/');
    if (slash == std::string::npos) {
        return nullptr;
    }
    path = spec.substr(0, slash);
    const std::string n = spec.substr(slash + 1);
    int number = 0;
    const auto r = std::from_chars(n.data(), n.data() + n.size(), number);
    if (r.ec != std::errc() || r.ptr != n.data() + n.size()) {
        return nullptr;
    }
    const model::RangeGroup* group = layer.ranges.find(path);
    if (group == nullptr) {
        return nullptr;
    }
    for (const model::Range& range : group->ranges) {
        if (range.enabled && range.number == number) {
            return &range;
        }
    }
    if (number >= 1 && static_cast<std::size_t>(number) <= group->ranges.size()) {
        return &group->ranges[static_cast<std::size_t>(number - 1)];
    }
    return nullptr;
}

// Both GPS inputs of the GPS corrections' default settings are logged in `layer` (as MathsService decides).

std::optional<Rate> parseHz(const std::string& text) {
    double hz = 0.0;
    const auto r = fast_float::from_chars(text.data(), text.data() + text.size(), hz);
    if (r.ec != std::errc() || r.ptr != text.data() + text.size() || !(hz > 0.0) || hz > 1e6) {
        return std::nullopt;
    }
    if (std::trunc(hz) == hz) {
        return Rate{static_cast<std::uint32_t>(hz), 1};
    }
    return Rate{static_cast<std::uint32_t>(std::llround(hz * 1000.0)), 1000}.normalised();
}

} // namespace

int runExport(AppContext& ctx, const ExportOptions& o, std::ostream& out, std::ostream& err) {
    if (o.inputs.empty() || o.inputs.size() > 2) {
        err << "esses-cli export: give [<project>] <log>\n";
        return 2;
    }
    if (!o.output) {
        err << "esses-cli export: -o <file> is required\n";
        return 2;
    }
    if (o.format != "csv" && o.format != formats::kLegacyCsvWriterId) {
        err << "esses-cli export: --format is csv or " << formats::kLegacyCsvWriterId << "\n";
        return 2;
    }
    Result<const units::UnitRegistry*> unitsResult = ctx.units();
    if (!unitsResult || *unitsResult == nullptr) {
        err << "esses-cli export: the unit table is needed\n";
        return 2;
    }
    const units::UnitRegistry& units = **unitsResult;
    // MathGraph shares the registry; the context owns it for the whole command.
    const std::shared_ptr<const units::UnitRegistry> sharedUnits(&units, [](const units::UnitRegistry*) {});

    std::optional<ProjectDocs> docs;
    const std::filesystem::path log = o.inputs.back();
    if (o.inputs.size() == 2) {
        Result<ProjectDocs> d = loadProject(o.inputs.front(), units, err);
        if (!d) {
            err << "esses-cli export: " << describe(d.error()) << "\n";
            return 2;
        }
        docs = std::move(d).value();
    }

    // Open as the application does for this project (05 section 2): mappings and unit maps at step 4.
    Result<formats::ImportOptions> importOptions = makeImportOptions(o.importerId, o.optionsJson);
    if (!importOptions) {
        err << "esses-cli export: " << describe(importOptions.error()) << "\n";
        return 2;
    }
    Result<formats::ImporterRegistry*> reg = ctx.registry();
    Result<const analysis::RuleRunner*> rules = ctx.rules();
    if (!reg || !rules) {
        err << "esses-cli export: " << describe(!reg ? reg.error() : rules.error()) << "\n";
        return 2;
    }
    formats::OpenContext context = ctx.openContext(true, !o.noSidecar);
    context.rangeRebuild = (*rules)->rebuildFunction();
    if (docs) {
        context.mappings = docs->mappings;
        context.unitMaps = docs->unitMaps;
    }
    Result<formats::OpenedSource> opened = (*reg)->open(log, *importOptions, context);
    if (!opened) {
        err << "esses-cli export: " << describe(opened.error()) << "\n";
        return 1;
    }
    if (Result<void> phase2 = runRangePhase2(ctx, *opened); !phase2) {
        err << "esses-cli export: " << describe(phase2.error()) << "\n";
        return 1;
    }
    const std::shared_ptr<const model::DataSource> source = opened->source;
    const model::DataLayer* layer = source->layer(o.layer);
    if (layer == nullptr) {
        err << "esses-cli export: no layer '" << o.layer << "'\n";
        return 2;
    }
    const model::Range* range = nullptr;
    std::string rangePath;
    if (!o.range.empty()) {
        range = findRange(*layer, o.range, rangePath);
        if (range == nullptr) {
            err << "esses-cli export: no range '" << o.range << "' in layer " << layer->name
                << " (a range is <group path>/<number>, e.g. \"Outings:Laps/3\")\n";
            return 1;
        }
    }

    // The maths of the source (03 sections 4.3 and 9): built-in modules first, then local, global, as the
    // application's MathsService registers them.
    TaskPool pool; // this process's pool (01 section 5): one task per maths node
    m::MathGraph graph(source, std::string(model::canonicalLayerName(layer->name)), sharedUnits,
                       docs ? docs->compat : m::MathsCompat{}, &pool, &ctx.cache());
    if (docs) {
        static_cast<void>(graph.setResolverTables(docs->mappings, docs->aliases));
    }
    m::CorrectedSpeedDistanceModule corrected;
    m::GpsCorrectionsModule gps;
    if (Result<std::filesystem::path> schemas = ctx.schemaDir(); schemas) {
        static_cast<void>(corrected.loadSchema(*schemas));
        static_cast<void>(gps.loadSchema(*schemas));
    }
    std::vector<std::pair<const m::IBuiltinModule*, model::OpaqueJson>> builtins{
        {&corrected, docs ? docs->correctedSpeedSettings : model::OpaqueJson{}}};
    if (loggedGpsInputs(gps, *layer)) {
        builtins.emplace_back(&gps, model::OpaqueJson{});
    }
    for (const auto& [module, settings] : builtins) {
        m::Diagnostics diags;
        if (Result<std::vector<m::ChannelKey>> keys = graph.defineBuiltin(*module, settings, diags); !keys) {
            err << "esses-cli export: built-in module " << module->id() << ": " << describe(keys.error())
                << "\n";
        }
    }
    if (docs && !docs->correctedSpeedEnabled) {
        static_cast<void>(graph.setModuleEnabled(m::CorrectedSpeedDistanceModule::kId, false));
    }
    std::optional<m::MathModule> local;
    if (const std::optional<JsonDoc>& doc = source->sidecar.layer(layer->name).localMaths; doc) {
        std::vector<std::string> mlog;
        Result<m::MathModule> parsed = m::MathModuleIo::fromJson(*doc, units, mlog);
        if (parsed) {
            local = std::move(parsed).value();
            local->origin = m::MathModule::Origin::Local;
            if (local->id.empty()) {
                local->id = std::string(m::MathModule::kLocalId);
            }
        } else {
            err << "esses-cli export: the sidecar's local maths cannot be read: " << describe(parsed.error())
                << "\n";
        }
    }
    const std::span<const m::MathModule> globals =
        docs ? std::span<const m::MathModule>(docs->modules) : std::span<const m::MathModule>();
    static_cast<void>(graph.setModules(globals, local ? &*local : nullptr, nullptr));
    if (range != nullptr) {
        m::MathSelection sel;
        sel.main.ref.src = source->id;
        sel.main.ref.layer = layer->name;
        sel.main.ref.groupPath = rangePath;
        sel.main.ref.range = range->id;
        sel.main.start = range->start;
        sel.main.end = range->end;
        sel.main.offsetUs = range->offsetUs;
        sel.main.offsetM = range->offsetM;
        sel.main.stretch = range->stretch.value_or(1.0);
        Hash128Stream h;
        h.updatePart("esses.cli.export.selection-1");
        h.updatePart(rangePath);
        h.updateU64(static_cast<std::uint64_t>(range->start));
        h.updateU64(static_cast<std::uint64_t>(range->end));
        sel.key = h.digest();
        static_cast<void>(graph.setSelection(sel));
    }

    // The channels: a maths output (the head of its chain) before a logged channel of the same name.
    struct Pick {
        std::string name;
        std::optional<m::ChannelKey> maths;
        const model::Channel* logged = nullptr;
    };
    std::vector<Pick> picks;
    if (o.channels.empty()) {
        for (const model::Channel& c : layer->channels) {
            if (!graph.find(c.desc.name)) {
                picks.push_back(Pick{c.desc.name, std::nullopt, &c});
            }
        }
        for (const m::ChannelKey& key : graph.outputs()) {
            const m::MathStatus s = graph.status(key);
            if (s.state == m::MathState::Error || s.state == m::MathState::Disabled) {
                continue;
            }
            const std::optional<model::ChannelDescriptor> d = graph.descriptor(key);
            picks.push_back(Pick{d ? d->name : key.nameCf, key, nullptr});
        }
    } else {
        for (const std::string& name : o.channels) {
            if (const std::optional<m::ChannelKey> key = graph.find(name)) {
                picks.push_back(Pick{name, key, nullptr});
            } else if (const model::Channel* c = layer->find(name); c != nullptr) {
                picks.push_back(Pick{c->desc.name, std::nullopt, c});
            } else if (const model::Channel* lc = layer->findLogged(name); lc != nullptr) {
                picks.push_back(Pick{lc->desc.name, std::nullopt, lc});
            } else {
                err << "esses-cli export: no channel '" << name << "' in " << u8(log.filename())
                    << " (logged or maths)\n";
                return 1;
            }
        }
    }

    // Evaluate the maths outputs (one task per node, in parallel; this thread waits).
    std::vector<std::pair<std::size_t, Future<m::ColumnHandle>>> pending;
    for (std::size_t i = 0; i < picks.size(); ++i) {
        if (picks[i].maths) {
            pending.emplace_back(
                i, graph.require(*picks[i].maths, m::SampleRange::all(), Priority::Visible, {}));
        }
    }
    auto exported = std::make_shared<model::DataSource>(*source);
    model::DataLayer* outLayer = nullptr;
    for (model::DataLayer& l : exported->layers) {
        if (l.name == layer->name) {
            outLayer = &l;
        }
    }
    if (outLayer == nullptr) {
        err << "esses-cli export: internal error: the layer copy is missing\n";
        return 2;
    }
    std::vector<model::ChannelId> ids(picks.size());
    int failures = 0;
    for (auto& [i, future] : pending) {
        static_cast<void>(future.get()); // every evaluation finishes before the statuses are read
    }
    static_cast<void>(graph.applyReadyCompletions());
    for (auto& [i, future] : pending) {
        const Result<m::ColumnHandle>& r = future.get();
        if (!r) {
            // The node's Error summary (the definition's, or its first failing input's), else the failure.
            const m::MathStatus s = graph.status(*picks[i].maths);
            err << "esses-cli export: '" << picks[i].name << "' cannot be evaluated: "
                << (s.state == m::MathState::Error && !s.summary.empty() ? s.summary : describe(r.error()))
                << "\n";
            ++failures;
            continue;
        }
        model::Channel ch;
        ch.desc = r->descriptor;
        ch.series = r->series;
        ids[i] = ch.desc.id;
        if (model::Channel* shadowed = outLayer->findMutable(ch.desc.name); shadowed != nullptr) {
            ch.desc.loggedName = shadowed->desc.loggedName; // a filter or scale item over a logged channel
            *shadowed = std::move(ch);
            outLayer->rebuildIndex();
        } else {
            outLayer->addChannel(std::move(ch));
        }
    }
    if (failures > 0) {
        return 1;
    }
    for (std::size_t i = 0; i < picks.size(); ++i) {
        if (!picks[i].maths) {
            ids[i] = picks[i].logged->desc.id;
        }
    }

    // Display units: a maths item's display unit (else its declared result unit), then the project's
    // overrides, then --units.
    json csvUnits = json::object();
    const auto itemUnit = [&](const std::string& name) -> std::string {
        const std::string cf = caseFold(name);
        std::vector<const m::MathModule*> order; // local maths first: it shadows the global modules
        if (local) {
            order.push_back(&*local);
        }
        for (const m::MathModule& g : globals) {
            order.push_back(&g);
        }
        for (const m::MathModule* module : order) {
            for (const m::MathItem& item : module->items) {
                if (!item.enabled || caseFold(item.name) != cf || item.kind != m::ItemKind::Expression) {
                    continue;
                }
                const units::UnitId u =
                    item.display.unit != units::kNoUnit ? item.display.unit : item.resultUnit;
                return u != units::kNoUnit && units.contains(u) ? units.unit(u).symbol : std::string();
            }
        }
        return {};
    };
    const auto unitFor = [&](const std::string& channel, const std::string& unit) {
        if (units.find(unit)) {
            csvUnits[channel] = unit;
        } else {
            err << "esses-cli export: unknown unit '" << unit << "' for '" << channel
                << "' (base unit kept)\n";
        }
    };
    for (const Pick& p : picks) {
        if (p.maths) {
            if (const std::string u = itemUnit(p.name); !u.empty()) {
                csvUnits[p.name] = u;
            }
        }
    }
    if (docs) {
        for (const Pick& p : picks) {
            if (const project::ChannelOverrides::Entry* e = docs->overrides.find(p.name);
                e != nullptr && e->displayUnit && !e->displayUnit->empty()) {
                unitFor(p.name, *e->displayUnit);
            }
        }
    }
    for (const std::string& spec : o.units) {
        const std::size_t eq = spec.find('=');
        if (eq == std::string::npos || eq == 0) {
            err << "esses-cli export: --units takes <channel>=<unit>, not '" << spec << "'\n";
            return 2;
        }
        unitFor(spec.substr(0, eq), spec.substr(eq + 1));
    }

    formats::ExportView view;
    view.source = exported;
    view.layer = layer->name;
    view.channels = ids;
    if (range != nullptr) {
        view.start = range->start;
        view.end = range->end;
    }
    if (!o.rate.empty()) {
        const std::optional<Rate> rate = parseHz(o.rate);
        if (!rate) {
            err << "esses-cli export: --rate is a number of Hz, not '" << o.rate << "'\n";
            return 2;
        }
        view.resampleRate = rate;
    }
    formats::WriterOptions wopts;
    if (!csvUnits.empty()) {
        Result<model::OpaqueJson> j = model::OpaqueJson::parse(json{{"csv", {{"units", csvUnits}}}}.dump());
        if (j) {
            wopts.options = std::move(j).value();
        }
    }
    std::unique_ptr<formats::IWriter> writer =
        o.format == "csv" ? formats::makeGenericCsvWriter(&units) : formats::makeLegacyCsvWriter(&units);
    Result<formats::WriteReport> report =
        writer->write(*o.output, view, wopts, formats::Progress{}, CancelToken{});
    if (!report) {
        err << "esses-cli export: " << describe(report.error()) << "\n";
        return 1;
    }
    for (const formats::ImportLogEntry& e : report->notes) {
        err << "esses-cli export: " << e.code << ": " << e.message
            << (e.ctx.channel ? " (" + *e.ctx.channel + ")" : std::string()) << "\n";
    }
    std::size_t mathsCount = 0;
    json channels = json::array();
    for (const Pick& p : picks) {
        mathsCount += p.maths ? 1 : 0;
        channels.push_back(json{{"name", p.name}, {"kind", p.maths ? "maths" : "logged"}});
    }
    if (o.json) {
        json doc{{"output", u8(*o.output)},
                 {"format", o.format},
                 {"bytes", report->bytes},
                 {"channels", std::move(channels)},
                 {"range", o.range}};
        Result<std::string> text = canonicalText(doc);
        if (!text) {
            err << "esses-cli export: " << describe(text.error()) << "\n";
            return 2;
        }
        out << *text;
    } else {
        out << u8(*o.output) << ": " << picks.size() << " channels (" << mathsCount << " maths), "
            << report->bytes << " bytes\n";
    }
    return 0;
}

} // namespace esses::cli
