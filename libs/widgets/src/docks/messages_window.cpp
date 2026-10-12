// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/widgets/docks/messages_window.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QPalette>
#include <QSaveFile>
#include <QSortFilterProxyModel>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <array>

namespace esses::widgets {

namespace {

using M = qt::MessagesModel;

QString originName(int origin) {
    switch (static_cast<qt::Message::Origin>(origin)) {
    case qt::Message::Origin::Import:
        return QStringLiteral("Import");
    case qt::Message::Origin::Maths:
        return QStringLiteral("Maths");
    case qt::Message::Origin::Budget:
        return QStringLiteral("Budget");
    case qt::Message::Origin::Project:
        return QStringLiteral("Project");
    case qt::Message::Origin::System:
        return QStringLiteral("System");
    }
    return QStringLiteral("System");
}

QString severityName(int severity) {
    switch (static_cast<qt::Message::Severity>(severity)) {
    case qt::Message::Severity::Info:
        return QStringLiteral("Info");
    case qt::Message::Severity::Warning:
        return QStringLiteral("Warning");
    case qt::Message::Severity::Error:
        return QStringLiteral("Error");
    }
    return QStringLiteral("Info");
}

class MessagesFilterProxy final : public QSortFilterProxyModel {
public:
    MessagesFilterProxy(qt::CommandRegistry& commands, QObject* parent)
        : QSortFilterProxyModel(parent), commands_(commands) {}
    std::array<bool, 3> shown{true, true, true};
    QString text;

    void refilter() { invalidateRowsFilter(); }

    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        const QModelIndex i = sourceModel()->index(row, 0, parent);
        const int severity = i.data(M::SeverityRole).toInt();
        if (severity < 0 || severity > 2 || !shown[static_cast<std::size_t>(severity)]) {
            return false;
        }
        if (text.isEmpty()) {
            return true;
        }
        return i.data(Qt::DisplayRole).toString().contains(text, Qt::CaseInsensitive) ||
               i.data(M::SourceRole).toString().contains(text, Qt::CaseInsensitive) ||
               i.data(M::CodeRole).toString().contains(text, Qt::CaseInsensitive);
    }

    QVariant data(const QModelIndex& index, int role) const override {
        switch (role) {
        case Qt::DisplayRole: {
            const QModelIndex s = mapToSource(index);
            const QString when = s.data(M::TimestampRole).toDateTime().toString(QStringLiteral("hh:mm:ss"));
            const QString source = s.data(M::SourceRole).toString();
            QString line = when + QStringLiteral("  ") + originName(s.data(M::OriginRole).toInt()) +
                           QStringLiteral("  ") +
                           (source.isEmpty() ? QString() : source + QStringLiteral(": ")) +
                           s.data(Qt::DisplayRole).toString();
            const QString action = s.data(M::ActionRole).toString();
            if (!action.isEmpty()) {
                const qt::CommandSpec* spec = commands_.find(action);
                line += QStringLiteral("  [") + (spec != nullptr ? spec->title : action) + QLatin1Char(']');
            }
            return line;
        }
        case Qt::DecorationRole: {
            const QStyle* style = QApplication::style();
            switch (static_cast<qt::Message::Severity>(
                QSortFilterProxyModel::data(index, M::SeverityRole).toInt())) {
            case qt::Message::Severity::Error:
                return style->standardIcon(QStyle::SP_MessageBoxCritical);
            case qt::Message::Severity::Warning:
                return style->standardIcon(QStyle::SP_MessageBoxWarning);
            case qt::Message::Severity::Info:
                return style->standardIcon(QStyle::SP_MessageBoxInformation);
            }
            return {};
        }
        case Qt::ForegroundRole:
            if (!QSortFilterProxyModel::data(index, M::ActionRole).toString().isEmpty()) {
                return QApplication::palette().color(QPalette::Link);
            }
            return {};
        case Qt::ToolTipRole: {
            const QModelIndex s = mapToSource(index);
            QString tip = severityName(s.data(M::SeverityRole).toInt()) + QStringLiteral(" (") +
                          originName(s.data(M::OriginRole).toInt()) + QLatin1Char(')');
            if (const QString code = s.data(M::CodeRole).toString(); !code.isEmpty()) {
                tip += QStringLiteral(" ") + code;
            }
            return tip + QLatin1Char('\n') + s.data(Qt::DisplayRole).toString();
        }
        default:
            return QSortFilterProxyModel::data(index, role);
        }
    }

private:
    qt::CommandRegistry& commands_;
};

} // namespace

MessagesWindow::MessagesWindow(qt::MessagesModel& model, qt::CommandRegistry& commands, QWidget* parent)
    : QWidget(parent), model_(model), commands_(commands) {
    auto* proxy = new MessagesFilterProxy(commands_, this);
    proxy->setSourceModel(&model_);
    proxy_ = proxy;

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(2, 2, 2, 2);
    const std::array<std::pair<Severity, QStyle::StandardPixmap>, 3> kinds{
        std::pair{Severity::Error, QStyle::SP_MessageBoxCritical},
        std::pair{Severity::Warning, QStyle::SP_MessageBoxWarning},
        std::pair{Severity::Info, QStyle::SP_MessageBoxInformation}};
    for (const auto& [severity, icon] : kinds) {
        auto* b = new QToolButton(this);
        b->setCheckable(true);
        b->setChecked(true);
        b->setIcon(style()->standardIcon(icon));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setObjectName(QStringLiteral("messagesShow") + severityName(static_cast<int>(severity)));
        connect(b, &QToolButton::toggled, this,
                [this, severity](bool on) { setSeverityShown(severity, on); });
        severityButtons_[static_cast<int>(severity)] = b;
        bar->addWidget(b);
    }
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("messagesFilter"));
    filter_->setPlaceholderText(tr("Filter messages"));
    filter_->setClearButtonEnabled(true);
    bar->addWidget(filter_, 1);
    auto* clear = new QToolButton(this);
    clear->setText(tr("Clear"));
    auto* copy = new QToolButton(this);
    copy->setText(tr("Copy"));
    auto* save = new QToolButton(this);
    save->setText(tr("Save As…"));
    bar->addWidget(clear);
    bar->addWidget(copy);
    bar->addWidget(save);

    list_ = new QListView(this);
    list_->setObjectName(QStringLiteral("messagesList"));
    list_->setAccessibleName(tr("Messages"));
    list_->setModel(proxy_);
    list_->setUniformItemSizes(true);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->installEventFilter(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(list_);
    setFocusProxy(list_);

    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        auto* p = static_cast<MessagesFilterProxy*>(proxy_);
        p->text = text.trimmed();
        p->refilter();
    });
    connect(clear, &QToolButton::clicked, &model_, &qt::MessagesModel::clear);
    connect(copy, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(logText()); });
    connect(save, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save Messages"), QString(),
                                                          tr("Log files (*.log);;All files (*)"));
        if (!path.isEmpty()) {
            saveAs(path);
        }
    });
    connect(list_, &QListView::doubleClicked, this,
            [this](const QModelIndex& index) { invokeAction(index); });
    connect(&model_, &QAbstractItemModel::rowsInserted, this, [this] {
        updateCounts();
        list_->scrollToBottom();
    });
    connect(&model_, &QAbstractItemModel::rowsRemoved, this, &MessagesWindow::updateCounts);
    connect(&model_, &QAbstractItemModel::modelReset, this, &MessagesWindow::updateCounts);
    updateCounts();
}

MessagesWindow::~MessagesWindow() = default;

void MessagesWindow::setSeverityShown(Severity severity, bool shown) {
    auto* p = static_cast<MessagesFilterProxy*>(proxy_);
    const auto i = static_cast<std::size_t>(severity);
    if (p->shown[i] == shown) {
        return;
    }
    p->shown[i] = shown;
    if (QToolButton* b = severityButtons_[static_cast<int>(severity)];
        b != nullptr && b->isChecked() != shown) {
        b->setChecked(shown);
    }
    p->refilter();
}

bool MessagesWindow::severityShown(Severity severity) const {
    return static_cast<const MessagesFilterProxy*>(proxy_)->shown[static_cast<std::size_t>(severity)];
}

int MessagesWindow::visibleCount() const {
    return proxy_->rowCount();
}

void MessagesWindow::updateCounts() {
    for (const Severity s : {Severity::Error, Severity::Warning, Severity::Info}) {
        QToolButton* b = severityButtons_[static_cast<int>(s)];
        if (b == nullptr) {
            continue;
        }
        const QString name = s == Severity::Error     ? tr("Errors")
                             : s == Severity::Warning ? tr("Warnings")
                                                      : tr("Info");
        b->setText(
            QStringLiteral("%1 (%2)").arg(name).arg(model_.count(static_cast<qt::Message::Severity>(s))));
    }
}

QString MessagesWindow::logText() const {
    QString out;
    for (int r = 0; r < proxy_->rowCount(); ++r) {
        const QModelIndex s = proxy_->mapToSource(proxy_->index(r, 0));
        out += s.data(M::TimestampRole).toDateTime().toString(Qt::ISODateWithMs) + QLatin1Char('\t') +
               severityName(s.data(M::SeverityRole).toInt()) + QLatin1Char('\t') +
               originName(s.data(M::OriginRole).toInt()) + QLatin1Char('\t') +
               s.data(M::SourceRole).toString() + QLatin1Char('\t') + s.data(M::CodeRole).toString() +
               QLatin1Char('\t') + s.data(Qt::DisplayRole).toString() + QLatin1Char('\n');
    }
    return out;
}

bool MessagesWindow::saveAs(const QString& path, QString* error) const {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error != nullptr) {
            *error = f.errorString();
        }
        return false;
    }
    f.write(logText().toUtf8());
    if (!f.commit()) {
        if (error != nullptr) {
            *error = f.errorString();
        }
        return false;
    }
    return true;
}

bool MessagesWindow::invokeAction(const QModelIndex& index) {
    if (!index.isValid()) {
        return false;
    }
    const QModelIndex s = proxy_->mapToSource(index);
    if (!s.isValid()) {
        return false;
    }
    const qt::Message& m = model_.at(s.row());
    return !m.action.isEmpty() && commands_.invoke(m.action, m.actionArgs);
}

// ENTER on a row with an action runs it (the list ignores ENTER, so the key reaches this filter first).
bool MessagesWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            if (invokeAction(list_->currentIndex())) {
                key->accept();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace esses::widgets
