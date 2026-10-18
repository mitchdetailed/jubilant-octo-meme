// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The undo commands (D-032, D-161; 02 section 7): the "after" document of each named command, computed from
// the current state in the constructor, the sidecar commit of SidecarCommand, the component clipboard and the
// Mapping panel's decision.

#include <esses/base/casefold.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <utility>

namespace esses::qt {

namespace {

template <class Doc, class Setter> auto setterOf(ProjectSession& session, Setter setter) {
    return [&session, setter](const Doc& doc) { (session.*setter)(doc); };
}

model::SidecarDocument sidecarOf(const model::DataSource& source) {
    return source.sidecar;
}

// The override of `range` in `layer` (matched as SidecarDocument::setRangeOverride does), or a new one keyed
// (groupPath, startMarker) with the range name as fallback key.
model::RangeOverride overrideFor(const model::SidecarDocument& doc, const std::string& layer,
                                 const std::string& groupPath, const model::Range& range) {
    const std::vector<model::RangeOverride> list = doc.layer(layer).rangeOverrides; // copy (GCC 13)
    const std::optional<model::MarkerId> start =
        range.startMarker.v.isNil() ? std::nullopt : std::optional<model::MarkerId>(range.startMarker);
    auto it = list.end();
    if (start) {
        it = std::find_if(list.begin(), list.end(), [&](const model::RangeOverride& e) {
            return e.groupPath == groupPath && e.startMarker == start;
        });
    }
    if (it == list.end()) {
        it = std::find_if(list.begin(), list.end(), [&](const model::RangeOverride& e) {
            return e.groupPath == groupPath && e.name == range.name && (!e.startMarker || !start);
        });
    }
    if (it != list.end()) {
        model::RangeOverride ov = *it;
        if (start) {
            ov.startMarker = start;
        }
        return ov;
    }
    model::RangeOverride ov;
    ov.groupPath = groupPath;
    ov.startMarker = start;
    ov.name = range.name;
    return ov;
}

} // namespace

// ---- SidecarCommand ---------------------------------------------------------------------------------------

SidecarCommand::SidecarCommand(SourceLoader& loader, model::SourceId source, model::SidecarDocument before,
                               model::SidecarDocument after, const QString& text, QUndoCommand* parent)
    : QUndoCommand(text, parent), loader_(&loader), source_(source), before_(std::move(before)),
      after_(std::move(after)) {}

void SidecarCommand::redo() {
    commit(after_);
}
void SidecarCommand::undo() {
    commit(before_);
}

void SidecarCommand::commit(const model::SidecarDocument& doc) {
    SourceLoader* loader = loader_.data();
    if (loader == nullptr) {
        return; // the loader is gone (the window is closing)
    }
    std::shared_ptr<const model::DataSource> current = loader->source(source_);
    if (current == nullptr) {
        return; // the source was closed since the edit
    }
    (void)loader->commitSidecar(std::move(current), doc);
}

// ---- workbooks --------------------------------------------------------------------------------------------

WorkbookCommand::WorkbookCommand(ProjectSession& session, int index, project::WorkbookFile after,
                                 const QString& text, UndoMergeId mergeId, std::string componentId)
    : DocumentCommand<project::WorkbookFile>(
          text, session.workbooks().at(static_cast<std::size_t>(index)), std::move(after),
          [&session, index](const project::WorkbookFile& doc) { session.replaceWorkbook(index, doc); }),
      mergeId_(mergeId), componentId_(std::move(componentId)) {}

bool WorkbookCommand::mergeWith(const QUndoCommand* other) {
    if (mergeId_ == UndoMergeId::None || other->id() != id()) {
        return false;
    }
    const auto* o = dynamic_cast<const WorkbookCommand*>(other);
    if (o == nullptr || o->componentId_ != componentId_ || o->before().id != before().id) {
        return false;
    }
    setAfter(o->after());
    return true;
}

namespace {

project::WorkbookFile movedWorksheet(const ProjectSession& session, int workbook, int from, int to) {
    project::WorkbookFile wb = session.workbooks().at(static_cast<std::size_t>(workbook));
    const auto n = static_cast<int>(wb.worksheets.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) {
        return wb;
    }
    auto sheet = wb.worksheets.begin() + from;
    if (from < to) {
        std::rotate(sheet, sheet + 1, wb.worksheets.begin() + to + 1);
    } else {
        std::rotate(wb.worksheets.begin() + to, sheet, sheet + 1);
    }
    return wb;
}

} // namespace

MoveWorksheetCommand::MoveWorksheetCommand(ProjectSession& session, int workbook, int from, int to)
    : WorkbookCommand(session, workbook, movedWorksheet(session, workbook, from, to),
                      QStringLiteral("Move worksheet")) {}

namespace {

bool validIndex(int i, std::size_t n) noexcept {
    return i >= 0 && static_cast<std::size_t>(i) < n;
}

// `name`, or `name 2`, `name 3`, ... when `taken` already holds it (case-insensitively).
std::string uniqueName(const std::string& name, const std::vector<std::string>& taken) {
    const auto used = [&taken](const std::string& n) {
        return std::any_of(taken.begin(), taken.end(),
                           [&n](const std::string& t) { return caseFold(t) == caseFold(n); });
    };
    if (!used(name)) {
        return name;
    }
    for (int k = 2;; ++k) {
        std::string candidate = name + " " + std::to_string(k);
        if (!used(candidate)) {
            return candidate;
        }
    }
}

std::vector<std::string> sheetNames(const project::WorkbookFile& wb) {
    std::vector<std::string> names;
    for (const project::WorksheetDef& s : wb.worksheets) {
        names.push_back(s.name);
    }
    return names;
}

project::WorkbookFile withSheetAdded(const ProjectSession& session, int workbook, project::WorksheetDef sheet,
                                     int index) {
    project::WorkbookFile wb = session.workbooks().at(static_cast<std::size_t>(workbook));
    if (sheet.id.isNil()) {
        sheet.id = uuidRandom();
    }
    sheet.name = uniqueName(sheet.name.empty() ? std::string("Worksheet") : sheet.name, sheetNames(wb));
    const auto n = static_cast<int>(wb.worksheets.size());
    const int at = index < 0 || index > n ? n : index;
    wb.worksheets.insert(wb.worksheets.begin() + at, std::move(sheet));
    return wb;
}

project::WorkbookFile withSheetRemoved(const ProjectSession& session, int workbook, int sheet) {
    project::WorkbookFile wb = session.workbooks().at(static_cast<std::size_t>(workbook));
    if (validIndex(sheet, wb.worksheets.size())) {
        wb.worksheets.erase(wb.worksheets.begin() + sheet);
    }
    return wb;
}

project::WorkbookFile withSheetRenamed(const ProjectSession& session, int workbook, int sheet,
                                       const std::string& name) {
    project::WorkbookFile wb = session.workbooks().at(static_cast<std::size_t>(workbook));
    if (validIndex(sheet, wb.worksheets.size()) && !name.empty()) {
        std::vector<std::string> others = sheetNames(wb);
        others.erase(others.begin() + sheet);
        wb.worksheets[static_cast<std::size_t>(sheet)].name = uniqueName(name, others);
    }
    return wb;
}

project::WorkbookFile withComponentsAdded(const ProjectSession& session, int workbook, int sheet,
                                          std::vector<project::ComponentSpec> components) {
    project::WorkbookFile wb = session.workbooks().at(static_cast<std::size_t>(workbook));
    if (!validIndex(sheet, wb.worksheets.size())) {
        return wb;
    }
    project::WorksheetDef& ws = wb.worksheets[static_cast<std::size_t>(sheet)];
    int z = 0;
    for (const project::ComponentSpec& c : ws.components) {
        z = std::max(z, c.z + 1);
    }
    for (project::ComponentSpec& c : components) {
        c.id = uuidRandom(); // a paste never duplicates an id (undo merge, properties identity, build keys)
        c.z = z++;
        ws.components.push_back(std::move(c));
    }
    return wb;
}

project::WorkbookFile withComponentsRemoved(const ProjectSession& session, int workbook, int sheet,
                                            const std::vector<Uuid>& ids) {
    project::WorkbookFile wb = session.workbooks().at(static_cast<std::size_t>(workbook));
    if (!validIndex(sheet, wb.worksheets.size())) {
        return wb;
    }
    auto& list = wb.worksheets[static_cast<std::size_t>(sheet)].components;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&ids](const project::ComponentSpec& c) {
                                  return std::find(ids.begin(), ids.end(), c.id) != ids.end();
                              }),
               list.end());
    return wb;
}

} // namespace

AddWorksheetCommand::AddWorksheetCommand(ProjectSession& session, int workbook, project::WorksheetDef sheet,
                                         int index)
    : WorkbookCommand(session, workbook, withSheetAdded(session, workbook, std::move(sheet), index),
                      QStringLiteral("Add worksheet")) {}

RemoveWorksheetCommand::RemoveWorksheetCommand(ProjectSession& session, int workbook, int sheet)
    : WorkbookCommand(session, workbook, withSheetRemoved(session, workbook, sheet),
                      QStringLiteral("Delete worksheet")) {}

RenameWorksheetCommand::RenameWorksheetCommand(ProjectSession& session, int workbook, int sheet,
                                               std::string name)
    : WorkbookCommand(session, workbook, withSheetRenamed(session, workbook, sheet, name),
                      QStringLiteral("Rename worksheet")) {}

AddComponentsCommand::AddComponentsCommand(ProjectSession& session, int workbook, int sheet,
                                           std::vector<project::ComponentSpec> components,
                                           const QString& text)
    : WorkbookCommand(session, workbook, withComponentsAdded(session, workbook, sheet, std::move(components)),
                      text) {}

RemoveComponentsCommand::RemoveComponentsCommand(ProjectSession& session, int workbook, int sheet,
                                                 std::vector<Uuid> ids, const QString& text)
    : WorkbookCommand(session, workbook, withComponentsRemoved(session, workbook, sheet, ids), text) {}

QByteArray componentsMime(const std::vector<project::ComponentSpec>& components) {
    QJsonArray list;
    for (const project::ComponentSpec& c : components) {
        const std::string text = c.toJson();
        if (!text.empty()) {
            list.append(QJsonDocument::fromJson(QByteArray::fromStdString(text)).object());
        }
    }
    QJsonObject root;
    root.insert(QStringLiteral("components"), list);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::vector<project::ComponentSpec> componentsFromMime(const QByteArray& data) {
    std::vector<project::ComponentSpec> out;
    const QJsonArray list =
        QJsonDocument::fromJson(data).object().value(QStringLiteral("components")).toArray();
    for (const QJsonValue& v : list) {
        if (!v.isObject()) {
            continue;
        }
        const QByteArray text = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
        if (Result<project::ComponentSpec> c = project::ComponentSpec::parse(text.toStdString())) {
            out.push_back(std::move(c).value());
        }
    }
    return out;
}

// ---- workbook CRUD ----------------------------------------------------------------------------------------

namespace {

WorkbookSet withWorkbookAdded(const ProjectSession& session, project::WorkbookFile workbook, int index) {
    WorkbookSet set = session.workbookSet();
    if (workbook.id.isNil()) {
        workbook.id = uuidRandom();
    }
    const std::string name =
        uniqueName(workbook.name.empty() ? std::string("Workbook") : workbook.name, set.names);
    workbook.name = name;
    const auto n = static_cast<int>(set.workbooks.size());
    const int at = index < 0 || index > n ? n : index;
    set.workbooks.insert(set.workbooks.begin() + at, std::move(workbook));
    set.names.insert(set.names.begin() + at, name);
    return set;
}

WorkbookSet withWorkbookRemoved(const ProjectSession& session, int index) {
    WorkbookSet set = session.workbookSet();
    // The last workbook stays (a project always shows one, 06 section 4.1).
    if (validIndex(index, set.workbooks.size()) && set.workbooks.size() > 1) {
        set.workbooks.erase(set.workbooks.begin() + index);
        set.names.erase(set.names.begin() + index);
    }
    return set;
}

WorkbookSet withWorkbookRenamed(const ProjectSession& session, int index, const std::string& name) {
    WorkbookSet set = session.workbookSet();
    if (validIndex(index, set.workbooks.size()) && !name.empty()) {
        std::vector<std::string> others = set.names;
        others.erase(others.begin() + index);
        const std::string unique = uniqueName(name, others);
        set.workbooks[static_cast<std::size_t>(index)].name = unique;
        set.names[static_cast<std::size_t>(index)] = unique;
    }
    return set;
}

WorkbookSet withWorkbookMoved(const ProjectSession& session, int from, int to) {
    WorkbookSet set = session.workbookSet();
    const std::size_t n = set.workbooks.size();
    if (!validIndex(from, n) || !validIndex(to, n) || from == to) {
        return set;
    }
    const auto move = [from, to](auto& v) {
        auto item = std::move(v[static_cast<std::size_t>(from)]);
        v.erase(v.begin() + from);
        v.insert(v.begin() + to, std::move(item));
    };
    move(set.workbooks);
    move(set.names);
    return set;
}

} // namespace

WorkbookSetCommand::WorkbookSetCommand(ProjectSession& session, WorkbookSet after, const QString& text)
    : DocumentCommand<WorkbookSet>(text, session.workbookSet(), std::move(after),
                                   [&session](const WorkbookSet& doc) { session.setWorkbookSet(doc); }) {}

AddWorkbookCommand::AddWorkbookCommand(ProjectSession& session, project::WorkbookFile workbook, int index)
    : WorkbookSetCommand(session, withWorkbookAdded(session, std::move(workbook), index),
                         QStringLiteral("Add workbook")) {}

RemoveWorkbookCommand::RemoveWorkbookCommand(ProjectSession& session, int index)
    : WorkbookSetCommand(session, withWorkbookRemoved(session, index), QStringLiteral("Delete workbook")) {}

RenameWorkbookCommand::RenameWorkbookCommand(ProjectSession& session, int index, std::string name)
    : WorkbookSetCommand(session, withWorkbookRenamed(session, index, name),
                         QStringLiteral("Rename workbook")) {}

MoveWorkbookCommand::MoveWorkbookCommand(ProjectSession& session, int from, int to)
    : WorkbookSetCommand(session, withWorkbookMoved(session, from, to), QStringLiteral("Move workbook")) {}

// ---- channel documents ------------------------------------------------------------------------------------

namespace {

project::ChannelOverrides overridesWith(const ProjectSession& session, const std::string& channel,
                                        project::ChannelOverrides::Entry entry) {
    project::ChannelOverrides doc = session.overrides();
    doc.set(channel, std::move(entry));
    return doc;
}

} // namespace

SetOverrideCommand::SetOverrideCommand(ProjectSession& session, const std::string& channel,
                                       project::ChannelOverrides::Entry entry)
    : DocumentCommand<project::ChannelOverrides>(
          QStringLiteral("Edit channel %1").arg(QString::fromStdString(channel)), session.overrides(),
          overridesWith(session, channel, std::move(entry)),
          setterOf<project::ChannelOverrides>(session, &ProjectSession::setOverrides)) {}

ClearOverrideCommand::ClearOverrideCommand(ProjectSession& session, const std::string& channel)
    : DocumentCommand<project::ChannelOverrides>(
          QStringLiteral("Clear override %1").arg(QString::fromStdString(channel)), session.overrides(),
          overridesWith(session, channel, {}),
          setterOf<project::ChannelOverrides>(session, &ProjectSession::setOverrides)) {}

EditAliasesCommand::EditAliasesCommand(ProjectSession& session, project::Aliases after, const QString& text)
    : DocumentCommand<project::Aliases>(text, session.aliases(), std::move(after),
                                        setterOf<project::Aliases>(session, &ProjectSession::setAliases)) {}

EditMappingsCommand::EditMappingsCommand(ProjectSession& session, project::Mappings after,
                                         const QString& text)
    : DocumentCommand<project::Mappings>(text, session.mappings(), std::move(after),
                                         setterOf<project::Mappings>(session, &ProjectSession::setMappings)) {
}

namespace {

formats::UnitMappingProfile unitMapOf(const ProjectSession& session, const std::string& stem) {
    const auto it = session.unitMaps().find(stem);
    return it == session.unitMaps().end() ? formats::UnitMappingProfile{} : it->second;
}

} // namespace

SetUnitMapCommand::SetUnitMapCommand(ProjectSession& session, std::string stem,
                                     formats::UnitMappingProfile after, const QString& text)
    : DocumentCommand<formats::UnitMappingProfile>(
          text, unitMapOf(session, stem), std::move(after),
          [&session, stem](const formats::UnitMappingProfile& doc) { session.setUnitMap(stem, doc); }),
      stem_(stem) {}

void applyMappingDecision(ProjectSession& session, SourceLoader& loader, int pendingId,
                          formats::MappingDecision decision) {
    const formats::PendingResolution* pending = loader.pendingMapping(pendingId);
    if (pending == nullptr) {
        return;
    }
    // The project Mappings rows of the analysis names (05 section 8.1: the analysis name writes a project
    // Mapping, D-013); a logged name mapped already gets the new target.
    project::Mappings mappings = session.mappings();
    bool mappingsChanged = false;
    const std::shared_ptr<const units::UnitRegistry> registry = session.units();
    for (const model::MappingRow& r : decision.mappings) {
        if (r.from.empty() || r.to.empty()) {
            continue;
        }
        project::Mappings::Row row;
        row.from = r.from;
        row.to = r.to;
        row.unitlessOnly = r.unitlessOnly;
        if (r.unit && registry != nullptr && registry->contains(*r.unit)) {
            row.unit = registry->unit(*r.unit).symbol;
        }
        const auto it =
            std::find_if(mappings.rows.begin(), mappings.rows.end(), [&row](const project::Mappings::Row& e) {
                return caseFold(e.from) == caseFold(row.from);
            });
        if (it == mappings.rows.end()) {
            mappings.rows.push_back(std::move(row));
            mappingsChanged = true;
        } else if (!(*it == row)) {
            *it = std::move(row);
            mappingsChanged = true;
        }
    }
    const bool saveMap = decision.saveUnitMap && decision.unitMap.has_value();
    if (mappingsChanged || saveMap) {
        session.undoStack().beginMacro(QStringLiteral("Resolve mapping"));
        if (mappingsChanged) {
            session.undoStack().push(
                new EditMappingsCommand(session, std::move(mappings), QStringLiteral("Edit mappings")));
        }
        if (saveMap) {
            formats::UnitMappingProfile map = *decision.unitMap;
            if (map.importer.empty()) {
                map.importer = pending->importerId;
            }
            const std::string stem = formats::UnitMappingProfile::fileStem(
                pending->importerId.empty() ? map.importer : pending->importerId);
            session.undoStack().push(new SetUnitMapCommand(session, stem, std::move(map)));
        }
        session.undoStack().endMacro();
    }
    loader.resolveMapping(pendingId, std::move(decision));
}

// ---- maths modules and options ----------------------------------------------------------------------------

EditMathModulesCommand::EditMathModulesCommand(ProjectSession& session, std::vector<maths::MathModule> after,
                                               const QString& text)
    : DocumentCommand<std::vector<maths::MathModule>>(
          text, session.mathsModules(), std::move(after),
          setterOf<std::vector<maths::MathModule>>(session, &ProjectSession::setMathsModules)) {}

SetProjectOptionsCommand::SetProjectOptionsCommand(ProjectSession& session, project::ProjectFile after,
                                                   const QString& text)
    : DocumentCommand<project::ProjectFile>(
          text, session.projectFile(), std::move(after),
          setterOf<project::ProjectFile>(session, &ProjectSession::setProjectFile)) {}

// ---- sidecar edits ----------------------------------------------------------------------------------------

namespace {

model::SidecarDocument withDetail(const model::DataSource& source, const std::string& layer,
                                  const std::string& id, const std::optional<model::DetailValue>& value) {
    model::SidecarDocument doc = sidecarOf(source);
    if (value.has_value()) {
        model::Detail d;
        d.id = id;
        d.value = *value;
        doc.setDetail(layer, std::move(d));
    } else {
        doc.revertDetail(layer, id);
    }
    return doc;
}

model::SidecarDocument withTrust(const model::DataSource& source, const std::string& layer,
                                 const std::string& groupPath, const model::Range& range, bool trusted) {
    model::SidecarDocument doc = sidecarOf(source);
    model::RangeOverride ov = overrideFor(doc, layer, groupPath, range);
    ov.trusted = trusted;
    doc.setRangeOverride(layer, std::move(ov));
    return doc;
}

model::SidecarDocument withOffset(const model::DataSource& source, const std::string& layer,
                                  const std::string& groupPath, const model::Range& range, Micros offsetUs,
                                  double offsetM) {
    model::SidecarDocument doc = sidecarOf(source);
    model::RangeOverride ov = overrideFor(doc, layer, groupPath, range);
    // Zero offsets remove the keys (02 section 5.3 rule 7: pressing O again zeroes them).
    ov.offsetUs = offsetUs != 0 ? std::optional<Micros>(offsetUs) : std::nullopt;
    ov.offsetM = offsetM != 0.0 ? std::optional<double>(offsetM) : std::nullopt;
    doc.setRangeOverride(layer, std::move(ov));
    return doc;
}

template <class Edit> model::SidecarDocument edited(const model::DataSource& source, Edit edit) {
    model::SidecarDocument doc = sidecarOf(source);
    edit(doc);
    return doc;
}

} // namespace

SetDetailCommand::SetDetailCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                                   std::string detailId, std::optional<model::DetailValue> value)
    : SidecarCommand(loader, source.id, sidecarOf(source), withDetail(source, layer, detailId, value),
                     QStringLiteral("Edit detail %1").arg(QString::fromStdString(detailId))) {}

SetRangeTrustCommand::SetRangeTrustCommand(SourceLoader& loader, const model::DataSource& source,
                                           std::string layer, std::string groupPath,
                                           const model::Range& range, bool trusted)
    : SidecarCommand(loader, source.id, sidecarOf(source),
                     withTrust(source, layer, groupPath, range, trusted),
                     trusted ? QStringLiteral("Trust %1").arg(QString::fromStdString(range.name))
                             : QStringLiteral("Untrust %1").arg(QString::fromStdString(range.name))) {}

OffsetRangeCommand::OffsetRangeCommand(SourceLoader& loader, const model::DataSource& source,
                                       std::string layer, std::string groupPath, const model::Range& range,
                                       Micros offsetUs, double offsetM)
    : SidecarCommand(loader, source.id, sidecarOf(source),
                     withOffset(source, layer, groupPath, range, offsetUs, offsetM),
                     QStringLiteral("Offset %1").arg(QString::fromStdString(range.name))) {}

InsertMarkerCommand::InsertMarkerCommand(SourceLoader& loader, const model::DataSource& source,
                                         std::string layer, model::Marker marker)
    : SidecarCommand(loader, source.id, sidecarOf(source),
                     edited(source,
                            [&layer, &marker](model::SidecarDocument& doc) {
                                doc.addUserMarker(layer, std::move(marker));
                            }),
                     QStringLiteral("Insert beacon")) {}

RemoveMarkerCommand::RemoveMarkerCommand(SourceLoader& loader, const model::DataSource& source,
                                         std::string layer, model::MarkerId marker)
    : SidecarCommand(
          loader, source.id, sidecarOf(source),
          edited(source,
                 [&layer, &marker](model::SidecarDocument& doc) { doc.removeUserMarker(layer, marker); }),
          QStringLiteral("Delete beacon")) {}

MarkerEditCommand::MarkerEditCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                                     model::MarkerEdit edit)
    : SidecarCommand(loader, source.id, sidecarOf(source),
                     edited(source,
                            [&layer, &edit](model::SidecarDocument& doc) {
                                model::SidecarDocument trial = doc;
                                if (trial.applyEdit(layer, edit).ok()) {
                                    doc = std::move(trial);
                                }
                            }),
                     QStringLiteral("Edit beacon")) {}

RestoreLoggedLapsCommand::RestoreLoggedLapsCommand(SourceLoader& loader, const model::DataSource& source,
                                                   std::string layer)
    : SidecarCommand(loader, source.id, sidecarOf(source),
                     edited(source, [&layer](model::SidecarDocument& doc) { doc.restoreLogged(layer); }),
                     QStringLiteral("Restore logged laps")) {}

SetAnnotationCommand::SetAnnotationCommand(SourceLoader& loader, const model::DataSource& source,
                                           std::string layer, model::Annotation annotation)
    : SidecarCommand(loader, source.id, sidecarOf(source),
                     edited(source,
                            [&layer, &annotation](model::SidecarDocument& doc) {
                                doc.setAnnotation(layer, std::move(annotation));
                            }),
                     QStringLiteral("Edit annotation")) {}

RemoveAnnotationCommand::RemoveAnnotationCommand(SourceLoader& loader, const model::DataSource& source,
                                                 std::string layer, Uuid id)
    : SidecarCommand(
          loader, source.id, sidecarOf(source),
          edited(source, [&layer, &id](model::SidecarDocument& doc) { doc.removeAnnotation(layer, id); }),
          QStringLiteral("Delete annotation")) {}

SetLocalMathsCommand::SetLocalMathsCommand(SourceLoader& loader, const model::DataSource& source,
                                           std::string layer, std::optional<model::OpaqueJson> module,
                                           const QString& text)
    : SidecarCommand(loader, source.id, sidecarOf(source),
                     edited(source,
                            [&layer, &module](model::SidecarDocument& doc) {
                                doc.setLocalMaths(layer, std::move(module));
                            }),
                     text) {}

} // namespace esses::qt
