// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelStatusModel (06 sections 1 and 10, D-013, D-021, D-060; AC-M2-12): the item model behind
// ChannelStatusWindow (CTRL+U). Rows, in this order:
//   * every maths item and built-in output of every open source (MathsService::statusRows, shadowed and
//     ignored elements included), Kind "Expression", "Filter", "Scale & Offset", "2D Table", "3D Table",
//     "Built-in", "Plugin" or "Regression";
//   * every constant of the enabled global modules (Source "Project", Kind "Constant");
//   * every alias of the project per open source (Kind "Alias"), resolved through the project's mappings,
//     aliases and the maths outputs (a candidate that is a maths output resolves to it);
//   * every mapping of the project per open source that logs its name (Kind "Mapping"; a mapping no open
//     source uses is one Source "Project" row, "Not used");
//   * every channel a workbook component references that resolves in no way in an open source (Kind
//     "Reference", Status and Resolved to "(missing)"): missing references are shown, never dropped.
// Columns: Source, Channel, Kind, Status ("OK", "Pending", "Calculating", "Warning: <text>", "Error: <text>",
// "Disabled ..."), Resolved to (the ResolutionTrace summary; every step, one per line, in TraceRole and the
// tooltip), Rate, Unit. Updates arrive through MathsService::statusChanged and ProjectSession::
// documentsChanged coalesced to 10 Hz (refreshLater; refresh() rebuilds at once); a rebuild with the same
// rows emits dataChanged, otherwise the model is reset. GUI thread only. Additions: ModuleRole (the module
// id, for "open the Maths Editor at the item"), refreshLater(), rows().
#pragma once

#include <esses/maths/channel_key.hpp>
#include <esses/model/resolver.hpp>

#include <QAbstractTableModel>
#include <QString>
#include <optional>
#include <vector>

class QTimer;

namespace esses::qt {

class MathsService;
class ProjectSession;

class ChannelStatusModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column : int {
        SourceColumn,
        ChannelColumn,
        KindColumn,
        StatusColumn,
        ResolvedColumn,
        RateColumn,
        UnitColumn
    };
    static constexpr int kColumnCount = 7;
    enum Role : int {
        TraceRole = Qt::UserRole + 1, // QString: every ResolutionStep, one per line (the detail pane)
        ErrorRole,                    // bool: the row's status is an Error (the "errors only" toggle)
        KeyRole,                      // QString: the maths key text, for "open the Maths Editor at the item"
        ModuleRole                    // QString: the maths module id ("" for other rows)
    };

    explicit ChannelStatusModel(MathsService& maths, ProjectSession* session = nullptr,
                                QObject* parent = nullptr);
    ~ChannelStatusModel() override;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // Rebuilds the rows from MathsService::statusRows() and the project's aliases, mappings and workbooks.
    void refresh();
    // A refresh within 100 ms (10 Hz); repeated calls before it coalesce.
    void refreshLater();
    // The row of a maths output's chain head (else its first element), or -1 (channel.status,
    // CTRL+SHIFT+S, focuses it).
    int rowOf(const maths::ChannelKey& key) const;

    struct Row {
        QString id; // identity across refreshes
        QString source;
        QString channel;
        QString kind;
        QString status;
        QString resolved;
        QString trace;
        QString rate;
        QString unit;
        QString module;
        bool error = false;
        bool head = false;
        std::optional<maths::ChannelKey> key;
    };
    const std::vector<Row>& rows() const noexcept { return rows_; }

private:
    std::vector<Row> buildRows() const;

    MathsService& maths_;
    ProjectSession* session_ = nullptr;
    std::vector<Row> rows_;
    QTimer* coalesce_ = nullptr;
};

} // namespace esses::qt
