// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Data window's stale badge (data_window_model.hpp, "M4 reload"). Part of the M4 reload package's root in
// esses_qtmodel (docs/milestones/M4.md section 3); setStaleQuery and isStale are final (D-206). data() of a
// source row answers StaleRole and adds the stale line to its tool tip (data_window_model.cpp); the view
// draws the badge (widgets DataWindow).

#include <esses/qtmodel/data_window_model.hpp>

#include <utility>

namespace esses::qt {

void DataWindowModel::setStaleQuery(StaleQuery stale) {
    stale_ = std::move(stale);
}

bool DataWindowModel::isStale(const model::SourceId& source) const {
    return stale_ && stale_(source);
}

void DataWindowModel::refreshStale(const model::SourceId& source) {
    // The source row (level 1) of `source`, every column: its badge, tool tip and StaleRole changed.
    for (int row = 0; row < rowCount(); ++row) {
        const QModelIndex first = index(row, 0);
        if (sourceAt(first) == source) {
            Q_EMIT dataChanged(first, index(row, ColumnCount - 1));
        }
    }
}

} // namespace esses::qt
