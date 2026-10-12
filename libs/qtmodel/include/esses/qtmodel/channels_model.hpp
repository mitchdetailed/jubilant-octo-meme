// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelsModel (06 section 7): the tree behind the Channels window (C). Level 1 = source (plus "Project"),
// level 2 = category (Logged, Maths, Details, Alias, Mapped, Constants), level 3 = the importer's channel
// groups (flat when none), level 4 = channels "name (unit)" (italic when Pending, red when Error). Sorted
// with logical case-insensitive order. M1: plain substring filter (filterAccepts); the word-prefix search
// semantics of 06 section 7 item 3 arrive in M2. Updates coalesced to <= 10 Hz. GUI thread only.
// M1 details: categories without channels are omitted (M1 has Logged and Maths only; Details, Alias, Mapped
// and Constants arrive with their documents); level 3 is flat (the M1 importers deliver no channel groups);
// StatusRole carries model::ChannelStatus (Pending: italic, Error: red, styled by the view; Qt Core has no
// QFont/QColor); sources follow the Data-window order.
#pragma once

#include <QAbstractItemModel>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <memory>
#include <vector>

class QTimer;

namespace esses::qt {

class DataSelectionModel;
class MathsService;

class ChannelsModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    static constexpr int ChannelNameRole = Qt::UserRole + 1; // the canonical name (drag MIME, addChannel)
    static constexpr int StatusRole = Qt::UserRole + 2;      // int(model::ChannelStatus) of a channel row

    ChannelsModel(DataSelectionModel& selection, MathsService& maths, QObject* parent = nullptr);
    ~ChannelsModel() override;

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    // M1 filter: every whitespace-separated word must be a case-insensitive substring of the name.
    static bool filterAccepts(const QString& channelName, const QString& query);
    // MIME "application/x-esses-channels": {"channels": [...], "newPanel": bool}.
    static QByteArray channelsMime(const QStringList& channels, bool newPanel);

    // Rebuilds the tree now (it is otherwise rebuilt on sourcesChanged and, coalesced, on maths status).
    void rebuild();

private:
    struct Node;
    const Node* nodeOf(const QModelIndex& index) const;

    DataSelectionModel& selection_;
    MathsService& maths_;
    std::vector<std::unique_ptr<Node>> roots_;
    QTimer* coalesce_ = nullptr;
};

} // namespace esses::qt
