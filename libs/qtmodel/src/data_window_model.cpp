// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataWindowModel (06 section 6; 07 section 4.1: selection slots, lap time, delta to fastest, trust).

#include "range_lookup.hpp"

#include <esses/analysis/fastest_lap.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>

#include <cmath>
#include <span>
#include <utility>

namespace esses::qt {

namespace {

constexpr int kMainSlot = 100;

QString lapTime(Micros us) {
    const auto ms = static_cast<long long>(std::llround(static_cast<double>(us) / 1000.0));
    const long long minutes = ms / 60000;
    const long long rest = ms % 60000;
    return QStringLiteral("%1:%2.%3")
        .arg(minutes)
        .arg(rest / 1000, 2, 10, QLatin1Char('0'))
        .arg(rest % 1000, 3, 10, QLatin1Char('0'));
}

QString signedSeconds(double seconds) {
    return (seconds >= 0.0 ? QStringLiteral("+") : QString()) + QString::number(seconds, 'f', 3);
}

QString sourceLabel(const model::DataSource& src) {
    for (const model::Detail& d : src.details) {
        if (d.id == "File Name") {
            if (const auto* s = std::get_if<std::string>(&d.value)) {
                return QString::fromStdString(*s);
            }
        }
    }
    return QString::fromStdWString(src.prov.path.filename().wstring());
}

} // namespace

struct DataWindowModel::Node {
    enum class Kind : std::uint8_t { Source, Outing, Range };
    Kind kind = Kind::Source;
    Node* parent = nullptr;
    int row = 0;
    model::SourceId src;
    std::optional<model::RangeRef> ref; // Outing and Range rows
    QString label;
    std::vector<std::unique_ptr<Node>> children;
};

DataWindowModel::DataWindowModel(DataSelectionModel& selection, QObject* parent)
    : QAbstractItemModel(parent), selection_(selection) {
    connect(&selection_, &DataSelectionModel::sourcesChanged, this, [this] { rebuild(); });
    connect(&selection_, &DataSelectionModel::filterChanged, this, [this] { rebuild(); });
    connect(&selection_, &DataSelectionModel::changed, this,
            [this](const plot::SelectionSnapshot&, DataSelectionModel::Changes what) {
                if (what.testFlag(DataSelectionModel::Change::Sources)) {
                    rebuild();
                    return;
                }
                // A filter keeps the slots visible: a slot change may add or remove rows.
                bool filtered = false;
                for (const auto& s : selection_.sources()) {
                    filtered = filtered || selection_.filter(s->id).kind != FilterSpec::Kind::All;
                }
                if (filtered) {
                    rebuild();
                } else {
                    emitSlotsChanged(QModelIndex());
                }
            });
    rebuild();
}

DataWindowModel::~DataWindowModel() = default;

void DataWindowModel::rebuild() {
    beginResetModel();
    roots_.clear();
    for (const auto& src : selection_.sources()) {
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Source;
        node->row = static_cast<int>(roots_.size());
        node->src = src->id;
        node->label = sourceLabel(*src);
        const std::string groupPath =
            detail::groupOf(*src, model::kNormalLayer, selection_.displayedGroupPath()) != nullptr
                ? selection_.displayedGroupPath()
                : std::string(detail::kOutingsGroup);
        const std::vector<model::RangeRef> shown =
            selection_.displayedRanges(src->id, model::kNormalLayer, groupPath);
        const model::RangeGroup* outings = detail::groupOf(*src, model::kNormalLayer, detail::kOutingsGroup);
        const bool outingLevel =
            groupPath != detail::kOutingsGroup && outings != nullptr && outings->ranges.size() > 1;
        const auto addRange = [](Node& parent, const model::RangeRef& ref) {
            auto r = std::make_unique<Node>();
            r->kind = Node::Kind::Range;
            r->parent = &parent;
            r->row = static_cast<int>(parent.children.size());
            r->src = ref.src;
            r->ref = ref;
            r->label = QString::fromStdString(ref.name);
            parent.children.push_back(std::move(r));
        };
        if (outingLevel) {
            for (const model::Range& o : outings->ranges) {
                auto on = std::make_unique<Node>();
                on->kind = Node::Kind::Outing;
                on->parent = node.get();
                on->row = static_cast<int>(node->children.size());
                on->src = src->id;
                on->ref = detail::makeRef(*src, model::kNormalLayer, detail::kOutingsGroup, o);
                on->label = QString::fromStdString(o.name);
                for (const model::RangeRef& ref : shown) {
                    const model::Range* r = detail::findRange(*src, ref);
                    if (r != nullptr && r->start >= o.start && r->start < o.end) {
                        addRange(*on, ref);
                    }
                }
                node->children.push_back(std::move(on));
            }
        } else {
            for (const model::RangeRef& ref : shown) {
                addRange(*node, ref);
            }
        }
        roots_.push_back(std::move(node));
    }
    endResetModel();
}

void DataWindowModel::emitSlotsChanged(const QModelIndex& parent) {
    const int rows = rowCount(parent);
    if (rows == 0) {
        return;
    }
    Q_EMIT dataChanged(index(0, 0, parent), index(rows - 1, ColumnCount - 1, parent));
    for (int r = 0; r < rows; ++r) {
        emitSlotsChanged(index(r, 0, parent));
    }
}

const DataWindowModel::Node* DataWindowModel::nodeOf(const QModelIndex& index) const {
    return index.isValid() ? static_cast<const Node*>(index.internalPointer()) : nullptr;
}

QModelIndex DataWindowModel::indexOfNode(const Node* node, int column) const {
    return node == nullptr ? QModelIndex() : createIndex(node->row, column, const_cast<Node*>(node));
}

QModelIndex DataWindowModel::index(int row, int column, const QModelIndex& parent) const {
    if (row < 0 || column < 0 || column >= ColumnCount) {
        return {};
    }
    const auto& list = parent.isValid() ? nodeOf(parent)->children : roots_;
    if (static_cast<std::size_t>(row) >= list.size()) {
        return {};
    }
    return createIndex(row, column, list[static_cast<std::size_t>(row)].get());
}

QModelIndex DataWindowModel::parent(const QModelIndex& child) const {
    const Node* n = nodeOf(child);
    return n == nullptr || n->parent == nullptr ? QModelIndex() : indexOfNode(n->parent, 0);
}

int DataWindowModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return static_cast<int>(roots_.size());
    }
    if (parent.column() != 0) {
        return 0;
    }
    return static_cast<int>(nodeOf(parent)->children.size());
}

int DataWindowModel::columnCount(const QModelIndex&) const {
    return ColumnCount;
}

QVariant DataWindowModel::data(const QModelIndex& index, int role) const {
    const Node* n = nodeOf(index);
    if (n == nullptr) {
        return {};
    }
    const auto src = selection_.source(n->src);
    if (src == nullptr) {
        return {};
    }
    if (n->kind == Node::Kind::Source) {
        if (role == Qt::DisplayRole && index.column() == NameCol) {
            return n->label;
        }
        if (role == Qt::ToolTipRole) {
            return QString::fromStdWString(src->prov.path.wstring()) + QStringLiteral(" (") +
                   QString::fromStdString(src->prov.importerId) + QLatin1Char(')');
        }
        return {};
    }
    const model::Range* range = n->ref ? detail::findRange(*src, *n->ref) : nullptr;
    if (range == nullptr) {
        return {};
    }
    const plot::SelectionSnapshot& snap = selection_.snapshot();
    const bool isMain = selection_.hasMain() && snap.main == *n->ref;
    const int number = selection_.overlayNumber(*n->ref);
    switch (role) {
    case UntrustedRole:
        return !range->trusted;
    case SlotRole:
        return isMain ? kMainSlot : number;
    case FilteredInRole:
        return n->kind == Node::Kind::Range && selection_.filter(n->src).kind != FilterSpec::Kind::All &&
               (isMain || number >= 0);
    case Qt::CheckStateRole:
        return index.column() == TrustedCol && n->kind == Node::Kind::Range
                   ? QVariant(range->trusted ? Qt::Checked : Qt::Unchecked)
                   : QVariant();
    case Qt::DisplayRole:
        break;
    default:
        return {};
    }
    switch (index.column()) {
    case MainCol:
        return isMain ? QStringLiteral("●") : QString();
    case Overlay0Col:
        return number == 0 ? QStringLiteral("■") : QString();
    case OverlayCol:
        return number > 0 ? QString::number(number) : QString();
    case NameCol:
        return n->label;
    case TimeCol:
        return range->kind == model::RangeKind::Outing ? QStringLiteral("—") : lapTime(range->duration());
    case DeltaFastestCol: {
        if (range->kind != model::RangeKind::Lap) {
            return {};
        }
        const model::RangeGroup* g = detail::groupOf(*src, n->ref->layer, n->ref->groupPath);
        const model::Range* fastest =
            g == nullptr ? nullptr : analysis::fastestLap(std::span<const model::Range>(g->ranges));
        if (fastest == nullptr) {
            return {};
        }
        return signedSeconds(static_cast<double>(range->duration() - fastest->duration()) / 1e6);
    }
    case OffsetCol:
        if (range->offsetUs != 0) {
            return signedSeconds(static_cast<double>(range->offsetUs) / 1e6) + QStringLiteral(" s");
        }
        if (range->offsetM != 0.0) {
            return (range->offsetM > 0.0 ? QStringLiteral("+") : QString()) +
                   QString::number(range->offsetM, 'f', 2) + QStringLiteral(" m");
        }
        return {};
    case LocalMathsCol: {
        const std::string layer(model::canonicalLayerName(n->ref->layer));
        const bool local = src->sidecar.hasLayer(layer) && src->sidecar.layer(layer).localMaths.has_value();
        return local ? QStringLiteral("ƒ") : QString();
    }
    default:
        return {};
    }
}

QVariant DataWindowModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case MainCol:
        return QStringLiteral("M");
    case Overlay0Col:
        return QStringLiteral("O");
    case OverlayCol:
        return QStringLiteral("1");
    case NameCol:
        return tr("Name");
    case TimeCol:
        return tr("Time");
    case DeltaFastestCol:
        return tr("Δ Fastest");
    case OffsetCol:
        return tr("Offset");
    case TrustedCol:
        return tr("Trusted");
    case LocalMathsCol:
        return tr("Local maths");
    default:
        return {};
    }
}

Qt::ItemFlags DataWindowModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags f = QAbstractItemModel::flags(index);
    const Node* n = nodeOf(index);
    if (n != nullptr && n->kind == Node::Kind::Range && index.column() == TrustedCol) {
        f |= Qt::ItemIsUserCheckable;
    }
    return f;
}

bool DataWindowModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const Node* n = nodeOf(index);
    if (n == nullptr || n->kind != Node::Kind::Range || index.column() != TrustedCol ||
        role != Qt::CheckStateRole) {
        return false;
    }
    const bool trusted = value.toInt() == Qt::Checked;
    selection_.setTrusted(*n->ref, trusted); // rows refresh through sourceReplaced
    return true;
}

std::optional<model::RangeRef> DataWindowModel::rangeAt(const QModelIndex& index) const {
    const Node* n = nodeOf(index);
    return n != nullptr && n->kind == Node::Kind::Range ? n->ref : std::nullopt;
}

std::optional<model::SourceId> DataWindowModel::sourceAt(const QModelIndex& index) const {
    const Node* n = nodeOf(index);
    return n == nullptr ? std::nullopt : std::optional<model::SourceId>(n->src);
}

QModelIndex DataWindowModel::indexOf(const model::RangeRef& range) const {
    for (const auto& root : roots_) {
        if (root->src != range.src) {
            continue;
        }
        for (const auto& child : root->children) {
            if (child->kind == Node::Kind::Range && *child->ref == range) {
                return indexOfNode(child.get(), NameCol);
            }
            for (const auto& leaf : child->children) {
                if (*leaf->ref == range) {
                    return indexOfNode(leaf.get(), NameCol);
                }
            }
        }
    }
    return {};
}

} // namespace esses::qt
