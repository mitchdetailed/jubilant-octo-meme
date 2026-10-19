// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataWindowModel (06 section 6): the tree behind the Data window (F8). Level 1 = source rows (details
// template, stale badge later), level 2 = outings when the linked view has more than one, level 3 = the
// ranges of the worksheet's linkTo view in start-time order. Columns: M (red dot on Main), O (Overlay 0), 1
// (overlay number), Name (italic when untrusted), Time (lap time), Delta Fastest (analysis::fastestLap rule),
// Offset, Trusted (checkbox -> DataSelectionModel::setTrusted), Local maths. M1: selection slots, lap time,
// delta to fastest, trust (07 section 4.1); filters arrive in M2. Reads DataSelectionModel; GUI thread only.
// M1 details (Qt Core only, so no QFont/QColor/QIcon): the M column shows "●" on Main, O "■" on Overlay
// 0, column 1 the overlay number; UntrustedRole (italic), SlotRole (-1 none, 0 Overlay 0, k overlay k, 100
// Main) and FilteredInRole (shown only because it holds a slot) let the view style the rows; Trusted is a
// checkbox (CheckStateRole -> DataSelectionModel::setTrusted); Time is the lap-time format m:ss.fff ("—" for
// outings); Delta Fastest is "+s.fff" against the analysis::fastestLap rule of the same source; Offset shows
// the non-zero alignment offset (time first). Rows are the displayed (filtered) ranges of the displayed
// group path; outing rows appear when the source has more than one outing.
#pragma once

#include <esses/model/range.hpp>

#include <QAbstractItemModel>
#include <QString>
#include <QVariant>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace esses::qt {

class DataSelectionModel;

class DataWindowModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    // StaleRole (M4, D-206): true on the source row of a source whose file changed on disk (below).
    enum Role { UntrustedRole = Qt::UserRole + 1, SlotRole, FilteredInRole, StaleRole };
    enum Column {
        MainCol = 0,
        Overlay0Col,
        OverlayCol,
        NameCol,
        TimeCol,
        DeltaFastestCol,
        OffsetCol,
        TrustedCol,
        LocalMathsCol,
        ColumnCount
    };

    explicit DataWindowModel(DataSelectionModel& selection, QObject* parent = nullptr);
    ~DataWindowModel() override;

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

    // The range of a level-3 row, or nullopt for source/outing rows.
    std::optional<model::RangeRef> rangeAt(const QModelIndex& index) const;
    QModelIndex indexOf(const model::RangeRef& range) const; // "Main is forced into view"
    // The source of any row (source, outing or range), or nullopt.
    std::optional<model::SourceId> sourceAt(const QModelIndex& index) const;

    // ---- M4 reload (D-206; package reload, src/m4_reload/data_window_model_stale.cpp) ----
    // The stale badge (06 section 6 item 1, 02 section 1 behaviour 5): the query answers whether a source's
    // file changed on disk after it was opened (the shell passes SourceLoader::isStale). isStale is final
    // (the query's answer, false without one). refreshStale(source) re-announces the source row after the
    // loader's staleChanged; data() of a source row answering StaleRole, and the badge in
    // its Name cell and tool tip, are the reload package's (data_window_model.cpp).
    using StaleQuery = std::function<bool(const model::SourceId&)>;
    void setStaleQuery(StaleQuery stale);
    bool isStale(const model::SourceId& source) const;
    void refreshStale(const model::SourceId& source);

private:
    struct Node;
    void rebuild();
    void emitSlotsChanged(const QModelIndex& parent);
    const Node* nodeOf(const QModelIndex& index) const;
    QModelIndex indexOfNode(const Node* node, int column) const;

    DataSelectionModel& selection_;
    std::vector<std::unique_ptr<Node>> roots_;
    StaleQuery stale_; // M4 reload
};

} // namespace esses::qt
