// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelsModel (06 section 7): source > category > channel, sorted logically and case-insensitively.

#include <esses/maths/channel_key.hpp>
#include <esses/qtmodel/channels_model.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>

#include <QCollator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace esses::qt {

namespace {

constexpr int kCoalesceMs = 100; // <= 10 Hz (06 section 7 item 7)

} // namespace

struct ChannelsModel::Node {
    Node* parent = nullptr;
    int row = 0;
    QString text;
    QString name; // channel rows: the canonical name
    model::ChannelStatus status = model::ChannelStatus::Ok;
    bool channel = false;
    std::vector<std::unique_ptr<Node>> children;
};

ChannelsModel::ChannelsModel(DataSelectionModel& selection, MathsService& maths, QObject* parent)
    : QAbstractItemModel(parent), selection_(selection), maths_(maths), coalesce_(new QTimer(this)) {
    coalesce_->setSingleShot(true);
    coalesce_->setInterval(kCoalesceMs);
    connect(coalesce_, &QTimer::timeout, this, &ChannelsModel::rebuild);
    connect(&selection_, &DataSelectionModel::sourcesChanged, this, &ChannelsModel::rebuild);
    connect(&maths_, &MathsService::statusChanged, this, [this] {
        if (!coalesce_->isActive()) {
            coalesce_->start();
        }
    });
    rebuild();
}

ChannelsModel::~ChannelsModel() = default;

void ChannelsModel::rebuild() {
    coalesce_->stop();
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    const auto add = [](Node& parent, QString text) -> Node& {
        auto n = std::make_unique<Node>();
        n->parent = &parent;
        n->row = static_cast<int>(parent.children.size());
        n->text = std::move(text);
        parent.children.push_back(std::move(n));
        return *parent.children.back();
    };
    const auto sortChannels = [&collator](Node& category) {
        std::stable_sort(
            category.children.begin(), category.children.end(),
            [&collator](const auto& a, const auto& b) { return collator.compare(a->name, b->name) < 0; });
        for (std::size_t i = 0; i < category.children.size(); ++i) {
            category.children[i]->row = static_cast<int>(i);
        }
    };
    const units::UnitRegistry* registry = maths_.units().get();
    const auto label = [registry](const model::ChannelDescriptor& d) {
        QString text = QString::fromStdString(d.name);
        if (registry != nullptr && registry->contains(d.unit)) {
            const std::string_view unit = registry->unit(d.unit).label();
            if (!unit.empty()) {
                text += QStringLiteral(" (") +
                        QString::fromUtf8(unit.data(), static_cast<qsizetype>(unit.size())) +
                        QLatin1Char(')');
            }
        }
        return text;
    };

    beginResetModel();
    roots_.clear();
    for (const auto& src : selection_.sources()) {
        auto root = std::make_unique<Node>();
        root->row = static_cast<int>(roots_.size());
        root->text = QString::fromStdWString(src->prov.path.filename().wstring());
        const model::DataLayer* layer = src->layer(model::kNormalLayer);
        if (layer != nullptr) {
            Node logged;
            for (const model::Channel& c : layer->channels) {
                if (c.desc.source == model::ChannelSource::Maths) {
                    continue;
                }
                Node& n = add(logged, label(c.desc));
                n.name = QString::fromStdString(c.desc.name);
                n.status = c.status;
                n.channel = true;
            }
            Node mathsNode;
            for (const maths::ChannelKey& key : maths_.outputs(src->id, model::kNormalLayer)) {
                const auto desc = maths_.descriptor(key);
                if (!desc) {
                    continue;
                }
                Node& n = add(mathsNode, label(*desc));
                n.name = QString::fromStdString(desc->name);
                n.status = maths::toChannelStatus(maths_.status(key).state);
                n.channel = true;
            }
            for (auto* category : {&logged, &mathsNode}) {
                if (category->children.empty()) {
                    continue;
                }
                Node& cat = add(*root, category == &logged ? tr("Logged") : tr("Maths"));
                cat.children = std::move(category->children);
                for (auto& child : cat.children) {
                    child->parent = &cat;
                }
                sortChannels(cat);
            }
        }
        roots_.push_back(std::move(root));
    }
    endResetModel();
}

const ChannelsModel::Node* ChannelsModel::nodeOf(const QModelIndex& index) const {
    return index.isValid() ? static_cast<const Node*>(index.internalPointer()) : nullptr;
}

QModelIndex ChannelsModel::index(int row, int column, const QModelIndex& parent) const {
    if (row < 0 || column != 0) {
        return {};
    }
    const auto& list = parent.isValid() ? nodeOf(parent)->children : roots_;
    if (static_cast<std::size_t>(row) >= list.size()) {
        return {};
    }
    return createIndex(row, column, list[static_cast<std::size_t>(row)].get());
}

QModelIndex ChannelsModel::parent(const QModelIndex& child) const {
    const Node* n = nodeOf(child);
    if (n == nullptr || n->parent == nullptr) {
        return {};
    }
    return createIndex(n->parent->row, 0, n->parent);
}

int ChannelsModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return static_cast<int>(roots_.size());
    }
    return parent.column() == 0 ? static_cast<int>(nodeOf(parent)->children.size()) : 0;
}

int ChannelsModel::columnCount(const QModelIndex&) const {
    return 1;
}

QVariant ChannelsModel::data(const QModelIndex& index, int role) const {
    const Node* n = nodeOf(index);
    if (n == nullptr) {
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
        return n->text;
    case ChannelNameRole:
        return n->channel ? QVariant(n->name) : QVariant();
    case StatusRole:
        return n->channel ? QVariant(static_cast<int>(n->status)) : QVariant();
    default:
        return {};
    }
}

bool ChannelsModel::filterAccepts(const QString& channelName, const QString& query) {
    const QStringList words = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& w : words) {
        if (!channelName.contains(w, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}

QByteArray ChannelsModel::channelsMime(const QStringList& channels, bool newPanel) {
    QJsonObject root;
    root.insert(QStringLiteral("channels"), QJsonArray::fromStringList(channels));
    root.insert(QStringLiteral("newPanel"), newPanel);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace esses::qt
