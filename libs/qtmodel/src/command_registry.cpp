// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CommandRegistry (06 section 14.1, D-030): command ids -> handlers; menus, toolbars, the keymap and the Qt
// Test rows are generated from this table.

#include <esses/qtmodel/command_registry.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <array>
#include <utility>

namespace esses::qt {

namespace {

constexpr std::array<const char*, 9> kContextNames{"global",      "worksheet",       "component",
                                                   "graph",       "summary",         "track",
                                                   "data_window", "channels_window", "values_window"};

} // namespace

QString commandContextName(CommandContext context) {
    return QString::fromLatin1(kContextNames[static_cast<std::size_t>(context)]);
}

std::optional<CommandContext> parseCommandContext(const QString& name) {
    for (std::size_t i = 0; i < kContextNames.size(); ++i) {
        if (name == QLatin1String(kContextNames[i])) {
            return static_cast<CommandContext>(i);
        }
    }
    return std::nullopt;
}

// ---- CommandRegistry ------------------------------------------------------------------------------------

CommandRegistry::CommandRegistry(QObject* parent) : QObject(parent) {}

CommandRegistry::~CommandRegistry() = default;

void CommandRegistry::add(CommandSpec spec) {
    QString id = spec.id;
    commands_[id] = std::move(spec);
    Q_EMIT commandsChanged();
}

bool CommandRegistry::setHandler(const QString& id, std::function<void(const QVariantList&)> invoke,
                                 std::function<bool()> enabled) {
    const auto it = commands_.find(id);
    if (it == commands_.end()) {
        return false;
    }
    it->second.invoke = std::move(invoke);
    if (enabled) {
        it->second.enabled = std::move(enabled);
    }
    return true;
}

bool CommandRegistry::invoke(const QString& id, const QVariantList& args) {
    const auto it = commands_.find(id);
    if (it == commands_.end() || !it->second.invoke) {
        return false;
    }
    if (it->second.enabled && !it->second.enabled()) {
        return false;
    }
    it->second.invoke(args);
    Q_EMIT invoked(id);
    return true;
}

const CommandSpec* CommandRegistry::find(const QString& id) const {
    const auto it = commands_.find(id);
    return it == commands_.end() ? nullptr : &it->second;
}

QList<const CommandSpec*> CommandRegistry::all() const {
    QList<const CommandSpec*> out;
    for (const auto& entry : commands_) {
        out.append(&entry.second);
    }
    return out;
}

bool CommandRegistry::isEnabled(const QString& id) const {
    const CommandSpec* spec = find(id);
    return spec != nullptr && spec->invoke && (!spec->enabled || spec->enabled());
}

QString CommandRegistry::dumpJson() const {
    QJsonArray rows;
    for (const auto& entry : commands_) {
        const CommandSpec& c = entry.second;
        QJsonObject row;
        row.insert(QStringLiteral("id"), c.id);
        row.insert(QStringLiteral("title"), c.title);
        row.insert(QStringLiteral("context"), commandContextName(c.context));
        row.insert(QStringLiteral("keys"), QJsonArray::fromStringList(c.defaultKeys));
        rows.append(row);
    }
    QJsonObject root;
    root.insert(QStringLiteral("commands"), rows);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

} // namespace esses::qt
