// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers shared by the cross-layer Qt Tests of tests/ui (compiled into every tst_*.cpp of this directory, so
// everything lives in esses::uitest and nothing here includes another test-support .cpp): fixture lookup (the
// generated public ACC stand-in, the private corpus), a MainWindow on a private configuration directory and
// the repository's data directory, key and mouse delivery as the platform would, the golden fonts, waits on
// the time_distance graph and number parsing of Values cells.
#pragma once

#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/widgets/components/time_distance_graph.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QEvent>
#include <QPointF>
#include <QString>
#include <QWidget>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace esses::uitest {

std::filesystem::path repoRoot();
// The build tree's root (CMAKE_BINARY_DIR).
std::filesystem::path buildRoot();

// fixtures/motec.ld/synthetic-acc-dialect/input.ld as generated into the build tree by the ctest fixture
// fixtures.generate_inputs (<build>/fixture-inputs), else the copy in the source tree (esses-gen-fixtures
// --inputs-only); empty when neither exists.
std::filesystem::path accFixtureInput();

// A real logger file of the private corpus (fixtures/private/motec.ld/manifest.json, never committed) whose
// expected.laps.json has at least two trusted flying laps in Outings:Laps, with its input present. When there
// is none, `reason` says why (no private checkout, no input files, no file with beacon laps).
struct PrivateLapFile {
    std::filesystem::path input;
    std::string name;
};
std::optional<PrivateLapFile> privateBeaconLapFile(std::string& reason);

// Before the QApplication exists (initMain): QT_QPA_FONTDIR = tests/golden/fonts unless already set, so the
// offscreen platform sees only the golden fonts (D-127).
void prepareFontEnvironment();
// Registers tests/golden/fonts with the application (needs the QApplication); the number of font files.
int registerGoldenFonts();

Paths pathsFor(const QString& configDir);
// The runtime of a shell test. `guiThreadNoBlock` (default, as in esses): the calling (GUI) thread carries
// ThreadRole::NoBlock until shutdown(), so any blocking Future wait or Column::require on it fails the
// AC-M1-12 assertion; test code that blocks on purpose (writing a synthetic log) runs under
// ThreadRole::Scope(ThreadRole::Normal). Installs the assertion recorder below.
std::unique_ptr<qt::AppRuntime> makeRuntime(unsigned workers = 4, bool guiThreadNoBlock = true);
// ESSES_ASSERT failures recorded since the last reset (any thread; the recorder replaces the aborting
// default handler for the whole test executable): suites check assertFailures() == 0 in cleanup().
void installAssertRecorder();
int assertFailures();
std::string lastAssertFailure();
void resetAssertFailures();
bool showAndActivate(QWidget& w);
// Sends a key click to the focus widget (else `fallback`), as the platform would.
void press(QWidget& fallback, int key, Qt::KeyboardModifiers mods = Qt::NoModifier);
void sendMouse(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons);

// The time_distance graph of the active worksheet (the first one in paint order), or null.
widgets::TimeDistanceGraph* graphOf(widgets::MainWindow& w);
// Waits until the graph's Static layer is current and stays so for `quietMs`.
bool waitQuiet(widgets::TimeDistanceGraph& g, int timeoutMs = 60000, int quietMs = 150);

// The number at the start of a Values cell ("123.4 km/h", "-0.50"); nullopt for "", "…" or text.
std::optional<double> numberOf(const QString& cell);

} // namespace esses::uitest
