// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The KEYMAP_TEST entries of the M4 reload package (D-206, D-208, docs/milestones/M4.md section 3; 06
// section 14.4 file.reload_source, AC-M4-10), included by tst_keymap.cpp inside its anonymous namespace after
// Fixture, Case and Before (as keymap_m3_cases.hpp), so it uses only what that file includes.
// file.reload_source (data_window, menu-only: the harness triggers File > Reload File, and runs it again from
// the palette): the arrange puts a selection on both logs (Main and an overlay on log a, Overlay 0 on log b),
// marks a lap of log a untrusted (a sidecar edit), then replaces log a on disk the way an atomic writer does
// (the mapped file renamed aside, the same bytes written anew under its name, so the SourceId stays) and
// announces the application's activation, which marks the source stale; the focus is the Data window's
// highlight on log a. Verify: the source reopened through the open pipeline (no reload pending, not stale,
// the badge gone), first in the Data window again, its sidecar re-applied (the lap still untrusted) and the
// selection the same RangeRefs. The entry undoes its edit and clears the overlays afterwards.
#pragma once

int reloadAsides = 0; // a fresh aside name per run

// The source row of `id` in the Data window's model.
QModelIndex dataSourceRow(Fixture& f, const model::SourceId& id) {
    qt::DataWindowModel& m = f.w->dataWindow().model();
    for (int row = 0; row < m.rowCount(); ++row) {
        const QModelIndex i = m.index(row, qt::DataWindowModel::NameCol);
        if (m.sourceAt(i) == id) {
            return i;
        }
    }
    return {};
}

void addM4ReloadCases(std::map<QString, Case>& table) {
    using F = Fixture;
    table.insert_or_assign(
        KEYMAP_TEST("file.reload_source"),
        Case{[](F& f, int) {
                 f.sel().toggleOverlay(f.lapsA[1]);
                 f.sel().toggleOverlay0(f.lapsB[1]);
                 f.sel().setTrusted(f.lapsA[3], false); // a sidecar edit (written when the source closes)
                 QVERIFY(f.sel().resolve(f.lapsA[3]) != nullptr && !f.sel().resolve(f.lapsA[3])->trusted);
                 // An atomic writer replaces log a: the mapped file is renamed aside (its share mode allows
                 // it) and the same bytes are written under the name, newer than the mapping's stamp.
                 const Result<std::string> bytes = readWholeFile(f.ldA);
                 QVERIFY(bytes.ok());
                 std::filesystem::path aside = f.ldA;
                 aside += ".aside" + std::to_string(++reloadAsides);
                 std::error_code ec;
                 // Again for a moment while it is refused (a scanner may hold the file without sharing
                 // delete).
                 QVERIFY(QTest::qWaitFor(
                     [&] {
                         std::filesystem::rename(f.ldA, aside, ec);
                         return !ec;
                     },
                     5000));
                 {
                     std::ofstream out(f.ldA, std::ios::binary | std::ios::trunc);
                     out.write(bytes.value().data(), static_cast<std::streamsize>(bytes.value().size()));
                 }
                 std::filesystem::last_write_time(
                     f.ldA, std::filesystem::last_write_time(aside, ec) + std::chrono::seconds(2), ec);
                 QVERIFY2(!ec, ec.message().c_str());
                 // The application becomes active: the shell checks every open file (06 section 6).
                 Q_EMIT qApp->applicationStateChanged(Qt::ApplicationActive);
                 QVERIFY(f.w->loader().isStale(f.a));
                 QVERIFY(!f.w->loader().isStale(f.b));
                 QVERIFY(f.w->dataWindow()
                             .model()
                             .data(dataSourceRow(f, f.a), qt::DataWindowModel::StaleRole)
                             .toBool());
             },
             {},
             [](F& f, const Before& b, int) {
                 QTRY_VERIFY_WITH_TIMEOUT(
                     f.w->loader().pendingReloads() == 0 && f.sel().source(f.a) != nullptr, 30000);
                 QVERIFY(!f.w->loader().isStale(f.a));
                 QVERIFY(!f.w->dataWindow()
                              .model()
                              .data(dataSourceRow(f, f.a), qt::DataWindowModel::StaleRole)
                              .toBool());
                 QCOMPARE(f.sel().sources().size(), std::size_t{2});
                 QVERIFY(f.sel().sources().front()->id == f.a); // its Data-window position
                 const model::Range* edited = f.sel().resolve(f.lapsA[3]);
                 QVERIFY(edited != nullptr);
                 QVERIFY(!edited->trusted); // the sidecar re-applied
                 QVERIFY(f.sel().snapshot().main == b.selection.main);
                 QVERIFY(f.sel().snapshot().overlay0 == b.selection.overlay0);
                 QVERIFY(f.sel().snapshot().overlays == b.selection.overlays);
             },
             [](F& f) {
                 f.sel().clearOverlays();
                 undoToMark(f); // the trust edit
                 QVERIFY(f.sel().resolve(f.lapsA[3]) != nullptr && f.sel().resolve(f.lapsA[3])->trusted);
                 f.lapsA = f.sel().displayedRanges(f.a);
             }});
}
