// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// KeymapBinder (06 sections 14.2-14.4, D-030, D-033; AC-M1-06): the keymap-1 rows bound to command ids per
// context, resolved along the focus chain.

#include <esses/project/keymap.hpp>
#include <esses/qtmodel/keymap_binder.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <utility>

namespace esses::qt {

namespace {

// Table key: the normal form with key names compared case-insensitively (06 section 14.2 rule 7, as
// project::Keymap::conflicts compares them).
QString tableKey(const QString& keyText) {
    return KeymapBinder::normalise(keyText).toLower();
}

} // namespace

KeymapBinder::KeymapBinder(CommandRegistry& registry, QObject* parent)
    : QObject(parent), registry_(registry) {}

KeymapBinder::~KeymapBinder() = default;

QString KeymapBinder::normalise(const QString& keyText) {
    return QString::fromStdString(project::normaliseKeySequence(keyText.trimmed().toStdString()));
}

QStringList KeymapBinder::setKeymap(const project::Keymap& keymap) {
    table_.clear();
    order_.clear();
    QStringList rejected;
    for (const project::KeyBinding& row : keymap.bindings) {
        const QString command = QString::fromStdString(row.command);
        const std::optional<CommandContext> context =
            parseCommandContext(QString::fromStdString(row.context));
        if (!context) {
            rejected << QStringLiteral("keymap: %1: unknown context '%2'; the row is disabled")
                            .arg(command, QString::fromStdString(row.context));
            continue;
        }
        QVariantList args;
        if (row.args.isArray()) {
            args = QJsonDocument::fromJson(QByteArray::fromStdString(row.args.compact()))
                       .array()
                       .toVariantList();
        }
        for (const std::string& key : row.keys) {
            const QString text = normalise(QString::fromStdString(key));
            if (text.isEmpty()) {
                continue;
            }
            const auto slot = std::make_pair(static_cast<int>(*context), tableKey(text));
            if (const auto it = table_.find(slot); it != table_.end()) {
                // 06 section 14.2 rule 7: two bindings of one sequence in one context are rejected at load.
                rejected << QStringLiteral(
                                "keymap: %1 is bound to both %2 and %3 in context %4; %3 is disabled")
                                .arg(text, it->second.command, command, commandContextName(*context));
                continue;
            }
            table_.emplace(slot, KeyResolution{command, args, *context});
            order_.append({command, text});
        }
    }
    Q_EMIT bindingsChanged();
    return rejected;
}

std::optional<KeyResolution> KeymapBinder::resolve(const QString& keyText,
                                                   const QList<CommandContext>& chain) const {
    const QString text = tableKey(keyText);
    for (const CommandContext c : chain) {
        const auto it = table_.find({static_cast<int>(c), text});
        if (it != table_.end()) {
            return it->second;
        }
    }
    return std::nullopt;
}

bool KeymapBinder::dispatch(const QString& keyText, const QList<CommandContext>& chain) {
    const auto r = resolve(keyText, chain);
    return r && registry_.invoke(r->command, r->args);
}

QStringList KeymapBinder::keysFor(const QString& commandId) const {
    QStringList keys;
    for (const auto& [command, key] : order_) {
        if (command == commandId && !keys.contains(key)) {
            keys.append(key);
        }
    }
    return keys;
}

} // namespace esses::qt
