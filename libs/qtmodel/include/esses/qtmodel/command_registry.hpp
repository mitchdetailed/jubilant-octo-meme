// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CommandRegistry (06 section 14.1, D-030): command ids -> actions. Menus, toolbars, context menus, hotkeys,
// the command palette and the Qt Test rows are generated from this table; a widget never creates a bare
// QAction with its own shortcut (06 section 1 rule 5). Commands are registered by their owning layer at
// start-up (esses_app: file/view/layout; esses_qtmodel facades: data/tools; components: component/graph/zoom/
// pan/cursor/datum; docks: data_window/channels_window/values_window). A command with no handler in the
// current context is enabled() == false. Command ids are snake_case <area>.<verb_noun> exactly as 06 section
// 14.4 lists them.
//
// Deviation from 06 section 14.1 (01 section 2.2 rule 4: esses_qtmodel links Qt Core only): default keys are
// Qt portable-text strings instead of QKeySequence, and the shared QAction per id (action(id)) lives in
// esses_widgets (widgets::CommandActions), because QKeySequence and QAction are Qt Gui types in Qt 6.
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace esses::qt {

enum class CommandContext : std::uint8_t {
    Global,
    Worksheet,
    Component,
    Graph,
    Summary,
    Track,
    DataWindow,
    ChannelsWindow,
    ValuesWindow
};
// "global", "worksheet", ..., "data_window", "channels_window", "values_window" (keymap-1 spelling).
QString commandContextName(CommandContext context);
std::optional<CommandContext> parseCommandContext(const QString& name);

struct CommandSpec {
    QString id;       // "zoom.in"
    QString title;    // "Zoom In"
    QString menuPath; // "Component/Zoom"
    CommandContext context = CommandContext::Global;
    QStringList defaultKeys; // portable text: "Up", "Ctrl+F", "F9"
    bool checkable = false;
    QString iconName;              // theme icon or empty
    std::function<bool()> enabled; // evaluated on menu show / palette open; empty = always
    std::function<bool()> checked;
    std::function<void(const QVariantList& args)> invoke;
};

class CommandRegistry final : public QObject {
    Q_OBJECT
public:
    explicit CommandRegistry(QObject* parent = nullptr);
    ~CommandRegistry() override;

    // Adds or replaces the command with spec.id.
    void add(CommandSpec spec);
    // Sets the handler of an existing command (components and docks attach handlers to app-registered ids).
    bool setHandler(const QString& id, std::function<void(const QVariantList&)> invoke,
                    std::function<bool()> enabled = {});
    // False when the id is unknown, disabled or has no handler.
    bool invoke(const QString& id, const QVariantList& args = {});
    const CommandSpec* find(const QString& id) const;
    QList<const CommandSpec*> all() const;
    bool isEnabled(const QString& id) const;
    // `esses --dump-commands`: the registry export that esses-cli validate checks keymaps against.
    QString dumpJson() const;

Q_SIGNALS:
    void invoked(const QString& id); // telemetry for tests; later the scripting hook
    void commandsChanged();

private:
    std::map<QString, CommandSpec> commands_;
};

} // namespace esses::qt
