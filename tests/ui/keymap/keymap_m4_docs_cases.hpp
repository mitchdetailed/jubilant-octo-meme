// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The KEYMAP_TEST entries of the M4 docs package (wave 2; D-206, D-215, docs/milestones/M4.md section 3;
// 06 section 14.4 help.contents F1), included by tst_keymap.cpp inside its anonymous namespace after
// Fixture, Case and Before (as keymap_m3_cases.hpp), so it uses only what that file includes (docs_shell.hpp,
// QFileInfo and QUrl among them since the integration, D-216).
// help.contents (global, F1; from the Data window and from the palette too): the arrange replaces the
// shell's URL opener by one that records the URL and reports success, so no browser starts. The fixture's
// data directory is the repository's data/, beside docs/user/, the layout of an installation
// (docs_shell.hpp), so verify expects exactly one URL: the file URL of the repository's docs/user/index.md
// (compared as the canonical local path), and the window's hint.
#pragma once

QStringList docsOpened; // the URLs the recording opener was handed in the current row

void addM4DocsCases(std::map<QString, Case>& table) {
    using F = Fixture;
    table.insert_or_assign(KEYMAP_TEST("help.contents"),
                           Case{[](F& f, int) {
                                    docsOpened.clear();
                                    auto* docs = shellExtension<DocsShell>(*f.w);
                                    QVERIFY(docs != nullptr);
                                    docs->setUrlOpener([](const QString& url) {
                                        docsOpened << url;
                                        return true;
                                    });
                                    f.w->clearHint();
                                },
                                {},
                                [](F& f, const Before&, int) {
                                    QCOMPARE(docsOpened.size(), 1);
                                    const QUrl url(docsOpened.front(), QUrl::StrictMode);
                                    QVERIFY2(url.isLocalFile(), qPrintable(docsOpened.front()));
                                    const QString expected = QString::fromStdWString(
                                        (qt::test::repoRoot() / "docs" / "user" / "index.md").wstring());
                                    QVERIFY(QFileInfo::exists(expected));
                                    QCOMPARE(QFileInfo(url.toLocalFile()).canonicalFilePath(),
                                             QFileInfo(expected).canonicalFilePath());
                                    QVERIFY(!f.w->lastHint().isEmpty());
                                },
                                [](F&) { docsOpened.clear(); }});
}
