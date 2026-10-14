// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CommandPalette (06 section 15, AC-M3-12). Owner: src/properties_shell/.

#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/widgets/properties_shell/command_palette.hpp>

#include <QApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScreen>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <tuple>

namespace esses::widgets {

namespace {

// Fuzzy subsequence: every character of `needle` appears in `hay` in order (case-insensitive).
bool subsequence(const QString& needle, const QString& hay) {
    qsizetype at = 0;
    for (const QChar c : needle) {
        at = hay.indexOf(c, at, Qt::CaseInsensitive);
        if (at < 0) {
            return false;
        }
        ++at;
    }
    return true;
}

QString argsText(const QVariantList& args) {
    QStringList parts;
    for (const QVariant& a : args) {
        parts.push_back(a.toString());
    }
    return parts.join(QStringLiteral(", "));
}

const QList<qt::CommandContext>& allContexts() {
    static const QList<qt::CommandContext> all{
        qt::CommandContext::Graph,      qt::CommandContext::Summary,        qt::CommandContext::Track,
        qt::CommandContext::Component,  qt::CommandContext::Worksheet,      qt::CommandContext::Global,
        qt::CommandContext::DataWindow, qt::CommandContext::ChannelsWindow, qt::CommandContext::ValuesWindow};
    return all;
}

} // namespace

CommandPalette::CommandPalette(qt::CommandRegistry& registry, const qt::KeymapBinder* keymap,
                               qt::SettingsStore* settings, QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), registry_(registry), keymap_(keymap),
      settings_(settings) {
    setObjectName(QStringLiteral("commandPalette"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    edit_ = new QLineEdit(this);
    edit_->setObjectName(QStringLiteral("paletteQuery"));
    edit_->setPlaceholderText(
        tr("Type a command; ? lists disabled ones, > worksheets, @ channels, # ranges"));
    edit_->setProperty("esses.textEntry", true);
    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("paletteList"));
    list_->setColumnCount(4);
    list_->setHeaderLabels({tr("Command"), tr("Menu"), tr("Key"), tr("Context")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(6, 6, 6, 6);
    column->addWidget(edit_);
    column->addWidget(list_, 1);
    resize(640, 420);
    edit_->installEventFilter(this);
    connect(edit_, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(list_, &QTreeWidget::itemActivated, this, [this] { invokeCurrent(); });
}

CommandPalette::~CommandPalette() = default;

void CommandPalette::setProvider(QChar prefix, ItemProvider provider) {
    providers_.insert(prefix, std::move(provider));
}

void CommandPalette::popup(qt::CommandContext context, const QString& query) {
    context_ = context;
    previousFocus_ = QApplication::focusWidget();
    if (QWidget* w = parentWidget() != nullptr ? parentWidget()->window() : nullptr) {
        const QRect g = w->geometry();
        move(g.center() - QPoint(width() / 2, height() / 2));
    }
    show();
    raise();
    activateWindow();
    edit_->setFocus(Qt::PopupFocusReason);
    {
        const QSignalBlocker block(edit_);
        edit_->setText(query);
    }
    rebuild();
}

void CommandPalette::setQuery(const QString& query) {
    edit_->setText(query); // rebuilds through textChanged
    if (query.isEmpty()) {
        rebuild();
    }
}

QString CommandPalette::query() const {
    return edit_->text();
}

QString CommandPalette::keyText(const QString& commandId) const {
    if (keymap_ == nullptr) {
        return {};
    }
    const QStringList keys = keymap_->keysFor(commandId);
    return keys.isEmpty() ? QString() : keys.front();
}

QStringList CommandPalette::recentIds() const {
    if (settings_ == nullptr) {
        return {};
    }
    return settings_->value(QString::fromLatin1(kRecentKey)).toStringList();
}

void CommandPalette::rememberRecent(const QString& id) {
    if (settings_ == nullptr) {
        return;
    }
    QStringList recent = recentIds();
    recent.removeAll(id);
    recent.prepend(id);
    while (recent.size() > kRecentMax) {
        recent.removeLast();
    }
    settings_->setValue(QString::fromLatin1(kRecentKey), recent);
}

void CommandPalette::rebuild() {
    rows_.clear();
    items_.clear();
    list_->clear();
    QString q = edit_->text();
    const QChar prefix = q.isEmpty() ? QChar() : q.front();
    if (!q.isEmpty() && providers_.contains(prefix)) {
        const QString needle = q.mid(1).trimmed();
        const QList<PaletteItem> all = providers_.value(prefix)();
        for (const PaletteItem& item : all) {
            if (!needle.isEmpty() && !subsequence(needle, item.title) && !subsequence(needle, item.detail)) {
                continue;
            }
            Row r;
            r.id = item.key;
            r.item = static_cast<int>(items_.size());
            items_.append(item);
            rows_.append(r);
            auto* row = new QTreeWidgetItem(list_, {item.title, item.detail, QString(), QString()});
            static_cast<void>(row);
        }
        list_->setCurrentItem(list_->topLevelItem(0));
        return;
    }
    const bool withDisabled = q.startsWith(QLatin1Char('?'));
    if (withDisabled) {
        q = q.mid(1);
    }
    q = q.trimmed();
    const QStringList recent = recentIds();
    struct Entry {
        bool prefixMatch = false;
        int recentRank = 0;
        QString title;
        Row row;
        const qt::CommandSpec* spec = nullptr;
    };
    std::vector<Entry> entries;
    for (const qt::CommandSpec* spec : registry_.all()) {
        const bool enabled = registry_.isEnabled(spec->id);
        if (!enabled && !withDisabled) {
            continue;
        }
        // One row per distinct argument list of the command's keymap rows (view.worksheet_n 1..10, ...).
        std::vector<QVariantList> argLists;
        if (keymap_ != nullptr) {
            for (const QString& key : keymap_->keysFor(spec->id)) {
                const std::optional<qt::KeyResolution> r = keymap_->resolve(key, allContexts());
                if (r && r->command == spec->id && !r->args.isEmpty() &&
                    std::find(argLists.begin(), argLists.end(), r->args) == argLists.end()) {
                    argLists.push_back(r->args);
                }
            }
        }
        if (argLists.empty()) {
            argLists.emplace_back();
        }
        for (const QVariantList& args : argLists) {
            const QString title =
                args.isEmpty() ? spec->title
                               : spec->title + QStringLiteral(" (") + argsText(args) + QLatin1Char(')');
            if (!q.isEmpty() && !subsequence(q, title) && !subsequence(q, spec->id) &&
                !subsequence(q, spec->menuPath)) {
                continue;
            }
            Entry e;
            e.prefixMatch = !q.isEmpty() && (title.startsWith(q, Qt::CaseInsensitive) ||
                                             spec->id.startsWith(q, Qt::CaseInsensitive));
            const qsizetype rank = recent.indexOf(spec->id);
            e.recentRank = q.isEmpty() || rank < 0 ? kRecentMax + 1 : static_cast<int>(rank);
            e.title = title;
            e.row.id = spec->id;
            e.row.args = args;
            e.row.enabled = enabled;
            e.spec = spec;
            entries.push_back(std::move(e));
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return std::make_tuple(!a.prefixMatch, a.recentRank, a.title.toLower(), a.row.id) <
               std::make_tuple(!b.prefixMatch, b.recentRank, b.title.toLower(), b.row.id);
    });
    for (const Entry& e : entries) {
        QString key;
        if (keymap_ != nullptr) {
            key = e.row.args.isEmpty() ? keyText(e.row.id) : QString();
            if (!e.row.args.isEmpty()) {
                for (const QString& k : keymap_->keysFor(e.row.id)) {
                    const std::optional<qt::KeyResolution> r = keymap_->resolve(k, allContexts());
                    if (r && r->command == e.row.id && r->args == e.row.args) {
                        key = k;
                        break;
                    }
                }
            }
        }
        auto* item = new QTreeWidgetItem(
            list_, {e.title, e.spec->menuPath, key, qt::commandContextName(e.spec->context)});
        item->setData(0, Qt::UserRole, e.row.id);
        if (!e.row.enabled) {
            item->setDisabled(true);
        }
        rows_.append(e.row);
    }
    list_->setCurrentItem(list_->topLevelItem(0));
}

QStringList CommandPalette::visibleIds() const {
    QStringList out;
    out.reserve(rows_.size());
    for (const Row& r : rows_) {
        out.push_back(r.id);
    }
    return out;
}

int CommandPalette::currentRow() const {
    QTreeWidgetItem* item = list_->currentItem();
    return item == nullptr ? -1 : list_->indexOfTopLevelItem(item);
}

void CommandPalette::setCurrentRow(int row) {
    if (row >= 0 && row < list_->topLevelItemCount()) {
        list_->setCurrentItem(list_->topLevelItem(row));
    }
}

bool CommandPalette::invokeCurrent() {
    return invokeCurrent(Qt::NoModifier);
}

bool CommandPalette::invokeCurrent(Qt::KeyboardModifiers modifiers) {
    const int row = currentRow();
    if (row < 0 || row >= rows_.size()) {
        return false;
    }
    const Row r = rows_.at(row);
    if (!r.enabled) {
        return false;
    }
    // Back to the focus of the moment the palette opened: the handler sees that context (AC-M3-12).
    hide();
    if (!previousFocus_.isNull()) {
        previousFocus_->activateWindow();
        previousFocus_->setFocus(Qt::OtherFocusReason);
    }
    if (r.item >= 0) {
        const PaletteItem item = items_.value(r.item);
        if (item.invoke) {
            item.invoke(modifiers);
        }
        Q_EMIT invoked(item.key);
        return true;
    }
    if (!registry_.isEnabled(r.id)) {
        return false;
    }
    rememberRecent(r.id);
    const bool ran = registry_.invoke(r.id, r.args);
    if (ran) {
        Q_EMIT invoked(r.id);
    }
    return ran;
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == edit_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Up:
            setCurrentRow(std::max(0, currentRow() - 1));
            return true;
        case Qt::Key_Down:
            setCurrentRow(std::min(list_->topLevelItemCount() - 1, currentRow() + 1));
            return true;
        case Qt::Key_PageUp:
            setCurrentRow(std::max(0, currentRow() - 10));
            return true;
        case Qt::Key_PageDown:
            setCurrentRow(std::min(list_->topLevelItemCount() - 1, currentRow() + 10));
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            invokeCurrent(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier));
            return true;
        case Qt::Key_Escape:
            hide();
            if (!previousFocus_.isNull()) {
                previousFocus_->setFocus(Qt::OtherFocusReason);
            }
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace esses::widgets
