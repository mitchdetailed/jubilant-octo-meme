// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DocsShell (M4 wave 2, docs package; D-206; 07 section 7.1 "Docs"; 06 section 14.4 help.contents F1): Help >
// Help Contents opens the user documentation (docs/user/, installed beside the data directory) through a URL
// opener the tests replace. registerCommands() registers kCommand once implemented: until then the keymap row
// stays "planned for M4" and the generic planned row of tests/ui/keymap covers it; the package flips the row
// to "implemented" with its KEYMAP_TEST in tests/ui/keymap/keymap_m4_docs_cases.hpp. M4-STUB: every member
// below is inert.
#pragma once

#include <esses/widgets/shell/shell_extension.hpp>

#include <QString>
#include <functional>

namespace esses::widgets {

class DocsShell final : public ShellExtension {
    Q_OBJECT
public:
    static constexpr const char* kKey = "docs";
    static constexpr const char* kCommand = "help.contents";
    // Opens a URL (the default: QDesktopServices::openUrl); false when it could not.
    using UrlOpener = std::function<bool(const QString& url)>;

    explicit DocsShell(MainWindow& window);
    ~DocsShell() override;

    void setUrlOpener(UrlOpener opener); // M4-STUB: ignored
    // The documentation's start page as a URL (empty when it is not installed). M4-STUB: empty.
    QString contentsUrl() const;

    void registerCommands() override; // M4-STUB
};

} // namespace esses::widgets
