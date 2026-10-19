// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M4-10 in the real shell, offscreen (07 section 7.2; 06 section 6 "Changed files", section 14.4
// file.reload_source; 02 section 1 behaviour 5; D-208): the offscreen half of the SMB case of
// docs/milestones/M4.md section 6.2, on a local disk.
//   * staleOnActivationAndReloadFromMessages: two logs open with a selection on both and a trust edit; log a
//     is replaced on disk by an atomic writer (renamed aside, a session with one more lap written under its
//     name). The application's activation marks it stale: the Data window's badge on its row only, a Messages
//     row with the Reload action, File > Reload File enabled. The row's action reopens it through the open
//     pipeline: a new SourceId, the selection restored by RangeId with its positions, the source first in the
//     Data window again, the sidecar edit re-applied, the badge gone.
//   * reloadFromTheDataWindow: the Data window's context menu offers Reload File on a stale row (disabled
//     on a fresh row); it reloads the same bytes as the same SourceId with the selection unchanged.
//   * nothingToReload: with no stale source the command is disabled and does nothing, with or without an
//     argument.
// Every runtime marks the GUI thread NoBlock as esses does; no ESSES_ASSERT may fail on any thread.

#include "../../libs/qtmodel/tests/qtmodel_test_support.hpp"
#include "ui_test_support.hpp"

#include <esses/base/read_file.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/reload_shell.hpp>
#include <esses/widgets/shell/shell_extension.hpp>

#include <QAction>
#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QIdentityProxyModel>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The qtmodel synthetic-source helpers (circuit laps, .ld writer), compiled into this executable only.
#include "../../libs/qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;
namespace fs = std::filesystem;

namespace {

fs::path fsPath(const QString& p) {
    return fs::path(p.toStdWString());
}

fs::path writeLog(qt::AppRuntime& rt, const fs::path& dir, const std::string& name,
                  std::vector<double> laps) {
    const ThreadRole::Scope blocking(ThreadRole::Normal); // writing a log reads every chunk
    fs::create_directories(dir);
    qt::test::SyntheticOptions o;
    o.path = name + ".ld";
    o.lapSeconds = std::move(laps);
    return qt::test::writeLd(*qt::test::makeSource(o, rt.cache()), dir, name);
}

// An atomic writer's replacement of an open log: the mapped file renamed aside (its share mode allows it),
// the new .ld (and .ldx) taking the name, its modification time after the old one's.
bool replaceOnDisk(const fs::path& log, const fs::path& replacementLd) {
    const Result<std::string> bytes = readWholeFile(replacementLd);
    if (!bytes.ok()) {
        return false;
    }
    std::error_code ec;
    fs::path aside = log;
    aside += ".aside";
    fs::remove(aside, ec);
    // Again for a moment while it is refused: a virus scanner may hold a file just written without sharing
    // delete access.
    const bool renamed = QTest::qWaitFor(
        [&] {
            fs::rename(log, aside, ec);
            return !ec;
        },
        5000);
    if (!renamed) {
        return false;
    }
    {
        std::ofstream out(log, std::ios::binary | std::ios::trunc);
        out.write(bytes.value().data(), static_cast<std::streamsize>(bytes.value().size()));
    }
    fs::path fromLdx = replacementLd;
    fromLdx.replace_extension(".ldx");
    fs::path toLdx = log;
    toLdx.replace_extension(".ldx");
    if (fromLdx != toLdx && fs::exists(fromLdx)) {
        fs::copy_file(fromLdx, toLdx, fs::copy_options::overwrite_existing, ec);
    }
    fs::last_write_time(log, fs::last_write_time(aside) + std::chrono::seconds(2), ec);
    return !ec;
}

struct Shell {
    QTemporaryDir config;
    QTemporaryDir logs;
    std::unique_ptr<MainWindow> w;

    bool start(qt::AppRuntime& rt) {
        if (!config.isValid() || !logs.isValid()) {
            return false;
        }
        w = std::make_unique<MainWindow>(rt, uitest::pathsFor(config.path()));
        w->setExitPrompt([] { return MainWindow::ExitAnswer::Discard; });
        return uitest::showAndActivate(*w);
    }
    std::shared_ptr<const model::DataSource> open(const fs::path& ld) {
        const std::size_t before = w->selection().sources().size();
        w->openLog(ld);
        if (!QTest::qWaitFor([&] { return w->selection().sources().size() == before + 1; }, 30000)) {
            return nullptr;
        }
        for (const auto& s : w->selection().sources()) {
            if (s->prov.path.filename() == ld.filename()) {
                return s;
            }
        }
        return nullptr;
    }
    // The application becomes active (what the platform reports when the user comes back to it).
    static void activate() { Q_EMIT qApp->applicationStateChanged(Qt::ApplicationActive); }
    // The Data window's row of a source, in the tree's own (styled) model.
    QModelIndex row(const model::SourceId& id) {
        QIdentityProxyModel& view = w->dataWindow().viewModel();
        for (int r = 0; r < view.rowCount(); ++r) {
            const QModelIndex i = view.index(r, qt::DataWindowModel::NameCol);
            if (w->dataWindow().model().sourceAt(view.mapToSource(i)) == id) {
                return i;
            }
        }
        return {};
    }
    bool badge(const model::SourceId& id) {
        const QVariant icon = w->dataWindow().viewModel().data(row(id), Qt::DecorationRole);
        return icon.isValid() && !icon.value<QIcon>().isNull();
    }
    // The Messages list's row with this code, or an invalid index.
    QModelIndex message(const QString& code) {
        QSortFilterProxyModel& list = w->messagesWindow().filterModel();
        for (int r = list.rowCount() - 1; r >= 0; --r) {
            const QModelIndex i = list.index(r, 0);
            if (list.data(i, qt::MessagesModel::CodeRole).toString() == code) {
                return i;
            }
        }
        return {};
    }
};

model::RangeRef onSource(model::RangeRef r, const model::SourceId& id) {
    r.src = id;
    return r;
}

} // namespace

class TstReloadSource : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup() {
        const int failures = uitest::assertFailures();
        const std::string last = uitest::lastAssertFailure();
        uitest::resetAssertFailures();
        QVERIFY2(failures == 0, last.c_str());
    }

    void initTestCase() { registerBuiltinComponents(); }

    void staleOnActivationAndReloadFromMessages() {
        auto rt = uitest::makeRuntime();
        {
            Shell s;
            QVERIFY(s.start(*rt));
            const fs::path dir = fsPath(s.logs.path());
            const fs::path a = writeLog(*rt, dir / "a", "session-a", {40.0, 50.0, 48.0, 49.0, 30.0});
            const fs::path b = writeLog(*rt, dir / "b", "session-b", {30.0, 45.0, 44.0, 20.0});
            const fs::path next =
                writeLog(*rt, dir / "next", "session-a", {40.0, 50.0, 48.0, 49.0, 47.0, 30.0});
            const auto srcA = s.open(a);
            const auto srcB = s.open(b);
            QVERIFY(srcA != nullptr && srcB != nullptr);
            auto* shell = shellExtension<ReloadShell>(*s.w);
            QVERIFY(shell != nullptr);
            qt::DataSelectionModel& sel = s.w->selection();
            const model::RangeRef mainA = qt::test::rangeRef(*srcA, 2);
            const model::RangeRef overlayA = qt::test::rangeRef(*srcA, 3);
            const model::RangeRef overlay0B = qt::test::rangeRef(*srcB, 1);
            sel.setMain(mainA);
            sel.toggleOverlay0(overlay0B);
            sel.toggleOverlay(overlayA);
            sel.setTrusted(qt::test::rangeRef(*srcA, 1), false);
            QVERIFY(!sel.resolve(qt::test::rangeRef(*srcA, 1))->trusted);
            QVERIFY(!s.w->commands().isEnabled(QString::fromLatin1(ReloadShell::kCommand)));
            QVERIFY(!s.badge(srcA->id) && !s.badge(srcB->id));

            // The writer replaces a; coming back to the application marks it stale.
            QVERIFY(replaceOnDisk(a, next));
            Shell::activate();
            QVERIFY(s.w->loader().isStale(srcA->id));
            QVERIFY(!s.w->loader().isStale(srcB->id));
            QVERIFY(s.badge(srcA->id));
            QVERIFY(!s.badge(srcB->id));
            QVERIFY(s.w->dataWindow()
                        .viewModel()
                        .data(s.row(srcA->id), Qt::ToolTipRole)
                        .toString()
                        .contains(QStringLiteral("Changed on disk")));
            QVERIFY(s.w->commands().isEnabled(QString::fromLatin1(ReloadShell::kCommand)));
            QAction* menuAction = s.w->actions().action(QString::fromLatin1(ReloadShell::kCommand));
            QVERIFY(menuAction != nullptr && menuAction->isEnabled());
            QCOMPARE(menuAction->text().section(QLatin1Char('\t'), 0, 0), QStringLiteral("Reload File"));
            const QModelIndex row = s.message(QStringLiteral("open.file_changed"));
            QVERIFY(row.isValid());
            QCOMPARE(row.data(qt::MessagesModel::ActionRole).toString(),
                     QStringLiteral("file.reload_source"));
            QVERIFY(row.data(Qt::DisplayRole).toString().contains(QStringLiteral("Reload File")));
            Shell::activate(); // a second activation adds nothing
            QCOMPARE(s.w->loader().staleSources(), std::vector<model::SourceId>{srcA->id});

            // Reload from the Messages row.
            QVERIFY(s.w->messagesWindow().invokeAction(row));
            QVERIFY(sel.source(srcA->id) == nullptr); // closed at once, reopening
            QTRY_VERIFY_WITH_TIMEOUT(s.w->loader().pendingReloads() == 0 && sel.sources().size() == 2, 30000);
            const auto reopened = sel.sources().front(); // first again in the Data window
            QVERIFY(!(reopened->id == srcA->id));        // one more lap: another SourceId
            QCOMPARE(reopened->prov.path, a);
            QCOMPARE(qt::test::rangeCount(*reopened), std::size_t{6});
            QVERIFY(!s.w->loader().isStale(reopened->id));
            QVERIFY(!s.badge(reopened->id));
            QVERIFY(!s.w->commands().isEnabled(QString::fromLatin1(ReloadShell::kCommand)));
            QVERIFY(sel.snapshot().main == onSource(mainA, reopened->id));
            QVERIFY(sel.snapshot().overlay0 && *sel.snapshot().overlay0 == overlay0B);
            QCOMPARE(sel.snapshot().overlays.size(), std::size_t{1});
            QVERIFY(sel.snapshot().overlays[0] == onSource(overlayA, reopened->id));
            QVERIFY(
                !sel.resolve(onSource(qt::test::rangeRef(*srcA, 1), reopened->id))->trusted); // the sidecar
            QVERIFY(QTest::qWaitFor([&] { return uitest::runtimeQuiet(*rt); }, 60000));
        }
        rt->shutdown();
    }

    void reloadFromTheDataWindow() {
        auto rt = uitest::makeRuntime();
        {
            Shell s;
            QVERIFY(s.start(*rt));
            const fs::path dir = fsPath(s.logs.path());
            const fs::path a = writeLog(*rt, dir / "a", "session-a", {40.0, 50.0, 48.0, 49.0, 30.0});
            const fs::path b = writeLog(*rt, dir / "b", "session-b", {30.0, 45.0, 44.0, 20.0});
            const fs::path copy = dir / "copy" / "session-b.ld";
            fs::create_directories(copy.parent_path());
            fs::copy_file(b, copy);
            const auto srcA = s.open(a);
            const auto srcB = s.open(b);
            QVERIFY(srcA != nullptr && srcB != nullptr);
            qt::DataSelectionModel& sel = s.w->selection();
            sel.setMain(qt::test::rangeRef(*srcB, 2));
            sel.toggleOverlay(qt::test::rangeRef(*srcA, 1));
            const plot::SelectionSnapshot before = sel.snapshot();

            QVERIFY(replaceOnDisk(b, copy)); // the same bytes, a newer file
            Shell::activate();
            QVERIFY(s.w->loader().isStale(srcB->id));
            // A fresh row offers Reload File disabled; the stale row enabled.
            const auto reloadIn = [](QMenu& menu) {
                return menu.findChild<QAction*>(QStringLiteral("dataReloadFile"));
            };
            {
                const std::unique_ptr<QMenu> fresh = s.w->dataWindow().contextMenu(
                    s.w->dataWindow().viewModel().index(s.row(srcA->id).row(), 0));
                QVERIFY(fresh != nullptr);
                QAction* reload = reloadIn(*fresh);
                QVERIFY(reload != nullptr);
                QVERIFY(!reload->isEnabled());
            }
            const std::unique_ptr<QMenu> menu =
                s.w->dataWindow().contextMenu(s.w->dataWindow().viewModel().index(s.row(srcB->id).row(), 0));
            QVERIFY(menu != nullptr);
            QAction* reload = reloadIn(*menu);
            QVERIFY(reload != nullptr && reload->isEnabled());
            reload->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(s.w->loader().pendingReloads() == 0 && sel.source(srcB->id) != nullptr &&
                                         sel.sources().size() == 2,
                                     30000);
            QVERIFY(!s.w->loader().isStale(srcB->id)); // the same bytes: the same SourceId
            QVERIFY(sel.source(srcB->id) != srcB);     // a new snapshot of the reopened file
            QCOMPARE(sel.sources().back()->id, srcB->id);
            QVERIFY(sel.snapshot().main == before.main);
            QVERIFY(sel.snapshot().overlays == before.overlays);
            QVERIFY(QTest::qWaitFor([&] { return uitest::runtimeQuiet(*rt); }, 60000));
        }
        rt->shutdown();
    }

    void nothingToReload() {
        auto rt = uitest::makeRuntime();
        {
            Shell s;
            QVERIFY(s.start(*rt));
            const fs::path a =
                writeLog(*rt, fsPath(s.logs.path()) / "a", "session-a", {40.0, 50.0, 48.0, 49.0, 30.0});
            const auto src = s.open(a);
            QVERIFY(src != nullptr);
            Shell::activate();
            QVERIFY(s.w->loader().staleSources().empty());
            const QString id = QString::fromLatin1(ReloadShell::kCommand);
            QVERIFY(!s.w->commands().isEnabled(id));
            QVERIFY(!s.w->commands().invoke(id));
            QVERIFY(!s.w->commands().invoke(id, {QString::fromStdString(src->id.hex())}));
            auto* shell = shellExtension<ReloadShell>(*s.w);
            QVERIFY(shell != nullptr);
            QVERIFY(!shell->reloadSource());
            QVERIFY(!shell->reloadSource(src->id));
            QVERIFY(shell->checkNow().empty());
            QCOMPARE(s.w->loader().pendingReloads(), 0);
            QVERIFY(QTest::qWaitFor([&] { return uitest::runtimeQuiet(*rt); }, 60000));
        }
        rt->shutdown();
    }
};

QTEST_MAIN(TstReloadSource)
#include "tst_reload_source.moc"
