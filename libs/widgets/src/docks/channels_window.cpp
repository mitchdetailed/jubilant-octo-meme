// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/model/channel.hpp>
#include <esses/qtmodel/channels_model.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/widgets/docks/channels_window.hpp>

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVBoxLayout>
#include <functional>

namespace esses::widgets {

namespace {

// The search over ChannelsModel (06 section 7 item 3, D-175: word prefixes, loose = substrings), the drag
// source of channel rows and the styling of the channel status (Pending italic, Error red).
class ChannelFilterProxy final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    QString query;
    bool loose = false;

    void setQuery(const QString& q, bool looseMatch) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        // invalidateRowsFilter() is deprecated for Qt 6.13 (and warned about from 6.12);
        // endFilterChange(Direction) exists from 6.10.
        beginFilterChange();
        query = q.trimmed();
        loose = looseMatch;
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        query = q.trimmed();
        loose = looseMatch;
        invalidateRowsFilter();
#endif
    }

    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        if (query.isEmpty()) {
            return true;
        }
        const QModelIndex i = sourceModel()->index(row, 0, parent);
        const QVariant name = i.data(qt::ChannelsModel::ChannelNameRole);
        // Source and category rows stay when a descendant matches (recursive filtering).
        return name.isValid() && qt::ChannelsModel::filterAccepts(name.toString(), query, loose);
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        Qt::ItemFlags f = QSortFilterProxyModel::flags(index);
        if (index.data(qt::ChannelsModel::ChannelNameRole).isValid()) {
            f |= Qt::ItemIsDragEnabled;
        } else {
            f &= ~Qt::ItemIsDragEnabled;
        }
        return f;
    }

    QStringList mimeTypes() const override { return {QString::fromLatin1(ChannelsWindow::kMimeType)}; }

    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        QStringList names;
        for (const QModelIndex& i : indexes) {
            const QVariant name = i.data(qt::ChannelsModel::ChannelNameRole);
            if (name.isValid() && !names.contains(name.toString())) {
                names << name.toString();
            }
        }
        if (names.isEmpty()) {
            return nullptr;
        }
        auto* mime = new QMimeData;
        mime->setData(QString::fromLatin1(ChannelsWindow::kMimeType),
                      qt::ChannelsModel::channelsMime(names, false));
        mime->setText(names.join(QLatin1Char('\n')));
        return mime;
    }

    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }

    QVariant data(const QModelIndex& index, int role) const override {
        if (role == Qt::FontRole || role == Qt::ForegroundRole) {
            const QVariant status = QSortFilterProxyModel::data(index, qt::ChannelsModel::StatusRole);
            if (status.isValid()) {
                const auto s = static_cast<model::ChannelStatus>(status.toInt());
                if (role == Qt::FontRole && s == model::ChannelStatus::Pending) {
                    QFont f;
                    f.setItalic(true);
                    return f;
                }
                if (role == Qt::ForegroundRole && s == model::ChannelStatus::Error) {
                    return QColor(Qt::red);
                }
            }
        }
        return QSortFilterProxyModel::data(index, role);
    }
};

void visit(const QAbstractItemModel& m, const QModelIndex& parent,
           const std::function<void(const QModelIndex&)>& f) {
    for (int r = 0; r < m.rowCount(parent); ++r) {
        const QModelIndex i = m.index(r, 0, parent);
        f(i);
        visit(m, i, f);
    }
}

} // namespace

std::optional<ChannelsWindow::ChannelDrop> ChannelsWindow::parseMime(const QMimeData& mime) {
    const QString type = QString::fromLatin1(kMimeType);
    if (!mime.hasFormat(type)) {
        return std::nullopt;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(mime.data(type));
    if (!doc.isObject()) {
        return std::nullopt;
    }
    ChannelDrop drop;
    for (const QJsonValue& v : doc.object().value(QStringLiteral("channels")).toArray()) {
        if (v.isString() && !v.toString().isEmpty()) {
            drop.channels << v.toString();
        }
    }
    drop.newPanel = doc.object().value(QStringLiteral("newPanel")).toBool();
    if (drop.channels.isEmpty()) {
        return std::nullopt;
    }
    return drop;
}

ChannelsWindow::ChannelsWindow(qt::ChannelsModel& model, qt::CommandRegistry& commands, QWidget* parent)
    : QWidget(parent), model_(model), commands_(commands) {
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("channelSearch"));
    search_->setPlaceholderText(tr("Search channels"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(tr("Search channels"));
    search_->setToolTip(
        tr("Type the first letters of words of a channel name, in order: \"W S FL\" finds Wheel "
           "Speed FL, \"temp (ecu)\" finds Water Temp (ECU). TAB or DOWN moves into the list, "
           "ENTER adds the channel to the focused component."));
    noMatch_ = new QLabel(this);
    noMatch_->setObjectName(QStringLiteral("channelNoMatch"));
    noMatch_->setWordWrap(true);
    noMatch_->setVisible(false);
    auto* proxy = new ChannelFilterProxy(this);
    proxy->setRecursiveFilteringEnabled(true);
    proxy->setSourceModel(&model_);
    proxy_ = proxy;
    tree_ = new QTreeView(this);
    tree_->setObjectName(QStringLiteral("channelTree"));
    tree_->setAccessibleName(tr("Channels"));
    tree_->setModel(proxy_);
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setDefaultDropAction(Qt::CopyAction);
    tree_->setExpandsOnDoubleClick(false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(search_);
    layout->addWidget(noMatch_);
    layout->addWidget(tree_);
    setFocusProxy(search_);

    search_->installEventFilter(this);
    tree_->installEventFilter(this);
    connect(search_, &QLineEdit::textChanged, this, &ChannelsWindow::applyFilter);
    connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (index.data(qt::ChannelsModel::ChannelNameRole).isValid()) {
            tree_->setCurrentIndex(index);
            request(QApplication::keyboardModifiers().testFlag(Qt::ControlModifier));
        }
    });
    connect(proxy_, &QAbstractItemModel::modelReset, this, [this] {
        tree_->expandAll();
        highlightFirstMatch();
    });
    connect(proxy_, &QAbstractItemModel::rowsInserted, tree_, &QTreeView::expandAll);
    static_cast<void>(commands_); // channels_window context commands arrive with the M2 search semantics
    tree_->expandAll();
}

ChannelsWindow::~ChannelsWindow() = default;

void ChannelsWindow::applyFilter(const QString& text) {
    static_cast<ChannelFilterProxy*>(proxy_)->setQuery(text, loose_);
    tree_->expandAll();
    highlightFirstMatch();
    const QString query = text.trimmed();
    const bool none = !query.isEmpty() && visibleChannels().isEmpty();
    noMatch_->setText(
        none ? tr("No channel matches \"%1\"%2.")
                   .arg(query, loose_ ? QString() : tr(" (word prefixes; loose matching is in Options)"))
             : QString());
    // Shown only while it is not hidden explicitly (an offscreen test reads isHidden()).
    noMatch_->setHidden(!none);
}

void ChannelsWindow::setLooseMatch(bool loose) {
    if (loose_ == loose) {
        return;
    }
    loose_ = loose;
    applyFilter(search_->text());
}

void ChannelsWindow::followSession(qt::ProjectSession& session) {
    const QPointer<qt::ProjectSession> s(&session);
    const auto follow = [this, s] {
        if (!s.isNull()) {
            setLooseMatch(s->looseChannelSearch());
        }
    };
    connect(&session, &qt::ProjectSession::documentsChanged, this, follow);
    connect(&session, &qt::ProjectSession::projectOpened, this, follow);
    connect(&session, &qt::ProjectSession::projectClosed, this, follow);
    follow();
}

void ChannelsWindow::highlightFirstMatch() {
    QModelIndex first;
    visit(*proxy_, {}, [&first](const QModelIndex& i) {
        if (!first.isValid() && i.data(qt::ChannelsModel::ChannelNameRole).isValid()) {
            first = i;
        }
    });
    if (first.isValid()) {
        tree_->setCurrentIndex(first);
        tree_->scrollTo(first);
    }
}

QString ChannelsWindow::currentChannel() const {
    return tree_->currentIndex().data(qt::ChannelsModel::ChannelNameRole).toString();
}

QStringList ChannelsWindow::visibleChannels() const {
    QStringList out;
    visit(*proxy_, {}, [&out](const QModelIndex& i) {
        const QVariant name = i.data(qt::ChannelsModel::ChannelNameRole);
        if (name.isValid()) {
            out << name.toString();
        }
    });
    return out;
}

void ChannelsWindow::request(bool newPanel) {
    const QString name = currentChannel();
    if (!name.isEmpty()) {
        Q_EMIT addChannelRequested(name, newPanel);
    }
}

bool ChannelsWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress && (watched == search_ || watched == tree_)) {
        auto* key = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers mods = key->modifiers() & ~Qt::KeypadModifier;
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            (mods == Qt::NoModifier || mods == Qt::ControlModifier)) {
            request(mods == Qt::ControlModifier);
            key->accept();
            return true;
        }
        if (watched == search_ && mods == Qt::NoModifier &&
            (key->key() == Qt::Key_Down || key->key() == Qt::Key_Tab)) {
            if (!tree_->currentIndex().isValid()) {
                highlightFirstMatch();
            }
            tree_->setFocus(Qt::TabFocusReason);
            key->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace esses::widgets
