// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The undo commands (D-032, D-161; 02 section 7, 06 section 1 rule 4; AC-M2-09): one QUndoCommand per
// user-visible document edit, pushed on ProjectSession::undoStack(). Every command holds the document before
// and after the edit by value and writes one or the other in redo()/undo(), so undo/redo replays are exact
// and selection, cursor, zoom, domain, dock layout and file open/close are never on the stack. Two bases:
//   DocumentCommand<Doc>  a project document written through a ProjectSession setter (or any setter);
//   SidecarCommand        a source's sidecar, committed through SourceLoader::commitSidecar, which
//   republishes
//                         the source (new version, synchronous phase-1 range rebuild, sidecar write).
// The named commands compute their "after" document in the constructor from the current state. Consecutive
// edits with the same mergeId() (slider-style edits, drags of one component) merge into one command.
//
// Coverage of D-032 / 02 section 7 in 0.1: workbook CRUD (WorkbookSetCommand and the Add/Remove/Rename/Move
// workbook commands), worksheet CRUD and reorder, component add/paste/cut/delete/move/resize/z-order/property
// edits (WorkbookCommand, AddComponentsCommand, RemoveComponentsCommand; componentsMime for the clipboard),
// channel overrides, aliases, mappings and unit maps, maths modules, project options, and every sidecar edit.
// Track-map section edits arrive with the track map document (M3) and scheme edits with the scheme editor
// (v1.0; schemes are edited as JSON in 0.1, 06 section 13.9). GUI thread only.
#pragma once

#include <esses/base/time.hpp>
#include <esses/base/uuid.hpp>
#include <esses/formats/mapping.hpp>
#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/range.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/project/aliases.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/project/component_spec.hpp>
#include <esses/project/mappings.hpp>
#include <esses/project/project_file.hpp>
#include <esses/project/workbook_file.hpp>
#include <esses/qtmodel/project_session.hpp>

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QUndoCommand>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace esses::qt {

class ProjectSession;
class SourceLoader;

// Merge ids (QUndoCommand::id()) of the commands that merge consecutive edits.
enum class UndoMergeId : int { None = -1, ComponentGeometry = 1, ComponentSpec = 2, ChannelOverride = 3 };

// A document edit applied through `apply` (a ProjectSession setter): redo() writes `after`, undo() `before`.
template <class Doc> class DocumentCommand : public QUndoCommand {
public:
    using Apply = std::function<void(const Doc&)>;

    DocumentCommand(const QString& text, Doc before, Doc after, Apply apply, QUndoCommand* parent = nullptr)
        : QUndoCommand(text, parent), before_(std::move(before)), after_(std::move(after)),
          apply_(std::move(apply)) {}

    void redo() override { apply_(after_); }
    void undo() override { apply_(before_); }

    const Doc& before() const noexcept { return before_; }
    const Doc& after() const noexcept { return after_; }

protected:
    void setAfter(Doc after) { after_ = std::move(after); }

private:
    Doc before_;
    Doc after_;
    Apply apply_;
};

// A sidecar edit of one open source (02 section 7 rules): redo()/undo() commit `after`/`before` through
// SourceLoader::commitSidecar; nothing happens when the source has been closed (or the loader destroyed)
// since.
class SidecarCommand : public QUndoCommand {
public:
    SidecarCommand(SourceLoader& loader, model::SourceId source, model::SidecarDocument before,
                   model::SidecarDocument after, const QString& text, QUndoCommand* parent = nullptr);

    void redo() override;
    void undo() override;

    const model::SourceId& source() const noexcept { return source_; }
    const model::SidecarDocument& before() const noexcept { return before_; }
    const model::SidecarDocument& after() const noexcept { return after_; }

protected:
    void setAfter(model::SidecarDocument after) { after_ = std::move(after); }

private:
    void commit(const model::SidecarDocument& doc);

    QPointer<SourceLoader> loader_;
    model::SourceId source_;
    model::SidecarDocument before_;
    model::SidecarDocument after_;
};

// ---- workbooks (06 sections 4.2-4.4, 12) ------------------------------------------------------------------

// Any edit of workbook `index` (component add/remove/move/resize/z-order/properties, worksheet CRUD, rename,
// linkTo, domain). `mergeId` != None merges consecutive commands of the same id and component.
class WorkbookCommand : public DocumentCommand<project::WorkbookFile> {
public:
    WorkbookCommand(ProjectSession& session, int index, project::WorkbookFile after, const QString& text,
                    UndoMergeId mergeId = UndoMergeId::None, std::string componentId = {});
    int id() const override { return static_cast<int>(mergeId_); }
    bool mergeWith(const QUndoCommand* other) override;

private:
    UndoMergeId mergeId_;
    std::string componentId_;
};

// Tab reorder by drag (06 section 4.1 item 6).
class MoveWorksheetCommand final : public WorkbookCommand {
public:
    MoveWorksheetCommand(ProjectSession& session, int workbook, int from, int to);
};

// Worksheet CRUD (06 section 4.1): a new worksheet at `index` (-1: last; a nil id gets a fresh one, a name
// already used gets a number), the removal of worksheet `sheet`, and its rename.
class AddWorksheetCommand final : public WorkbookCommand {
public:
    AddWorksheetCommand(ProjectSession& session, int workbook, project::WorksheetDef sheet, int index = -1);
};

class RemoveWorksheetCommand final : public WorkbookCommand {
public:
    RemoveWorksheetCommand(ProjectSession& session, int workbook, int sheet);
};

class RenameWorksheetCommand final : public WorkbookCommand {
public:
    RenameWorksheetCommand(ProjectSession& session, int workbook, int sheet, std::string name);
};

// Component add and paste (D-027: cut/copy/paste with the domain preserved): the components are appended to
// worksheet `sheet` above the others with fresh ids; their rects, bodies and settings are kept.
class AddComponentsCommand final : public WorkbookCommand {
public:
    AddComponentsCommand(ProjectSession& session, int workbook, int sheet,
                         std::vector<project::ComponentSpec> components,
                         const QString& text = QStringLiteral("Paste components"));
};

// Cut and delete: the components with these ids leave worksheet `sheet`.
class RemoveComponentsCommand final : public WorkbookCommand {
public:
    RemoveComponentsCommand(ProjectSession& session, int workbook, int sheet, std::vector<Uuid> ids,
                            const QString& text = QStringLiteral("Delete components"));
};

// The component clipboard (copy, cut, paste): MIME type and payload {"components": [ComponentSpec JSON]}.
inline constexpr const char* kComponentsMimeType = "application/x-esses-components";
QByteArray componentsMime(const std::vector<project::ComponentSpec>& components);
// The components of a payload (malformed entries skipped; an empty list for anything else).
std::vector<project::ComponentSpec> componentsFromMime(const QByteArray& data);

// ---- workbook CRUD (06 section 4.1; 02 section 7) ---------------------------------------------------------

// Any change of the workbook list (add, remove, rename, reorder) as the whole WorkbookSet.
class WorkbookSetCommand : public DocumentCommand<WorkbookSet> {
public:
    WorkbookSetCommand(ProjectSession& session, WorkbookSet after, const QString& text);
};

// A new workbook at `index` (-1: last); a nil id gets a fresh one and a name already used gets a number.
class AddWorkbookCommand final : public WorkbookSetCommand {
public:
    AddWorkbookCommand(ProjectSession& session, project::WorkbookFile workbook, int index = -1);
};

class RemoveWorkbookCommand final : public WorkbookSetCommand {
public:
    RemoveWorkbookCommand(ProjectSession& session, int index);
};

// Renames the workbook and its file (workbooks/<name>.workbook.json; the old file is retired on save). The
// layout keeps its perspective: it is keyed by the workbook id (D-019).
class RenameWorkbookCommand final : public WorkbookSetCommand {
public:
    RenameWorkbookCommand(ProjectSession& session, int index, std::string name);
};

class MoveWorkbookCommand final : public WorkbookSetCommand {
public:
    MoveWorkbookCommand(ProjectSession& session, int from, int to);
};

// ---- channel documents (06 section 13.1) ------------------------------------------------------------------

class SetOverrideCommand final : public DocumentCommand<project::ChannelOverrides> {
public:
    SetOverrideCommand(ProjectSession& session, const std::string& channel,
                       project::ChannelOverrides::Entry entry);
};

// "Clear override"; also the undoable removal of an override that cannot apply to a newly opened file (06
// section 13.1 item 6).
class ClearOverrideCommand final : public DocumentCommand<project::ChannelOverrides> {
public:
    ClearOverrideCommand(ProjectSession& session, const std::string& channel);
};

class EditAliasesCommand final : public DocumentCommand<project::Aliases> {
public:
    EditAliasesCommand(ProjectSession& session, project::Aliases after, const QString& text);
};

class EditMappingsCommand final : public DocumentCommand<project::Mappings> {
public:
    EditMappingsCommand(ProjectSession& session, project::Mappings after, const QString& text);
};

// channels/unitmaps/<stem>.json (05 section 8.2; the Mapping panel's Save): `after` replaces the map of the
// importer stem; a profile without rules, importer and unknown members removes it.
class SetUnitMapCommand final : public DocumentCommand<formats::UnitMappingProfile> {
public:
    SetUnitMapCommand(ProjectSession& session, std::string stem, formats::UnitMappingProfile after,
                      const QString& text = QStringLiteral("Save unit map"));
    const std::string& stem() const noexcept { return stem_; }

private:
    std::string stem_;
};

// The Mapping panel's Resolve / Open anyway (05 section 8.1, 06 section 13.8 item 4; decisions are undoable
// project edits): one macro "Resolve mapping" holding an EditMappingsCommand with the decision's analysis
// names added to the project's Mappings (a row for an already mapped logged name replaces it) and, when
// decision.saveUnitMap, a SetUnitMapCommand with decision.unitMap for the pending open's importer; then
// SourceLoader::resolveMapping submits task B with the decision. Nothing is pushed for an empty decision.
void applyMappingDecision(ProjectSession& session, SourceLoader& loader, int pendingId,
                          formats::MappingDecision decision);

// ---- maths modules (03 section 11 item 9, 06 section 13.2) ------------------------------------------------

// Item/constant/enumeration CRUD, enable, condition and module order (the whole ordered module set).
class EditMathModulesCommand final : public DocumentCommand<std::vector<maths::MathModule>> {
public:
    EditMathModulesCommand(ProjectSession& session, std::vector<maths::MathModule> after,
                           const QString& text);
};

// ---- project options (06 section 13.7) --------------------------------------------------------------------

class SetProjectOptionsCommand final : public DocumentCommand<project::ProjectFile> {
public:
    SetProjectOptionsCommand(ProjectSession& session, project::ProjectFile after, const QString& text);
};

// ---- sidecar edits (06 sections 5.2, 13.3, 13.4; 02 section 4.3) ------------------------------------------

// A detail edit with provenance User (the original value kept, D-015); nullopt restores the original.
class SetDetailCommand final : public SidecarCommand {
public:
    SetDetailCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                     std::string detailId, std::optional<model::DetailValue> value);
};

// Trusted flag of a range, keyed (groupPath, startMarker) (D-014); offsets of the same override are kept.
class SetRangeTrustCommand final : public SidecarCommand {
public:
    SetRangeTrustCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                         std::string groupPath, const model::Range& range, bool trusted);
};

// Alignment offset of a range (O axis, display only; 04 section 3.3), written when the drag ends.
class OffsetRangeCommand final : public SidecarCommand {
public:
    OffsetRangeCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                       std::string groupPath, const model::Range& range, Micros offsetUs, double offsetM);
};

// A user marker (SHIFT+F7, Lap Editor insert); laps rebuild synchronously (AC-M2-05).
class InsertMarkerCommand final : public SidecarCommand {
public:
    InsertMarkerCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                        model::Marker marker);
};

class RemoveMarkerCommand final : public SidecarCommand {
public:
    RemoveMarkerCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                        model::MarkerId marker);
};

// MarkerEdit offset / ignore / restore on one marker, or the layer's offsetDistance (beacon offset by
// distance, 06 section 13.3 item 3; one command per Apply).
class MarkerEditCommand final : public SidecarCommand {
public:
    MarkerEditCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                      model::MarkerEdit edit);
};

// "Restore logged laps" (06 section 13.3 item 6): every MarkerEdit and user marker of the layer, one command.
class RestoreLoggedLapsCommand final : public SidecarCommand {
public:
    RestoreLoggedLapsCommand(SourceLoader& loader, const model::DataSource& source, std::string layer);
};

// Annotation insert/replace (by id) or removal (nullopt annotation with an id).
class SetAnnotationCommand final : public SidecarCommand {
public:
    SetAnnotationCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                         model::Annotation annotation);
};

class RemoveAnnotationCommand final : public SidecarCommand {
public:
    RemoveAnnotationCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                            Uuid id);
};

// Local maths of a source (sidecar localMaths; scale/offset, zero at cursor, the Maths Editor's Local Maths).
class SetLocalMathsCommand final : public SidecarCommand {
public:
    SetLocalMathsCommand(SourceLoader& loader, const model::DataSource& source, std::string layer,
                         std::optional<model::OpaqueJson> module, const QString& text);
};

} // namespace esses::qt
