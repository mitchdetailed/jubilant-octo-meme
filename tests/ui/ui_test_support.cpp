// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "ui_test_support.hpp"

#include <esses/base/assert.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTest>
#include <atomic>
#include <mutex>
#include <system_error>

namespace esses::uitest {

namespace {

std::atomic<int> gAssertFailures{0};
std::mutex gAssertMutex;
std::string gLastAssert; // guarded by gAssertMutex

// Records a failed ESSES_ASSERT from any thread (the NoBlock check of the GUI thread included) instead of
// aborting, so the suite reports it in cleanup() with its text.
void recordingAssertHandler(const char* expression, const char* file, int line, const char* message) {
    gAssertFailures.fetch_add(1);
    std::lock_guard lock(gAssertMutex);
    gLastAssert = std::string(file != nullptr ? file : "?") + ":" + std::to_string(line) + ": " +
                  (expression != nullptr ? expression : "") +
                  (message != nullptr ? std::string(" - ") + message : "");
}

std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

QJsonObject readObject(const std::filesystem::path& p) {
    QFile f(QString::fromStdWString(p.wstring()));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(f.readAll()).object();
}

// Trusted flying laps in the Outings:Laps group of a laps-1 document.
int trustedFlyingLaps(const QJsonObject& laps) {
    int n = 0;
    const QJsonObject layers = laps.value(QStringLiteral("layers")).toObject();
    for (auto layer = layers.begin(); layer != layers.end(); ++layer) {
        QJsonArray stack = layer->toObject().value(QStringLiteral("groups")).toArray();
        while (!stack.isEmpty()) {
            const QJsonObject g = stack.first().toObject();
            stack.removeFirst();
            for (const QJsonValue child : g.value(QStringLiteral("children")).toArray()) {
                stack.append(child);
            }
            if (g.value(QStringLiteral("path")).toString() != QLatin1String("Outings:Laps")) {
                continue;
            }
            for (const QJsonValue r : g.value(QStringLiteral("ranges")).toArray()) {
                const QJsonObject range = r.toObject();
                if (range.value(QStringLiteral("lapClass")).toString() == QLatin1String("Flying") &&
                    range.value(QStringLiteral("trusted")).toBool()) {
                    ++n;
                }
            }
        }
    }
    return n;
}

} // namespace

std::filesystem::path repoRoot() {
    return std::filesystem::path(ESSES_TEST_REPO_ROOT);
}

std::filesystem::path buildRoot() {
    // ESSES_TEST_BINARY_DIR is <build>/tests/ui.
    return (std::filesystem::path(ESSES_TEST_BINARY_DIR) / ".." / "..").lexically_normal();
}

std::filesystem::path accFixtureInput() {
    const std::filesystem::path rel =
        std::filesystem::path("motec.ld") / "synthetic-acc-dialect" / "input.ld";
    for (const std::filesystem::path& base : {buildRoot() / "fixture-inputs", repoRoot() / "fixtures"}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(base / rel, ec)) {
            return base / rel;
        }
    }
    return {};
}

std::optional<PrivateLapFile> privateBeaconLapFile(std::string& reason) {
    const std::filesystem::path dir = repoRoot() / "fixtures" / "private" / "motec.ld";
    const std::filesystem::path manifest = dir / "manifest.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifest, ec)) {
        reason = "no private fixture checkout (fixtures/private/motec.ld/manifest.json absent)";
        return std::nullopt;
    }
    int listed = 0;
    int present = 0;
    for (const QJsonValue v : readObject(manifest).value(QStringLiteral("fixtures")).toArray()) {
        const QJsonObject f = v.toObject();
        ++listed;
        const std::filesystem::path input = fsPath(f.value(QStringLiteral("input")).toString());
        if (input.empty() || !std::filesystem::is_regular_file(input, ec)) {
            continue;
        }
        ++present;
        const std::string name = f.value(QStringLiteral("dir")).toString().toStdString();
        if (trustedFlyingLaps(readObject(dir / name / "expected.laps.json")) >= 2) {
            return PrivateLapFile{input, name};
        }
    }
    reason = "none of the " + std::to_string(listed) + " private logger files (" + std::to_string(present) +
             " present on this machine) has two trusted flying laps in Outings:Laps (no beacons); no real "
             "ACC export exists in the private corpus";
    return std::nullopt;
}

void prepareFontEnvironment() {
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        const std::filesystem::path dir = repoRoot() / "tests" / "golden" / "fonts";
        qputenv("QT_QPA_FONTDIR", QString::fromStdWString(dir.wstring()).toLocal8Bit());
    }
}

int registerGoldenFonts() {
    static const int count =
        qt::SchemeFonts::registerApplicationFonts(repoRoot() / "tests" / "golden" / "fonts");
    return count;
}

Paths pathsFor(const QString& configDir) {
    PathOverrides o;
    o.configDir = fsPath(configDir);
    o.dataDir = repoRoot() / "data";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve: %s", p.error().toString().c_str());
    }
    return *p;
}

void installAssertRecorder() {
    static const bool installed = [] {
        setAssertHandler(&recordingAssertHandler);
        return true;
    }();
    static_cast<void>(installed);
}

int assertFailures() {
    return gAssertFailures.load();
}

std::string lastAssertFailure() {
    std::lock_guard lock(gAssertMutex);
    return gLastAssert;
}

void resetAssertFailures() {
    gAssertFailures.store(0);
    std::lock_guard lock(gAssertMutex);
    gLastAssert.clear();
}

std::unique_ptr<qt::AppRuntime> makeRuntime(unsigned workers, bool guiThreadNoBlock) {
    installAssertRecorder();
    qt::AppRuntimeOptions o;
    o.workers = workers;
    o.cacheBudget = std::size_t{512} << 20;
    o.markGuiThreadNoBlock = guiThreadNoBlock; // as esses does (AC-M1-12); shutdown() restores the role
    return std::make_unique<qt::AppRuntime>(o);
}

bool showAndActivate(QWidget& w) {
    w.show();
    if (!QTest::qWaitForWindowExposed(&w)) {
        return false;
    }
    w.activateWindow();
    return QTest::qWaitForWindowActive(&w);
}

void press(QWidget& fallback, int key, Qt::KeyboardModifiers mods) {
    QWidget* target = QApplication::focusWidget() != nullptr ? QApplication::focusWidget() : &fallback;
    QTest::keyClick(target, static_cast<Qt::Key>(key), mods);
}

void sendMouse(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

widgets::TimeDistanceGraph* graphOf(widgets::MainWindow& w) {
    for (widgets::ComponentWidget* c : w.canvas().components()) {
        if (auto* g = qobject_cast<widgets::TimeDistanceGraph*>(c)) {
            return g;
        }
    }
    return nullptr;
}

bool waitQuiet(widgets::TimeDistanceGraph& g, int timeoutMs, int quietMs) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (!QTest::qWaitFor([&g] { return g.staticUpToDate(); }, timeoutMs)) {
            return false;
        }
        const int submitted = g.staticBuildsSubmitted();
        QTest::qWait(quietMs);
        if (g.staticUpToDate() && g.staticBuildsSubmitted() == submitted) {
            return true;
        }
    }
    return false;
}

std::optional<double> numberOf(const QString& cell) {
    static const QRegularExpression re(
        QStringLiteral("^\\s*([-+]?[0-9]+(?:\\.[0-9]*)?(?:[eE][-+]?[0-9]+)?)"));
    const QRegularExpressionMatch m = re.match(cell);
    if (!m.hasMatch()) {
        return std::nullopt;
    }
    bool ok = false;
    const double v = m.captured(1).toDouble(&ok);
    return ok ? std::optional<double>(v) : std::nullopt;
}

} // namespace esses::uitest
