// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/keymap_binder.hpp>
#include <esses/widgets/shell/command_actions.hpp>

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <algorithm>
#include <map>
#include <vector>

namespace esses::widgets {

const QStringList& CommandActions::menuOrder() {
    static const QStringList order{QStringLiteral("File"),      QStringLiteral("Edit"),
                                   QStringLiteral("Layout"),    QStringLiteral("Data"),
                                   QStringLiteral("Component"), QStringLiteral("Tools"),
                                   QStringLiteral("View"),      QStringLiteral("Help")};
    return order;
}

CommandActions::CommandActions(qt::CommandRegistry& registry, qt::KeymapBinder& keys, QObject* parent)
    : QObject(parent), registry_(registry), keys_(keys) {
    connect(&keys_, &qt::KeymapBinder::bindingsChanged, this, [this] {
        for (auto it = actions_.begin(); it != actions_.end(); ++it) {
            if (!it.value().isNull()) {
                updateText(it.key(), *it.value());
            }
        }
    });
}

CommandActions::~CommandActions() = default;

QAction* CommandActions::action(const QString& id) {
    if (const auto it = actions_.find(id); it != actions_.end() && !it.value().isNull()) {
        return it.value().data();
    }
    const qt::CommandSpec* spec = registry_.find(id);
    if (spec == nullptr) {
        return nullptr;
    }
    auto* a = new QAction(spec->title, this);
    a->setObjectName(id);
    a->setCheckable(spec->checkable);
    // Triggered from a menu, a tool bar or a context menu; the registry is the single executor. The checked
    // state is re-read afterwards (a checkable QAction toggles itself before the command runs).
    connect(a, &QAction::triggered, this, [this, id] {
        registry_.invoke(id);
        refresh(id);
    });
    actions_.insert(id, a);
    refresh(id);
    return a;
}

void CommandActions::updateText(const QString& id, QAction& a) {
    const qt::CommandSpec* spec = registry_.find(id);
    if (spec == nullptr) {
        return;
    }
    const QStringList keys = keys_.keysFor(id);
    const QString key = keys.isEmpty() ? QString() : keys.front();
    // "\t" shows the key in the menu's shortcut column without registering a QAction shortcut.
    a.setText(key.isEmpty() ? spec->title : spec->title + QLatin1Char('\t') + key);
    a.setIconText(spec->title);
    QString tip = key.isEmpty() ? spec->title : spec->title + QStringLiteral(" (") + key + QLatin1Char(')');
    if (const QString note = notes_.value(id); !note.isEmpty()) {
        tip += QStringLiteral(" - ") + note;
    }
    a.setToolTip(tip);
    a.setStatusTip(tip);
}

void CommandActions::refresh(const QString& id) {
    const auto it = actions_.find(id);
    if (it == actions_.end() || it.value().isNull()) {
        return;
    }
    QAction& a = *it.value();
    const qt::CommandSpec* spec = registry_.find(id);
    if (spec == nullptr) {
        a.setEnabled(false);
        return;
    }
    a.setCheckable(spec->checkable);
    a.setEnabled(registry_.isEnabled(id));
    if (spec->checkable && spec->checked) {
        a.setChecked(spec->checked());
    }
    updateText(id, a);
}

void CommandActions::refresh() {
    for (auto it = actions_.begin(); it != actions_.end(); ++it) {
        refresh(it.key());
    }
}

void CommandActions::setNote(const QString& id, const QString& note) {
    notes_.insert(id, note);
    refresh(id);
}

QMenu* CommandActions::menu(const QString& title) const {
    return menus_.value(title).data();
}

void CommandActions::populateMenuBar(QMenuBar& bar, const QStringList& itemOrder, const QStringList& skip) {
    // menuPath -> command ids in item order.
    std::map<QString, std::vector<const qt::CommandSpec*>> byPath;
    for (const qt::CommandSpec* spec : registry_.all()) {
        if (spec->menuPath.isEmpty() || skip.contains(spec->id)) {
            continue;
        }
        byPath[spec->menuPath].push_back(spec);
    }
    const auto rank = [&itemOrder](const qt::CommandSpec* s) {
        const qsizetype i = itemOrder.indexOf(s->id);
        return i < 0 ? itemOrder.size() : i;
    };
    for (auto& [path, specs] : byPath) {
        std::stable_sort(specs.begin(), specs.end(), [&](const qt::CommandSpec* a, const qt::CommandSpec* b) {
            const qsizetype ra = rank(a);
            const qsizetype rb = rank(b);
            return ra != rb ? ra < rb : a->id < b->id;
        });
    }
    QStringList tops = menuOrder();
    for (const auto& [path, specs] : byPath) {
        const QString top = path.section(QLatin1Char('/'), 0, 0);
        if (!tops.contains(top)) {
            tops.append(top);
        }
    }
    for (const QString& top : tops) {
        QMenu* m = bar.addMenu(top);
        m->setObjectName(QStringLiteral("menu.") + top);
        menus_.insert(top, m);
        connect(m, &QMenu::aboutToShow, this, [this] { refresh(); });
        // Direct items first, then one submenu per second-level path in first-seen order.
        if (const auto it = byPath.find(top); it != byPath.end()) {
            for (const qt::CommandSpec* s : it->second) {
                m->addAction(action(s->id));
            }
        }
        QHash<QString, QMenu*> subs;
        for (const auto& [path, specs] : byPath) {
            if (path.section(QLatin1Char('/'), 0, 0) != top || !path.contains(QLatin1Char('/'))) {
                continue;
            }
            const QString sub = path.section(QLatin1Char('/'), 1);
            QMenu*& sm = subs[sub];
            if (sm == nullptr) {
                sm = m->addMenu(sub);
                sm->setObjectName(QStringLiteral("menu.") + path);
                connect(sm, &QMenu::aboutToShow, this, [this] { refresh(); });
            }
            for (const qt::CommandSpec* s : specs) {
                sm->addAction(action(s->id));
            }
        }
        if (m->actions().isEmpty()) {
            m->menuAction()->setEnabled(false);
        }
    }
}

void CommandActions::populateToolBar(QToolBar& bar, const QStringList& ids) {
    for (const QString& id : ids) {
        if (id.isEmpty()) {
            bar.addSeparator();
        } else if (QAction* a = action(id)) {
            bar.addAction(a);
        }
    }
}

} // namespace esses::widgets
