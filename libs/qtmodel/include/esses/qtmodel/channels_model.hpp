// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelsModel (06 section 7): the tree behind the Channels window (C). Level 1 = source (plus "Project" for
// the source-independent constants), level 2 = category (Logged, Maths, Alias, Constants), level 3 = the
// importer's channel groups (flat when none), level 4 = channels "name (unit)" (italic when Pending, red when
// Error). Sorted with logical case-insensitive order. Updates coalesced to <= 10 Hz. GUI thread only.
// Search (06 section 7 item 3, M2): filterAccepts implements the word-prefix semantics: the query and the
// name are split into words on whitespace, '_', '-', '/', '(', ')', '[' and ']'; every query word must be a
// case-insensitive prefix of a distinct name word, in order ("W S FL" finds "Wheel Speed FL", "temp (ecu)"
// finds "Water Temp (ECU)"); loose (Options > General "loose name match", ProjectSession::looseChannelSearch)
// makes each word a substring match. The Channel Editor's filter uses the same function (06 section 13.1 item
// 2). Details: categories without channels are omitted; Maths lists the enabled chain heads (a Disabled
// output, e.g. an item of a disabled module, is not a channel); Alias lists the project's aliases
// that resolve in the source (through the logged channels or a maths output); Details and Mapped arrive with
// their editors; level 3 is flat (no importer delivers channel groups yet); StatusRole carries
// model::ChannelStatus (Pending: italic, Error: red, styled by the view; Qt Core has no QFont/QColor);
// sources follow the Data-window order.
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

    // The words of a channel name or a search query (06 section 7 item 3).
    static QStringList searchWords(const QString& text);
    // The word-prefix search: every query word a case-insensitive prefix (loose: substring) of a distinct
    // name word, in order; an empty query accepts everything.
    static bool filterAccepts(const QString& channelName, const QString& query, bool loose = false);
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
