// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CommandPalette (06 section 15, CTRL+SHIFT+P; 07 section 6.1; AC-M3-12): a frameless popup centred over the
// main window with a line edit and a list.
//   1. opens with every enabled CommandRegistry command sorted by title; typing filters by fuzzy
//      subsequence on title, id and menu path (prefix matches first, then recently used)
//   2. rows: title, menu path, current key (from the keymap), context badge; disabled commands only when the
//      query starts with '?'
//   3. ENTER invokes in the context captured when the palette opened (focused component, highlighted
//      Data-window range), exactly as pressing the key would (AC-M3-12's shared harness); ESC closes;
//      UP/DOWN move; the last 20 invoked ids are kept per user (SettingsStore)
//   4. prefixes: '>' worksheets/workbooks, '@' channels (ENTER adds to the focused component, CTRL+ENTER as
//      a new panel), '#' ranges of the Main source (ENTER Main, CTRL+ENTER Overlay 0, ALT+ENTER overlay)
//   5. Help -> Keyboard Shortcuts opens it with the query '?'
// As built (M3): the '>', '@' and '#' lists come from providers the shell sets (setProvider); a command is
// invoked after the popup has closed and the keyboard focus is back where it was when the palette opened,
// so its handler sees the same focused component, worksheet and Data-window highlight as a key press would.
// Within the prefix and the other matches, recently used commands come first, then by title. The recent list
// is the settings key "palette.recent". Commands that take arguments (view.worksheet_n, view.workbook_n,
// component.colour_band) are listed once per keymap row with that row's arguments.
#pragma once

#include <esses/qtmodel/command_registry.hpp>

#include <QChar>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWidget>
#include <functional>

class QLineEdit;
class QTreeWidget;

namespace esses::qt {
class KeymapBinder;
class SettingsStore;
} // namespace esses::qt

namespace esses::widgets {

// One row of a '>', '@' or '#' list.
struct PaletteItem {
    QString key;    // what visibleIds() lists ("worksheet:<book>:<sheet>", "channel:<name>", ...)
    QString title;  // shown and matched
    QString detail; // the second column
    std::function<void(Qt::KeyboardModifiers)> invoke;
};

class CommandPalette final : public QWidget {
    Q_OBJECT
public:
    // `keymap` and `settings` may be null (no key column; no recent list).
    CommandPalette(qt::CommandRegistry& registry, const qt::KeymapBinder* keymap, qt::SettingsStore* settings,
                   QWidget* parent = nullptr);
    ~CommandPalette() override;

    // Shows the palette for the focus context captured now.
    void popup(qt::CommandContext context, const QString& query = {});
    void setQuery(const QString& query);
    QString query() const;
    // Command ids of the visible rows in order (the '>', '@' and '#' modes list their items' keys instead).
    QStringList visibleIds() const;
    // The key sequence text shown for a command ("" when unbound).
    QString keyText(const QString& commandId) const;
    // Invokes the current row (ENTER); false when there is none or it is disabled.
    bool invokeCurrent();

    // Added in M3 (as built): the source of the '>', '@' or '#' list; the current row; the modifiers the next
    // invokeCurrent passes to an item (CTRL+ENTER, ALT+ENTER); the recent command ids, newest first; the
    // context captured by popup().
    using ItemProvider = std::function<QList<PaletteItem>()>;
    void setProvider(QChar prefix, ItemProvider provider);
    int currentRow() const;
    void setCurrentRow(int row);
    bool invokeCurrent(Qt::KeyboardModifiers modifiers);
    QStringList recentIds() const;
    qt::CommandContext capturedContext() const noexcept { return context_; }
    static constexpr int kRecentMax = 20;
    static constexpr const char* kRecentKey = "palette.recent";

Q_SIGNALS:
    void invoked(const QString& commandId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Row {
        QString id; // command id, or the item key
        QVariantList args;
        bool enabled = true;
        int item = -1; // index into items_ for a prefix list
    };
    void rebuild();
    void rememberRecent(const QString& id);

    qt::CommandRegistry& registry_;
    const qt::KeymapBinder* keymap_ = nullptr;
    qt::SettingsStore* settings_ = nullptr;
    QLineEdit* edit_ = nullptr;
    QTreeWidget* list_ = nullptr;
    QList<Row> rows_;
    QList<PaletteItem> items_;
    QMap<QChar, ItemProvider> providers_;
    QPointer<QWidget> previousFocus_;
    qt::CommandContext context_ = qt::CommandContext::Global;
};

} // namespace esses::widgets
