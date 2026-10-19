// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ValuesModel (06 section 8, 04 section 15; AC-M1-01 "cursor drag updates ValuesWindow rows for Main and
// Overlay 1"): rows = a channel list (the focused component's channelNames(), or every channel of the Main
// source with "All channels"); columns Name, Main, Overlay 0 / reference overlay, Delta, Unit. Values at the
// cursor through the single conversion rule of 04 section 3.2: x_c = map_M.xOf(M.start + cursorT),
// t_k = map_k.timeAt(x_c), Resampler::valueAt(series_k, t_k, interp) over chunks pinned with Column::tryPin
// (never blocks; "..." when absent or pending, the chunk requested at Visible, pending maths at Background).
// Refresh on the Cursor bit <= 2 ms for 250 rows: only visible rows are recomputed. Shared by the Values
// window and the values_list component. GUI thread only.
// M1 details: the reference column is the reference overlay (Overlay 0, else Overlay 1; none while F4 hides
// the overlays); Delta = Main - reference in the display unit; display units and decimals come from
// setChannelRequests (a values_list body) or the stored unit. A refresh takes the view (the cursor, the
// slots' mappers) once and marks every row out of date; a row is computed when a view asks for its cells
// (data(), cellText), so a refresh costs only the rows on screen (07 section 12 "Values window refresh, 250
// channels <= 2 ms"; D-217); a refresh is retried every 50 ms while a computed cell waits for a chunk or a
// maths output. The worksheet supplies the domain, cursor and DistanceService; without a worksheet every
// value cell is empty. rowCount() follows the effective rows (setChannels, or every channel of the Main
// source with setAllChannels(true)).
#pragma once

#include <esses/plotcore/change_mask.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/plotcore/view_state.hpp>
#include <esses/qtmodel/render_service.hpp>

#include <QAbstractTableModel>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QTimer;

namespace esses::qt {

class DataSelectionModel;
class MathsService;
class WorksheetViewModel;

class ValuesModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { NameCol = 0, MainCol, ReferenceCol, DeltaCol, UnitCol, ColumnCount };

    ValuesModel(DataSelectionModel& selection, MathsService& maths, QObject* parent = nullptr);
    ~ValuesModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void setChannels(const QStringList& names);
    // Rows with their persisted display unit and decimals (the values_list component's body).
    void setChannelRequests(const std::vector<ChannelRequest>& requests);
    QStringList channels() const { return channels_; }
    void setAllChannels(bool on);
    bool allChannels() const noexcept { return allChannels_; }
    // The worksheet whose ViewState (domain, cursor) the values follow; null = none.
    void setWorksheet(WorksheetViewModel* worksheet);
    // Added by M4 (D-211 (f)): the ViewState the values are read at instead of the worksheet's (a
    // values_list component with links.cursor or links.datum off keeps its own cursor and datum; the
    // worksheet still gives the DistanceService); empty: the worksheet's. The caller refreshes on its own
    // changes.
    using ViewSource = std::function<plot::ViewState()>;
    void setViewSource(ViewSource source);
    // Recomputes the visible rows (Cursor/Datum/Selection/Data bits).
    void refresh(esses::plot::ChangeMask what);
    // Text of a cell as displayed ("..." while pending) - for tests.
    QString cellText(int row, int column) const;

private:
    struct RowCells {
        QString main, reference, delta, unit;
        bool pending = false;
    };
    std::vector<ChannelRequest> effectiveRows() const;
    void recompute();
    void reset();

    DataSelectionModel& selection_;
    MathsService& maths_;
    WorksheetViewModel* worksheet_ = nullptr;
    ViewSource viewSource_;
    QMetaObject::Connection worksheetConnection_;
    QMetaObject::Connection destroyedConnection_;
    QStringList channels_;
    std::vector<ChannelRequest> requests_;
    bool allChannels_ = false;
    std::vector<ChannelRequest> rows_; // the effective rows
    // What one refresh read (D-217): the cells of a row are computed against it when a view first asks for
    // them.
    struct Context {
        bool live = false;
        plot::ViewState view;
        plot::SelectionSnapshot snap;
        std::optional<plot::DomainMapper> mainMap;
        std::optional<model::RangeRef> reference;
        std::optional<plot::DomainMapper> refMap;
        double xc = 0.0;
    };
    Context context_;
    std::uint64_t generation_ = 0;                  // bumps on every refresh
    mutable std::vector<RowCells> cells_;           // computed on demand
    mutable std::vector<std::uint64_t> computedAt_; // the generation each row was computed at (0: never)
    void computeRow(std::size_t row) const;
    QTimer* retry_ = nullptr;
};

} // namespace esses::qt
