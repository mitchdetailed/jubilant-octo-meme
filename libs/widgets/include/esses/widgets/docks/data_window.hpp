// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataWindow, dock id "data", F8 (06 sections 3.2, 5.2-5.3 and 6; AC-M1-01): a QTreeView over
// qt::DataWindowModel (sources > ranges; outings when a source has several) with the selection slot columns
// M (red dot on Main), O (Overlay 0), 1 (overlay number), Name (italic and greyed when untrusted), Time (lap
// time), Delta Fastest, Offset, Trusted (checkbox -> DataSelectionModel::setTrusted) and Local maths. The
// slot columns are drawn in the scheme's key colours (cursorMain, overlay0, overlayPalette[k-1]) with a text
// tool tip, so colour is not the only carrier (06 section 20 item 4).
// Keyboard (data_window context of the keymap, dispatched by the KeyEventRouter): UP/DOWN move the highlight
// (the tree); ENTER = data.select_main, CTRL+ENTER = data.toggle_overlay0, ALT+ENTER = data.toggle_overlay on
// the highlighted range; DELETE = data.close_file on a source row (no-op on a range row); ESC returns focus
// (view.return_focus). This widget registers those four commands with handlers acting on its highlighted
// row. Mouse: a click in M / O / 1 is setMain / toggleOverlay0 / toggleOverlay; a double-click on a range
// sets it as Main; the Trusted cell is a checkbox. The highlight follows Main when Main changes by N/P/CTRL+F
// (Main forced into view). Context menu: Set Main / Toggle Overlay 0 / Toggle Overlay, Trusted, Close File
// (filters, the Lap Editor and Reveal In File Manager arrive later). M1: no filters (M2).
#pragma once

#include <esses/model/ids.hpp>
#include <esses/model/range.hpp>
#include <esses/project/scheme.hpp>

#include <QPointer>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>

class QIdentityProxyModel;
class QModelIndex;
class QTreeView;

namespace esses::qt {
class CommandRegistry;
class DataSelectionModel;
class DataWindowModel;
} // namespace esses::qt

namespace esses::widgets {

class CommandActions;

class DataWindow final : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* kDockId = "data";

    DataWindow(qt::DataSelectionModel& selection, qt::CommandRegistry& commands, QWidget* parent = nullptr);
    ~DataWindow() override;

    QTreeView& tree() noexcept { return *tree_; }
    qt::DataWindowModel& model() noexcept { return *model_; }
    // The model the tree shows (the slot styling proxy over model()).
    QIdentityProxyModel& viewModel() noexcept { return *proxy_; }

    // The key colours of the slot columns (06 section 5.3); null: neutral colours.
    void setScheme(std::shared_ptr<const project::Scheme> scheme);
    // The shared command actions for the context menu (06 section 1 rule 5); null: no context menu.
    void setActions(CommandActions* actions);
    // DELETE / Close File on a source row (the shell closes it through the SourceLoader); default:
    // DataSelectionModel::closeSource.
    void setCloseSource(std::function<void(const model::SourceId&)> close);

    // The highlighted (current) row's range, or nullopt for a source/outing row or no highlight.
    std::optional<model::RangeRef> highlightedRange() const;
    // The source of the highlighted row (any level).
    std::optional<model::SourceId> highlightedSource() const;
    bool highlightedIsSource() const;
    // Moves the highlight to a range row and scrolls it into view.
    void highlight(const model::RangeRef& range);

private:
    void registerCommands();
    void onClicked(const QModelIndex& index);
    void onModelReset();
    void followMain();
    void showContextMenu(const QPoint& pos);

    qt::DataSelectionModel& selection_;
    qt::CommandRegistry& commands_;
    qt::DataWindowModel* model_ = nullptr;
    QIdentityProxyModel* proxy_ = nullptr;
    QTreeView* tree_ = nullptr;
    QPointer<CommandActions> actions_;
    std::function<void(const model::SourceId&)> closeSource_;
};

} // namespace esses::widgets
