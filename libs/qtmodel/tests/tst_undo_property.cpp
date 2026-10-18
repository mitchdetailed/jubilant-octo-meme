// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-09 (D-032, 02 section 7, 06 section 1 rule 4): 30 randomised sequences of undoable edits, undos and
// redos over every document kind the stack covers (workbooks and the workbook list, worksheets, components
// with paste/delete/merged resizes, overrides, aliases, mappings, unit maps, maths modules, project options
// and the sidecar of an open source: details, trust (through DataSelectionModel::setTrusted), offsets, beacon
// inserts and deletes, marker edits, restore logged laps, annotations, local maths), interleaved with
// view-state operations (selection, cursor, zoom, F9 domain, filters, the lap-stretch toggle) that must never
// touch the stack. After every step the documents equal a replay: a fresh session (and a plain sidecar
// document) to which the edits still applied, in stack order, are re-applied through the document APIs
// directly. Undoing everything restores the initial documents and redoing everything the final ones. The seed
// comes from ESSES_TEST_SEED (default fixed) and is printed with every failure.

#include "qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/lap_stretch_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtmodel/workspace_view_model.hpp>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>
#include <algorithm>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)

namespace {

constexpr int kSequences = 30;
constexpr int kStepsPerSequence = 40;
constexpr std::string_view kLayer = "Normal";
constexpr std::string_view kLaps = "Outings:Laps";

using Rng = std::mt19937_64;

std::uint64_t testSeed() {
    bool ok = false;
    const qulonglong v = qEnvironmentVariable("ESSES_TEST_SEED").toULongLong(&ok);
    return ok ? static_cast<std::uint64_t>(v) : std::uint64_t{20261012};
}

Paths pathsFor(const std::filesystem::path& config) {
    PathOverrides o;
    o.configDir = config;
    o.dataDir = repoRoot() / "data";
    o.cacheDir = config / "cache";
    o.logDir = config / "logs";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve failed: %s", p.error().toString().c_str());
    }
    return std::move(p).value();
}

int pick(Rng& rng, int n) {
    return n <= 0 ? 0 : static_cast<int>(rng() % static_cast<std::uint64_t>(n));
}

Uuid randomId(Rng& rng) {
    return uuidFromHash(Hash128{rng(), rng()}, 4);
}

// The project file without the view state the stack never carries (lastSelection, dataWindow, the MRU) and
// without the members other documents own (workbooks, mathsOrder).
std::string projectText(const ProjectSession& s) {
    project::ProjectFile p = s.projectFile();
    p.lastSelection = {};
    p.dataWindow = {};
    p.recentSources.clear();
    p.workbooks.clear();
    p.mathsOrder.clear();
    p.dirty = false;
    // The lap-stretch toggle is view state too (options.lapStretch.enabled, written without the stack).
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(p.options.compact())).object();
    QJsonObject stretch = o.value(QStringLiteral("lapStretch")).toObject();
    stretch.remove(QStringLiteral("enabled"));
    o.insert(QStringLiteral("lapStretch"), stretch);
    p.options = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()).value();
    return p.toJson();
}

// The edits of a sidecar: its edit time and its source block (filled by the loader on the first commit,
// D-015) are metadata, not edits.
std::string sidecarText(model::SidecarDocument doc) {
    doc.edited = {};
    doc.sourcePathRel.clear();
    doc.sourceSize = 0;
    doc.sourceHash = {};
    doc.sourceContentHash.reset();
    doc.importerId.clear();
    doc.importerVersion.clear();
    doc.companions.clear();
    doc.pruneEmptyLayers();
    Result<std::string> text = doc.toJson();
    return text ? *text : std::string("<non-finite>");
}

// Every document the stack covers, as text.
std::vector<std::string> documentsOf(const ProjectSession& s, const model::SidecarDocument& sidecar) {
    std::vector<std::string> out;
    for (const auto& [rel, text] : s.documentTexts()) {
        if (rel != "project.json") {
            out.push_back(rel + "\n" + text);
        }
    }
    out.push_back("project.json\n" + projectText(s));
    out.push_back("sidecar\n" + sidecarText(sidecar));
    return out;
}

// The same documents as values, compared member-wise (fast); documentsOf() names a difference.
struct Snapshot {
    WorkbookSet workbooks;
    project::ProjectFile project;
    project::ChannelOverrides overrides;
    model::AliasTable aliases;
    project::Mappings mappings;
    std::vector<std::pair<std::string, std::string>> unitMaps;
    std::vector<maths::MathModule> modules;
    model::SidecarDocument sidecar;

    bool operator==(const Snapshot& o) const {
        const auto sameAliases = [](const model::AliasTable& a, const model::AliasTable& b) {
            return std::equal(a.rows.begin(), a.rows.end(), b.rows.begin(), b.rows.end(),
                              [](const model::AliasRow& x, const model::AliasRow& y) {
                                  return x.name == y.name && x.colourIndex == y.colourIndex &&
                                         x.dps == y.dps &&
                                         std::equal(x.candidates.begin(), x.candidates.end(),
                                                    y.candidates.begin(), y.candidates.end(),
                                                    [](const auto& c, const auto& d) {
                                                        return c.name == d.name && c.enabled == d.enabled;
                                                    });
                              });
        };
        return workbooks == o.workbooks && project == o.project && overrides == o.overrides &&
               sameAliases(aliases, o.aliases) && mappings == o.mappings && unitMaps == o.unitMaps &&
               modules == o.modules && sidecar == o.sidecar;
    }
};

Snapshot snapshotOf(const ProjectSession& s, model::SidecarDocument sidecar) {
    Snapshot out;
    out.workbooks = s.workbookSet();
    out.project = s.projectFile();
    out.project.lastSelection = {};
    out.project.dataWindow = {};
    out.project.recentSources.clear();
    out.project.workbooks.clear();
    out.project.mathsOrder.clear();
    out.project.dirty = false;
    // options.lapStretch.enabled is the toolbar toggle (view state): compared without it.
    QJsonObject o =
        QJsonDocument::fromJson(QByteArray::fromStdString(out.project.options.compact())).object();
    QJsonObject stretch = o.value(QStringLiteral("lapStretch")).toObject();
    stretch.remove(QStringLiteral("enabled"));
    o.insert(QStringLiteral("lapStretch"), stretch);
    out.project.options =
        JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()).value();
    out.overrides = s.overrides();
    out.aliases = s.aliases().table;
    out.mappings = s.mappings();
    for (const auto& [stem, map] : s.unitMaps()) {
        Result<std::string> text = map.toJson();
        out.unitMaps.emplace_back(stem, text ? *text : std::string());
    }
    out.modules = s.mathsModules();
    sidecar.edited = {};
    sidecar.sourcePathRel.clear();
    sidecar.sourceSize = 0;
    sidecar.sourceHash = {};
    sidecar.sourceContentHash.reset();
    sidecar.importerId.clear();
    sidecar.importerVersion.clear();
    sidecar.companions.clear();
    sidecar.pruneEmptyLayers();
    out.sidecar = std::move(sidecar);
    return out;
}

// One edit: `push` pushes the command on the live stack (its "after" computed from the live state); `replay`
// applies the same edit through the document APIs to the replay state.
struct Op {
    std::string name;
    std::function<void()> push;
    std::function<void(ProjectSession&, model::SidecarDocument&)> replay;
};

maths::MathModule module(const std::string& id, int items) {
    maths::MathModule m;
    m.id = id;
    for (int i = 0; i < items; ++i) {
        maths::MathItem item;
        item.name = id + " item " + std::to_string(i);
        item.expr = "'Throttle' * " + std::to_string(i + 2);
        m.items.push_back(std::move(item));
    }
    maths::MathConstant c;
    c.name = id + " k";
    c.value.v = 1.5;
    m.constants.push_back(std::move(c));
    return m;
}

const model::Range* lapAt(const model::DataSource& src, int index) {
    const model::DataLayer* layer = src.layer(kLayer);
    const model::RangeGroup* g = layer == nullptr ? nullptr : layer->ranges.find(kLaps);
    if (g == nullptr || g->ranges.empty()) {
        return nullptr;
    }
    return &g->ranges[static_cast<std::size_t>(index) % g->ranges.size()];
}

// The override a trust/offset edit writes (the matching one with its other fields, else a new one); `byName`
// also takes over an override keyed by name only (the commands' rule; DataSelectionModel::setTrusted matches
// by start marker only).
model::RangeOverride overrideIn(const model::SidecarDocument& doc, const std::string& group,
                                const model::MarkerId& start, const std::string& name, bool byName) {
    for (const model::RangeOverride& e : doc.layer(kLayer).rangeOverrides) {
        if (e.groupPath == group && ((!start.isNil() && e.startMarker == start) ||
                                     (start.isNil() && !e.startMarker && e.name == name))) {
            return e;
        }
    }
    for (const model::RangeOverride& e : doc.layer(kLayer).rangeOverrides) {
        if (!byName) {
            break;
        }
        if (e.groupPath == group && e.name == name && (!e.startMarker || start.isNil())) {
            model::RangeOverride ov = e;
            if (!start.isNil()) {
                ov.startMarker = start;
            }
            return ov;
        }
    }
    model::RangeOverride ov;
    ov.groupPath = group;
    if (!start.isNil()) {
        ov.startMarker = start;
    }
    ov.name = name;
    return ov;
}

struct Fixture {
    AppRuntime rt;
    MathsService maths;
    DistanceService distance;
    SourceLoader loader;
    std::unique_ptr<ProjectSession> session;
    std::unique_ptr<DataSelectionModel> selection;
    WorkspaceViewModel workspace;
    std::unique_ptr<WorksheetViewModel> worksheet;
    std::unique_ptr<LapStretchService> stretch;
    std::shared_ptr<const model::DataSource> source;
    std::vector<model::MarkerId> beacons; // the logged beacons (marker edits)
    int ops = 0;                          // makes every generated name unique
    int lastKind = -1;

    Fixture()
        : rt([] {
              AppRuntimeOptions o;
              o.workers = 4;
              o.cacheBudget = std::size_t{256} << 20;
              return o;
          }()),
          maths(rt.pool(), rt.dispatcher(), shippedUnits()), distance(maths, rt.dispatcher()),
          loader(rt.pool(), rt.dispatcher(), shippedUnits()) {}
};

Op sidecarOp(Fixture& f, Rng& rng, int kind, const std::string& tag);

Op makeOp(Fixture& f, Rng& rng, int seq) {
    ProjectSession& s = *f.session;
    QUndoStack& stack = s.undoStack();
    const std::string tag = "s" + std::to_string(seq) + "n" + std::to_string(++f.ops);
    // A drag resizes one component several times in a row: those commands merge (UndoMergeId).
    const int kind = f.lastKind == 13 && rng() % 2 == 0 ? 13 : pick(rng, 27);
    f.lastKind = kind;
    switch (kind) {
    case 0:
    case 1: { // override set
        const std::string channel = std::array{"Engine RPM", "Throttle", "Gear", "Corr Speed"}[pick(rng, 4)];
        project::ChannelOverrides::Entry e;
        e.dps = pick(rng, 4);
        if (rng() % 2 == 0) {
            e.displayUnit = std::string("km/h");
        }
        return {"override " + channel,
                [&s, &stack, channel, e] { stack.push(new SetOverrideCommand(s, channel, e)); },
                [channel, e](ProjectSession& r, model::SidecarDocument&) {
                    project::ChannelOverrides d = r.overrides();
                    d.set(channel, e);
                    r.setOverrides(std::move(d));
                }};
    }
    case 2: { // override clear
        const std::string channel = std::array{"Engine RPM", "Throttle", "Gear"}[pick(rng, 3)];
        return {"clear override " + channel,
                [&s, &stack, channel] { stack.push(new ClearOverrideCommand(s, channel)); },
                [channel](ProjectSession& r, model::SidecarDocument&) {
                    project::ChannelOverrides d = r.overrides();
                    d.set(channel, {});
                    r.setOverrides(std::move(d));
                }};
    }
    case 3: { // aliases: add or replace one row, or remove the first
        const bool remove = rng() % 3 == 0;
        model::AliasRow row;
        row.name = std::array{"Wheel Speed", "Speed Any", "RPM Any"}[pick(rng, 3)];
        row.candidates = {{"Wheel Speed FL", true}, {"Wheel Speed FR", rng() % 2 == 0}};
        const auto edit = [remove, row](project::Aliases a) {
            auto& rows = a.table.rows;
            if (remove) {
                if (!rows.empty()) {
                    rows.erase(rows.begin());
                }
                return a;
            }
            const auto it = std::find_if(rows.begin(), rows.end(),
                                         [&row](const model::AliasRow& e) { return e.name == row.name; });
            if (it == rows.end()) {
                rows.push_back(row);
            } else {
                *it = row;
            }
            return a;
        };
        return {"aliases",
                [&s, &stack, edit] {
                    stack.push(new EditAliasesCommand(s, edit(s.aliases()), QStringLiteral("Aliases")));
                },
                [edit](ProjectSession& r, model::SidecarDocument&) { r.setAliases(edit(r.aliases())); }};
    }
    case 4: { // mappings
        const bool remove = rng() % 3 == 0;
        project::Mappings::Row row;
        row.from = "WheelSP_" + std::to_string(pick(rng, 3));
        row.to = "Wheel Speed " + std::to_string(pick(rng, 3));
        const auto edit = [remove, row](project::Mappings m) {
            if (remove) {
                if (!m.rows.empty()) {
                    m.rows.pop_back();
                }
            } else {
                m.rows.push_back(row);
            }
            return m;
        };
        return {"mappings",
                [&s, &stack, edit] {
                    stack.push(new EditMappingsCommand(s, edit(s.mappings()), QStringLiteral("Mappings")));
                },
                [edit](ProjectSession& r, model::SidecarDocument&) { r.setMappings(edit(r.mappings())); }};
    }
    case 5: { // unit map set or removed
        formats::UnitMappingProfile map;
        if (rng() % 3 != 0) {
            map.importer = "text.profile:generic.csv";
            formats::UnitMappingProfile::Rule rule;
            rule.match = formats::UnitMappingProfile::Match::UnitOnly;
            rule.vendorUnit = "KPH" + std::to_string(pick(rng, 3));
            rule.quantity = "Speed";
            rule.unit = "km/h";
            map.rules.push_back(rule);
        }
        return {"unit map", [&s, &stack, map] { stack.push(new SetUnitMapCommand(s, "generic.csv", map)); },
                [map](ProjectSession& r, model::SidecarDocument&) { r.setUnitMap("generic.csv", map); }};
    }
    case 6:
    case 7: { // maths modules: add, remove, reorder, item edit
        const int what = pick(rng, 4);
        const std::string id = "m" + std::to_string(pick(rng, 4));
        const int items = 1 + pick(rng, 3);
        const auto edit = [what, id, items](std::vector<maths::MathModule> mods) {
            switch (what) {
            case 0: // add (or replace)
                if (auto it = std::find_if(mods.begin(), mods.end(), [&](auto& m) { return m.id == id; });
                    it != mods.end()) {
                    *it = module(id, items);
                } else {
                    mods.push_back(module(id, items));
                }
                break;
            case 1:
                if (!mods.empty()) {
                    mods.erase(mods.begin());
                }
                break;
            case 2:
                if (mods.size() > 1) {
                    std::rotate(mods.begin(), mods.begin() + 1, mods.end());
                }
                break;
            default:
                if (!mods.empty()) {
                    mods.back().enabled = !mods.back().enabled;
                    if (!mods.back().items.empty()) {
                        mods.back().items.front().expr = "'Throttle' * " + std::to_string(items + 10);
                    }
                }
                break;
            }
            return mods;
        };
        return {"maths modules",
                [&s, &stack, edit] {
                    stack.push(
                        new EditMathModulesCommand(s, edit(s.mathsModules()), QStringLiteral("Maths")));
                },
                [edit](ProjectSession& r, model::SidecarDocument&) {
                    r.setMathsModules(edit(r.mathsModules()));
                }};
    }
    case 8: { // project options (fast-lap tolerance, compat switch)
        const double tolerance = 0.5 + pick(rng, 5);
        const bool compat = rng() % 2 == 0;
        const auto edit = [tolerance, compat](project::ProjectFile p) {
            QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(p.options.compact())).object();
            o.insert(QStringLiteral("fastLapTolerancePct"), tolerance);
            QJsonObject m = o.value(QStringLiteral("maths")).toObject();
            QJsonObject c = m.value(QStringLiteral("compat")).toObject();
            c.insert(QStringLiteral("unaryMinusAbovePower"), compat);
            m.insert(QStringLiteral("compat"), c);
            o.insert(QStringLiteral("maths"), m);
            p.options = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()).value();
            return p;
        };
        return {
            "options",
            [&s, &stack, edit] {
                stack.push(new SetProjectOptionsCommand(s, edit(s.projectFile()), QStringLiteral("Options")));
            },
            [edit](ProjectSession& r, model::SidecarDocument&) { r.setProjectFile(edit(r.projectFile())); }};
    }
    case 9: { // worksheet add
        const int wb = pick(rng, static_cast<int>(s.workbooks().size()));
        project::WorksheetDef sheet;
        sheet.id = randomId(rng);
        sheet.name = "Sheet " + tag;
        sheet.linkTo = "Laps";
        const int at = pick(rng, 3) - 1;
        return {"add worksheet",
                [&s, &stack, wb, sheet, at] { stack.push(new AddWorksheetCommand(s, wb, sheet, at)); },
                [wb, sheet, at](ProjectSession& r, model::SidecarDocument&) {
                    project::WorkbookFile w = r.workbooks().at(static_cast<std::size_t>(wb));
                    const auto n = static_cast<int>(w.worksheets.size());
                    const int pos = at < 0 || at > n ? n : at;
                    w.worksheets.insert(w.worksheets.begin() + pos, sheet);
                    r.replaceWorkbook(wb, std::move(w));
                }};
    }
    case 10: { // worksheet remove / rename / move
        const int wb = pick(rng, static_cast<int>(s.workbooks().size()));
        const int n = static_cast<int>(s.workbooks().at(static_cast<std::size_t>(wb)).worksheets.size());
        if (n < 2) {
            return makeOp(f, rng, seq);
        }
        const int a = pick(rng, n);
        const int b = pick(rng, n);
        const int what = pick(rng, 3);
        const std::string name = "Renamed " + tag;
        return {"worksheet",
                [&s, &stack, wb, a, b, what, name] {
                    if (what == 0) {
                        stack.push(new RemoveWorksheetCommand(s, wb, a));
                    } else if (what == 1) {
                        stack.push(new RenameWorksheetCommand(s, wb, a, name));
                    } else {
                        stack.push(new MoveWorksheetCommand(s, wb, a, b));
                    }
                },
                [wb, a, b, what, name](ProjectSession& r, model::SidecarDocument&) {
                    project::WorkbookFile w = r.workbooks().at(static_cast<std::size_t>(wb));
                    auto& ws = w.worksheets;
                    if (what == 0) {
                        ws.erase(ws.begin() + a);
                    } else if (what == 1) {
                        ws[static_cast<std::size_t>(a)].name = name;
                    } else if (a != b) {
                        auto sheet = ws[static_cast<std::size_t>(a)];
                        ws.erase(ws.begin() + a);
                        ws.insert(ws.begin() + b, std::move(sheet));
                    }
                    r.replaceWorkbook(wb, std::move(w));
                }};
    }
    case 11:
    case 12: { // component paste (ids assigned by the command) and delete
        const int wb = pick(rng, static_cast<int>(s.workbooks().size()));
        const auto& sheets = s.workbooks().at(static_cast<std::size_t>(wb)).worksheets;
        const int sheet = pick(rng, static_cast<int>(sheets.size()));
        const auto& comps = sheets.empty() ? std::vector<project::ComponentSpec>{}
                                           : sheets[static_cast<std::size_t>(sheet)].components;
        if (comps.empty() || sheets.empty()) {
            return makeOp(f, rng, seq);
        }
        if (kind == 11) {
            const std::vector<project::ComponentSpec> copied = componentsFromMime(
                componentsMime({comps[static_cast<std::size_t>(pick(rng, int(comps.size())))]}));
            auto ids = std::make_shared<std::vector<Uuid>>();
            return {"paste",
                    [&s, &stack, wb, sheet, copied, ids] {
                        const auto before = s.workbooks()[static_cast<std::size_t>(wb)]
                                                .worksheets[static_cast<std::size_t>(sheet)]
                                                .components.size();
                        stack.push(new AddComponentsCommand(s, wb, sheet, copied));
                        const auto& now = s.workbooks()[static_cast<std::size_t>(wb)]
                                              .worksheets[static_cast<std::size_t>(sheet)]
                                              .components;
                        for (std::size_t i = before; i < now.size(); ++i) {
                            ids->push_back(now[i].id);
                        }
                    },
                    [wb, sheet, copied, ids](ProjectSession& r, model::SidecarDocument&) {
                        project::WorkbookFile w = r.workbooks().at(static_cast<std::size_t>(wb));
                        auto& list = w.worksheets[static_cast<std::size_t>(sheet)].components;
                        int z = 0;
                        for (const auto& c : list) {
                            z = std::max(z, c.z + 1);
                        }
                        for (std::size_t i = 0; i < copied.size(); ++i) {
                            project::ComponentSpec c = copied[i];
                            c.id = ids->at(i);
                            c.z = z++;
                            list.push_back(std::move(c));
                        }
                        r.replaceWorkbook(wb, std::move(w));
                    }};
        }
        const Uuid id = comps[static_cast<std::size_t>(pick(rng, int(comps.size())))].id;
        return {"delete component",
                [&s, &stack, wb, sheet, id] { stack.push(new RemoveComponentsCommand(s, wb, sheet, {id})); },
                [wb, sheet, id](ProjectSession& r, model::SidecarDocument&) {
                    project::WorkbookFile w = r.workbooks().at(static_cast<std::size_t>(wb));
                    auto& list = w.worksheets[static_cast<std::size_t>(sheet)].components;
                    list.erase(std::remove_if(list.begin(), list.end(),
                                              [&id](const project::ComponentSpec& c) { return c.id == id; }),
                               list.end());
                    r.replaceWorkbook(wb, std::move(w));
                }};
    }
    case 13: { // component resize (consecutive resizes of one component merge)
        const auto& sheets = s.workbooks().at(0).worksheets;
        if (sheets.empty() || sheets[0].components.empty()) {
            return makeOp(f, rng, seq);
        }
        const Uuid id = sheets[0].components.front().id;
        const double right = 0.5 + static_cast<double>(pick(rng, 40)) / 100.0;
        const auto edit = [id, right](project::WorkbookFile w) {
            for (auto& c : w.worksheets[0].components) {
                if (c.id == id) {
                    c.rect.r = right;
                }
            }
            return w;
        };
        return {"resize",
                [&s, &stack, edit, id] {
                    stack.push(new WorkbookCommand(s, 0, edit(s.workbooks().at(0)), QStringLiteral("Resize"),
                                                   UndoMergeId::ComponentGeometry, id.str()));
                },
                [edit](ProjectSession& r, model::SidecarDocument&) {
                    r.replaceWorkbook(0, edit(r.workbooks().at(0)));
                }};
    }
    case 14: { // workbook list: add, remove, rename, move
        const int n = static_cast<int>(s.workbooks().size());
        const int what = pick(rng, 4);
        const int a = pick(rng, n);
        const int b = pick(rng, n);
        project::WorkbookFile added = s.workbooks().front();
        added.id = randomId(rng);
        added.name = "Book " + tag;
        const std::string name = "Renamed book " + tag;
        if (what == 1 && n < 2) {
            return makeOp(f, rng, seq);
        }
        return {"workbooks",
                [&s, &stack, what, a, b, added, name] {
                    switch (what) {
                    case 0:
                        stack.push(new AddWorkbookCommand(s, added, -1));
                        break;
                    case 1:
                        stack.push(new RemoveWorkbookCommand(s, a));
                        break;
                    case 2:
                        stack.push(new RenameWorkbookCommand(s, a, name));
                        break;
                    default:
                        stack.push(new MoveWorkbookCommand(s, a, b));
                        break;
                    }
                },
                [what, a, b, added, name](ProjectSession& r, model::SidecarDocument&) {
                    WorkbookSet set = r.workbookSet();
                    switch (what) {
                    case 0:
                        set.workbooks.push_back(added);
                        set.names.push_back(added.name);
                        break;
                    case 1:
                        set.workbooks.erase(set.workbooks.begin() + a);
                        set.names.erase(set.names.begin() + a);
                        break;
                    case 2:
                        set.workbooks[static_cast<std::size_t>(a)].name = name;
                        set.names[static_cast<std::size_t>(a)] = name;
                        break;
                    default:
                        if (a != b) {
                            auto w = set.workbooks[static_cast<std::size_t>(a)];
                            auto nm = set.names[static_cast<std::size_t>(a)];
                            set.workbooks.erase(set.workbooks.begin() + a);
                            set.names.erase(set.names.begin() + a);
                            set.workbooks.insert(set.workbooks.begin() + b, std::move(w));
                            set.names.insert(set.names.begin() + b, std::move(nm));
                        }
                        break;
                    }
                    r.setWorkbookSet(std::move(set));
                }};
    }
    default:
        return sidecarOp(f, rng, kind, tag);
    }
}

Op sidecarOp(Fixture& f, Rng& rng, int kind, const std::string& tag) {
    SourceLoader& loader = f.loader;
    QUndoStack& stack = f.session->undoStack();
    const std::shared_ptr<const model::DataSource> src = loader.source(f.source->id);
    const model::SidecarDocument& doc = src->sidecar;
    const std::string layer(kLayer);
    switch (kind) {
    case 15: { // detail edit or restore
        const std::string id = std::array{"Driver", "Venue", "Session"}[pick(rng, 3)];
        std::optional<model::DetailValue> value;
        if (rng() % 3 != 0) {
            value = model::DetailValue(std::string("value ") + tag);
        }
        return {"detail " + id,
                [&loader, &stack, src, layer, id, value] {
                    stack.push(new SetDetailCommand(loader, *src, layer, id, value));
                },
                [layer, id, value](ProjectSession&, model::SidecarDocument& d) {
                    if (value) {
                        model::Detail det;
                        det.id = id;
                        det.value = *value;
                        d.setDetail(layer, std::move(det));
                    } else {
                        d.revertDetail(layer, id);
                    }
                }};
    }
    case 16:
    case 17: { // trust through the Data window's path (DataSelectionModel::setTrusted, an undoable command)
        if (rangeCount(*src) == 0) {
            return {"noop", [] {}, [](ProjectSession&, model::SidecarDocument&) {}};
        }
        const auto i = static_cast<std::size_t>(pick(rng, int(rangeCount(*src))));
        const model::RangeRef ref = rangeRef(*src, i);
        const model::Range* r = &rangeAt(*src, i);
        const bool trusted = !r->trusted;
        const std::string group(kLaps);
        const model::MarkerId start = r->startMarker;
        const std::string name = r->name;
        DataSelectionModel* selection = f.selection.get();
        return {"trust " + name, [selection, ref, trusted] { selection->setTrusted(ref, trusted); },
                [layer, group, start, name, trusted](ProjectSession&, model::SidecarDocument& d) {
                    model::RangeOverride ov = overrideIn(d, group, start, name, false);
                    ov.trusted = trusted;
                    d.setRangeOverride(layer, std::move(ov));
                }};
    }
    case 18: { // alignment offset
        const model::Range* lap = lapAt(*src, pick(rng, 8));
        if (lap == nullptr) {
            return {"noop", [] {}, [](ProjectSession&, model::SidecarDocument&) {}};
        }
        const model::Range range = *lap;
        const Micros us = (pick(rng, 3) - 1) * 40000;
        const double m = static_cast<double>(pick(rng, 3) - 1) * 1.25;
        return {"offset " + range.name,
                [&loader, &stack, src, layer, range, us, m] {
                    stack.push(new OffsetRangeCommand(loader, *src, layer, std::string(kLaps), range, us, m));
                },
                [layer, range, us, m](ProjectSession&, model::SidecarDocument& d) {
                    model::RangeOverride ov =
                        overrideIn(d, std::string(kLaps), range.startMarker, range.name, true);
                    ov.offsetUs = us != 0 ? std::optional<Micros>(us) : std::nullopt;
                    ov.offsetM = m != 0.0 ? std::optional<double>(m) : std::nullopt;
                    d.setRangeOverride(layer, std::move(ov));
                }};
    }
    case 19:
    case 20: { // beacon insert (SHIFT+F7) or delete of a user beacon
        const auto& users = doc.layer(layer).markers;
        if (kind == 20 && !users.empty()) {
            const model::MarkerId id = users[static_cast<std::size_t>(pick(rng, int(users.size())))].id;
            return {
                "delete beacon",
                [&loader, &stack, src, layer, id] {
                    stack.push(new RemoveMarkerCommand(loader, *src, layer, id));
                },
                [layer, id](ProjectSession&, model::SidecarDocument& d) { d.removeUserMarker(layer, id); }};
        }
        model::Marker m;
        m.id = model::MarkerId{randomId(rng)};
        m.cls = model::MarkerClass::BCN;
        m.group = "Beacons";
        const Micros duration = src->layer(kLayer)->duration;
        m.t = static_cast<Micros>(rng() % static_cast<std::uint64_t>(std::max<Micros>(duration, 1)));
        m.prov = model::Provenance::User;
        return {
            "insert beacon",
            [&loader, &stack, src, layer, m] { stack.push(new InsertMarkerCommand(loader, *src, layer, m)); },
            [layer, m](ProjectSession&, model::SidecarDocument& d) { d.addUserMarker(layer, m); }};
    }
    case 21: { // marker edit: offset a logged beacon, or the layer's offsetDistance
        model::MarkerEdit edit;
        if (rng() % 2 == 0 && !f.beacons.empty()) {
            edit.op = model::MarkerEditOp::Offset;
            edit.ref = f.beacons[static_cast<std::size_t>(pick(rng, int(f.beacons.size())))];
            edit.dtUs = (pick(rng, 5) - 2) * 100000;
        } else {
            edit.op = model::MarkerEditOp::OffsetDistance;
            edit.dM = static_cast<double>(pick(rng, 5) - 2) * 2.5;
        }
        return {"marker edit",
                [&loader, &stack, src, layer, edit] {
                    stack.push(new MarkerEditCommand(loader, *src, layer, edit));
                },
                [layer, edit](ProjectSession&, model::SidecarDocument& d) {
                    model::SidecarDocument trial = d;
                    if (trial.applyEdit(layer, edit).ok()) {
                        d = std::move(trial);
                    }
                }};
    }
    case 22:
        return {
            "restore logged laps",
            [&loader, &stack, src, layer] { stack.push(new RestoreLoggedLapsCommand(loader, *src, layer)); },
            [layer](ProjectSession&, model::SidecarDocument& d) { d.restoreLogged(layer); }};
    case 23:
    case 24: { // annotation set or removed
        const auto& notes = doc.layer(layer).annotations;
        if (kind == 24 && !notes.empty()) {
            const Uuid id = notes[static_cast<std::size_t>(pick(rng, int(notes.size())))].id;
            return {
                "delete annotation",
                [&loader, &stack, src, layer, id] {
                    stack.push(new RemoveAnnotationCommand(loader, *src, layer, id));
                },
                [layer, id](ProjectSession&, model::SidecarDocument& d) { d.removeAnnotation(layer, id); }};
        }
        model::Annotation a;
        a.id = notes.empty() || rng() % 2 == 0
                   ? randomId(rng)
                   : notes[static_cast<std::size_t>(pick(rng, int(notes.size())))].id;
        a.tUs = static_cast<Micros>(pick(rng, 100000) * 100);
        a.text = "note " + tag;
        return {"annotation",
                [&loader, &stack, src, layer, a] {
                    stack.push(new SetAnnotationCommand(loader, *src, layer, a));
                },
                [layer, a](ProjectSession&, model::SidecarDocument& d) { d.setAnnotation(layer, a); }};
    }
    default: { // local maths set or removed
        std::optional<model::OpaqueJson> json;
        if (rng() % 3 != 0) {
            const std::string text = R"({"$schema":"urn:esses:maths-1","schemaVersion":1,"id":"local",)"
                                     R"("items":[{"kind":"expression","name":"Local )" +
                                     std::to_string(pick(rng, 3)) + R"(","expr":"'Throttle' / 2"}]})";
            json = JsonDoc::parse(text).value();
        }
        return {"local maths",
                [&loader, &stack, src, layer, json] {
                    stack.push(
                        new SetLocalMathsCommand(loader, *src, layer, json, QStringLiteral("Local maths")));
                },
                [layer, json](ProjectSession&, model::SidecarDocument& d) { d.setLocalMaths(layer, json); }};
    }
    }
}

// A view-state operation: never on the stack, never a document change.
std::string viewOp(Fixture& f, Rng& rng) {
    DataSelectionModel& sel = *f.selection;
    const auto src = f.loader.source(f.source->id);
    switch (pick(rng, 9)) {
    case 0:
        sel.setMain(rangeRef(*src, static_cast<std::size_t>(pick(rng, int(rangeCount(*src))))));
        return "setMain";
    case 1:
        sel.toggleOverlay(rangeRef(*src, static_cast<std::size_t>(pick(rng, int(rangeCount(*src))))));
        return "toggleOverlay";
    case 2:
        sel.clearOverlays();
        return "clearOverlays";
    case 3:
        sel.setSingleColourMain(!sel.snapshot().singleColourMain);
        return "singleColour";
    case 4:
        f.worksheet->zoomIn();
        return "zoomIn";
    case 5:
        f.worksheet->toggleDomain();
        return "F9";
    case 6:
        f.worksheet->stepCursor(pick(rng, 21) - 10);
        return "cursor";
    case 7: {
        FilterSpec spec;
        spec.kind = rng() % 2 == 0 ? FilterSpec::Kind::All : FilterSpec::Kind::LapNumbers;
        spec.laps = "1-" + std::to_string(1 + pick(rng, 4));
        sel.setFilter(src->id, spec);
        return "filter";
    }
    default:
        f.stretch->toggle();
        return "lapStretch";
    }
}

} // namespace

class TstUndoProperty : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
    }

    void thirtyRandomSequencesEqualTheirReplay() {
        const std::uint64_t seed = testSeed();
        const auto dir = scratchDir("undo-property");
        Fixture f;
        f.session = std::make_unique<ProjectSession>(pathsFor(dir / "config"));
        QVERIFY(f.session->loadDefaults().ok());
        f.session->setUnits(shippedUnits());
        f.selection = std::make_unique<DataSelectionModel>(f.session.get());
        static_cast<void>(f.maths.loadSettingsSchema(repoRoot() / "data"));
        f.loader.setUseSidecars(false);
        f.loader.setRuleRunner(analysis::RuleRunner::circuitDefault());
        f.loader.setMathsService(&f.maths);
        f.loader.setDistanceService(&f.distance);
        connectSourceLoader(f.loader, *f.selection, nullptr);
        f.stretch =
            std::make_unique<LapStretchService>(f.loader, f.distance, shippedUnits(), f.session.get());
        const project::WorksheetDef sheet = f.session->workbooks().front().worksheets.front();
        f.worksheet = std::make_unique<WorksheetViewModel>(sheet, f.workspace, *f.selection, f.distance);
        f.worksheet->setSize(800);

        // One open source (the sidecar edits).
        SyntheticOptions o;
        o.path = "undo.ld";
        const auto written = makeSource(o, f.rt.cache());
        const std::filesystem::path ld = writeLd(*written, dir, "undo");
        QSignalSpy published(&f.loader, &SourceLoader::sourcePublished);
        f.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        f.source =
            f.loader.source(published.first().at(0).value<std::shared_ptr<const model::DataSource>>()->id);
        for (const model::Marker& m : f.source->layer(kLayer)->baseMarkers) {
            if (m.cls == model::MarkerClass::BCN) {
                f.beacons.push_back(m.id);
            }
        }
        QVERIFY(!f.beacons.empty());
        QVERIFY(f.selection->hasMain());

        // Initial documents with content (written directly: not edits on the stack).
        {
            project::ChannelOverrides ov;
            project::ChannelOverrides::Entry rpm;
            rpm.displayUnit = std::string("rpm");
            ov.set("Engine RPM", rpm);
            f.session->setOverrides(std::move(ov));
            project::Aliases al;
            al.table.rows.push_back(
                model::AliasRow{"Speed Any", {{"Wheel Speed FL", true}}, -1, std::nullopt});
            f.session->setAliases(std::move(al));
            project::Mappings mp;
            mp.rows.push_back(project::Mappings::Row{"WheelSP_FL", "Wheel Speed FL", std::nullopt, false});
            f.session->setMappings(std::move(mp));
            f.session->setMathsModules({module("m0", 2)});
        }
        QUndoStack& stack = f.session->undoStack();
        QCOMPARE(stack.count(), 0);
        const model::SidecarDocument sidecar0 = f.loader.source(f.source->id)->sidecar;
        const WorkbookSet workbooks0 = f.session->workbookSet();
        const project::ProjectFile project0 = f.session->projectFile();
        const project::ChannelOverrides overrides0 = f.session->overrides();
        const project::Aliases aliases0 = f.session->aliases();
        const project::Mappings mappings0 = f.session->mappings();
        const std::vector<maths::MathModule> modules0 = f.session->mathsModules();
        // The replay session: reset to the initial documents before every replay.
        ProjectSession replay(pathsFor(dir / "config"));
        QVERIFY(replay.loadDefaults().ok());
        replay.setUnits(shippedUnits());
        const auto resetReplay = [&] {
            replay.setWorkbookSet(workbooks0);
            replay.setProjectFile(project0);
            replay.setOverrides(overrides0);
            replay.setAliases(aliases0);
            replay.setMappings(mappings0);
            const auto maps = replay.unitMaps();
            for (const auto& [stem, map] : maps) {
                replay.setUnitMap(stem, {});
            }
            replay.setMathsModules(modules0);
        };
        const Snapshot docs0 = snapshotOf(*f.session, sidecar0);
        Rng rng(seed);
        int pushes = 0;
        int viewOps = 0;
        int merges = 0;
        for (int seq = 0; seq < kSequences; ++seq) {
            std::vector<std::vector<Op>> applied; // per stack command, its edits (merged ones grouped)
            for (int step = 0; step < kStepsPerSequence; ++step) {
                const std::string where = "seed " + std::to_string(seed) + ", sequence " +
                                          std::to_string(seq) + ", step " + std::to_string(step);
                const int action = pick(rng, 10);
                std::string what;
                if (action < 5) {
                    Op op = makeOp(f, rng, seq);
                    what = op.name;
                    const int index = stack.index();
                    op.push();
                    if (stack.count() == index + 1 && stack.index() == index + 1) {
                        applied.resize(static_cast<std::size_t>(index));
                        applied.push_back({std::move(op)});
                        ++pushes;
                    } else if (stack.count() == index && index > 0) {
                        applied.resize(static_cast<std::size_t>(index));
                        applied.back().push_back(std::move(op)); // merged into the previous command
                        ++merges;
                    } else {
                        QVERIFY2(op.name == "noop", (where + ": " + op.name + " pushed nothing").c_str());
                    }
                } else if (action < 7) {
                    what = "undo";
                    stack.undo();
                } else if (action < 8) {
                    what = "redo";
                    stack.redo();
                } else {
                    const int count = stack.count();
                    const int index = stack.index();
                    const Snapshot before = snapshotOf(*f.session, f.loader.source(f.source->id)->sidecar);
                    what = viewOp(f, rng);
                    ++viewOps;
                    QVERIFY2(stack.count() == count && stack.index() == index, (where + ": " + what).c_str());
                    QVERIFY2(snapshotOf(*f.session, f.loader.source(f.source->id)->sidecar) == before,
                             (where + ": " + what + " changed a document").c_str());
                }
                // The replay: the initial documents and sidecar with the applied edits in stack order.
                resetReplay();
                model::SidecarDocument replaySidecar = sidecar0;
                for (int i = 0; i < stack.index(); ++i) {
                    for (const Op& op : applied[static_cast<std::size_t>(i)]) {
                        op.replay(replay, replaySidecar);
                    }
                }
                const model::SidecarDocument& liveSidecar = f.loader.source(f.source->id)->sidecar;
                const bool same = snapshotOf(*f.session, liveSidecar) == snapshotOf(replay, replaySidecar);
                if (!same) {
                    const std::vector<std::string> live = documentsOf(*f.session, liveSidecar);
                    const std::vector<std::string> expected = documentsOf(replay, replaySidecar);
                    for (std::size_t i = 0; i < live.size() && i < expected.size(); ++i) {
                        if (live[i] != expected[i]) {
                            qWarning("live:\n%s\nreplay:\n%s", live[i].c_str(), expected[i].c_str());
                            break;
                        }
                    }
                }
                QVERIFY2(same, (where + ": after " + what).c_str());
            }
            // Undo everything: the initial documents; redo everything: the final ones.
            const Snapshot final = snapshotOf(*f.session, f.loader.source(f.source->id)->sidecar);
            const int top = stack.index();
            stack.setIndex(0);
            QVERIFY2(snapshotOf(*f.session, f.loader.source(f.source->id)->sidecar) == docs0,
                     ("seed " + std::to_string(seed) + ", sequence " + std::to_string(seq) + ": undo all")
                         .c_str());
            stack.setIndex(top);
            QVERIFY2(snapshotOf(*f.session, f.loader.source(f.source->id)->sidecar) == final,
                     ("seed " + std::to_string(seed) + ", sequence " + std::to_string(seq) + ": redo all")
                         .c_str());
            stack.setIndex(0);
            stack.clear();
        }
        qInfo("seed %llu: %d commands pushed, %d merged, %d view-state operations",
              static_cast<unsigned long long>(seed), pushes, merges, viewOps);
        QVERIFY(pushes > kSequences * 10);
        QVERIFY(merges > 0);
        QVERIFY(viewOps > kSequences * 4);
    }

    // File open and close and the selection are view state (D-032): the stack is unchanged by them, and a
    // command whose source was closed does nothing on undo.
    void fileOpenAndCloseAreNotOnTheStack() {
        const auto dir = scratchDir("undo-files");
        Fixture f;
        f.session = std::make_unique<ProjectSession>(pathsFor(dir / "config"));
        QVERIFY(f.session->loadDefaults().ok());
        f.selection = std::make_unique<DataSelectionModel>(f.session.get());
        f.loader.setUseSidecars(false);
        f.loader.setMathsService(&f.maths);
        connectSourceLoader(f.loader, *f.selection, nullptr);
        SyntheticOptions o;
        o.path = "files.ld";
        const auto written = makeSource(o, f.rt.cache());
        const std::filesystem::path ld = writeLd(*written, dir, "files");
        QSignalSpy published(&f.loader, &SourceLoader::sourcePublished);
        f.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 30000);
        QCOMPARE(f.session->undoStack().count(), 0);
        const auto src =
            f.loader.source(published.first().at(0).value<std::shared_ptr<const model::DataSource>>()->id);
        f.selection->setTrusted(rangeRef(*src, 1), false);
        QCOMPARE(f.session->undoStack().count(), 1);
        QCOMPARE(f.session->undoStack().undoText(), QStringLiteral("Edit trust"));
        QVERIFY(!rangeAt(*f.loader.source(src->id), 1).trusted);
        f.session->undoStack().undo();
        QVERIFY(rangeAt(*f.loader.source(src->id), 1).trusted);
        f.session->undoStack().redo();
        f.loader.close(src->id);
        QCOMPARE(f.session->undoStack().count(), 1);
        f.session->undoStack().undo(); // the source is closed: nothing to commit, nothing breaks
        QVERIFY(f.loader.source(src->id) == nullptr);
    }
};

QTEST_GUILESS_MAIN(TstUndoProperty)
#include "tst_undo_property.moc"
