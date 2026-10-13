// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// KeymapBinder (06 sections 14.2-14.4, D-030, D-033; AC-M1-06): binds keymap-1 rows (project::Keymap, the
// shipped default merged with the user file) to CommandRegistry ids with the context rules of 06
// section 14.2: the first context of the focus chain with a binding for the key sequence wins (worksheet
// focus: [Graph|Summary|Track, Component, Worksheet, Global]; dock focus: [the dock's context, Global]);
// modal dialogs get no keymap handling; two bindings of one sequence in one context are rejected at load (a
// Messages row, the row disabled).
//
// Deviation from 06 section 14.2: the application-wide event filter that turns QKeyEvents into portable key
// text and computes the focus chain needs Qt Gui/Widgets, so it lives in esses_widgets
// (widgets::KeyEventRouter); this class holds the Qt-Core-only binding table and resolution.
// M1 details: every row with keys is bound whatever its status (planned rows bind keys whose commands are
// registered disabled, 06 section 14.4); key text is compared in project::normaliseKeySequence form, on
// load and on resolve, key names case-insensitively; keysFor() lists the keys in row order; a conflicting row
// (same key and context as an earlier row) is rejected and reported, the earlier row stays bound.
#pragma once

#include <esses/project/keymap.hpp>
#include <esses/qtmodel/command_registry.hpp>

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <map>
#include <optional>
#include <utility>

namespace esses::qt {

struct KeyResolution {
    QString command;
    QVariantList args;
    CommandContext context = CommandContext::Global;
};

class KeymapBinder final : public QObject {
    Q_OBJECT
public:
    explicit KeymapBinder(CommandRegistry& registry, QObject* parent = nullptr);
    ~KeymapBinder() override;

    // Rebuilds the binding table; returns the rejected rows as messages (conflicts, unknown contexts).
    QStringList setKeymap(const project::Keymap& keymap);
    // The command bound to `keyText` (portable text, normalised) in the first context of `chain` that has
    // one.
    std::optional<KeyResolution> resolve(const QString& keyText, const QList<CommandContext>& chain) const;
    // Resolves and invokes; true when a command ran (the key event is then accepted).
    bool dispatch(const QString& keyText, const QList<CommandContext>& chain);
    // The keys bound to a command (menus show the first one).
    QStringList keysFor(const QString& commandId) const;

Q_SIGNALS:
    void bindingsChanged();

public:
    // Normal form of portable key text ("ctrl+f" -> "Ctrl+F").
    static QString normalise(const QString& keyText);
    int bindingCount() const noexcept { return static_cast<int>(table_.size()); }

private:
    CommandRegistry& registry_;
    std::map<std::pair<int, QString>, KeyResolution> table_; // (context, lower-cased key text) -> command
    QList<std::pair<QString, QString>> order_;               // (command, key) in row order
};

} // namespace esses::qt
