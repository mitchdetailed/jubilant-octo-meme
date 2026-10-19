// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The 0.1 memory budget in esses_qtmodel (M4 budget package, AC-M4-02; memory_budget.hpp, source_loader.hpp
// "M4 budget"; 07 section 7.1 row 1, 01 section 7.2, D-011, D-043, D-206 (c)-(e)): the budget setting with
// its 1 GiB floor, the texts of the warning and of the Stats, and SourceLoader's open-time gate over a
// synthetic .ld: an open whose estimate exceeds the budget waits before publication with nothing decoded,
// Continue publishes it as any open, Cancel and cancelOpens() drop it with an Import Log row, an open under
// the budget and an open without a gate never wait. Expected texts and byte counts are written out by hand
// here (the chunk layout of 01 section 7.1), never taken from the code under test.

#include "qtmodel_test_support.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/memory_budget.hpp>
#include <esses/qtmodel/settings_store.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/series.hpp>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cstdint>
#include <limits>
#include <memory>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(esses::qt::OpenBudgetCheck)
Q_DECLARE_METATYPE(std::vector<esses::formats::ImportLogEntry>)
Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)

namespace {

constexpr int kTimeoutMs = 60000;
constexpr std::uint64_t kMiB = std::uint64_t{1} << 20;
constexpr std::uint64_t kGiB = std::uint64_t{1} << 30;

// ---- the synthetic log: three F32 channels (Wheel Speed FL/FR, Throttle) at 1 kHz over 217 s ----
// 217,000 samples per channel: 3 full chunks and one of 20,392 samples. A full chunk is 64 + 445,120 bytes
// (samples 262,144, bitmap 8,192, pyramid 174,760, rounded up to 64); the last one has samples 81,568 ->
// 81,600, bitmap 319 words -> 2,560, pyramid 5,098 + 1,275 + 319 + 80 + 20 + 5 + 2 + 1 = 6,800 entries =
// 54,400, layout 138,560, so 64 + 138,560. Per channel 3 x 445,184 + 138,624 = 1,474,176; three channels
// 4,422,528 bytes ("4.2 MB").
constexpr std::uint64_t kLogEstimate = 4'422'528;

std::uint64_t pad64(std::uint64_t n) {
    return (n + 63) / 64 * 64;
}
std::uint64_t ceilDiv(std::uint64_t a, std::uint64_t b) {
    return (a + b - 1) / b;
}
// One chunk of `n` 4-byte samples with a validity bitmap, as 01 section 7.1 lays it out.
std::uint64_t chunkBytes(std::uint64_t n) {
    std::uint64_t pyramid = 0;
    for (std::uint64_t bucket = 4; bucket <= 65'536; bucket *= 4) {
        pyramid += ceilDiv(n, bucket);
    }
    return 64 + pad64(pad64(4 * n) + pad64(ceilDiv(n, 64) * 8) + pyramid * 8);
}
std::uint64_t channelBytes(std::uint64_t samples) {
    return samples / 65'536 * chunkBytes(65'536) + (samples % 65'536 != 0 ? chunkBytes(samples % 65'536) : 0);
}

AppRuntimeOptions runtimeOptions() {
    AppRuntimeOptions o;
    o.workers = 2;
    o.cacheBudget = std::size_t{2} << 20; // 2 MiB: the log's 4.2 MB estimate is above it
    return o;
}

// Every task of the runtime's pool has completed and every result it posted has been delivered.
bool quiet(AppRuntime& rt) {
    const TaskPool::Stats s = rt.pool().stats();
    std::size_t queued = 0;
    for (const std::size_t q : s.queued) {
        queued += q;
    }
    return queued == 0 && s.parked == 0 && s.running == 0 && rt.dispatcher().pending() == 0;
}

// The log written through the motec.ld writer from a cache of its own, so the runtime's cache starts empty.
std::filesystem::path writeLog(const std::string& dir, const std::string& name) {
    const ThreadRole::Scope blocking(ThreadRole::Normal);
    store::ChunkCache scratch(std::size_t{256} << 20);
    SyntheticOptions o;
    o.path = name + ".ld";
    o.rateHz = 1000;
    return writeLd(*makeSource(o, scratch), scratchDir(dir), name);
}

struct Loader {
    AppRuntime rt{runtimeOptions()};
    std::unique_ptr<SourceLoader> loader;
    std::unique_ptr<QSignalSpy> published;
    std::unique_ptr<QSignalSpy> exceeded;
    std::unique_ptr<QSignalSpy> logs;

    explicit Loader(bool gate) {
        loader = std::make_unique<SourceLoader>(rt.pool(), rt.dispatcher(), shippedUnits());
        loader->setUseSidecars(false);
        loader->setRuleRunner(analysis::RuleRunner::circuitDefault());
        if (gate) {
            loader->setBudgetGate(&rt.cache());
        }
        published = std::make_unique<QSignalSpy>(loader.get(), &SourceLoader::sourcePublished);
        exceeded = std::make_unique<QSignalSpy>(loader.get(), &SourceLoader::budgetExceeded);
        logs = std::make_unique<QSignalSpy>(loader.get(), &SourceLoader::importLog);
    }
    ~Loader() {
        published.reset();
        exceeded.reset();
        logs.reset();
        loader.reset(); // its sources (and the waiting ones) hold columns of the runtime's cache
        rt.shutdown();
    }
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    int pendingId(qsizetype at = 0) const { return exceeded->at(at).at(0).toInt(); }
    OpenBudgetCheck check(qsizetype at = 0) const { return exceeded->at(at).at(1).value<OpenBudgetCheck>(); }
    // The Import Log codes announced so far.
    QStringList codes() const {
        QStringList out;
        for (const QList<QVariant>& call : *logs) {
            for (const formats::ImportLogEntry& e :
                 call.at(1).value<std::vector<formats::ImportLogEntry>>()) {
                out << QString::fromStdString(e.code);
            }
        }
        return out;
    }
};

// The runtime's cache holds nothing and was never asked for a chunk.
bool untouched(AppRuntime& rt) {
    const store::ChunkCache::Stats s = rt.cache().stats();
    return s.residentBytes == 0 && s.chunks == 0 && s.misses == 0 && s.hits == 0;
}

} // namespace

class TstMemoryBudget final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<OpenBudgetCheck>("esses::qt::OpenBudgetCheck");
        qRegisterMetaType<std::vector<formats::ImportLogEntry>>(
            "std::vector<esses::formats::ImportLogEntry>");
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
    }

    void bytesReadAsPeopleSayThem() {
        QCOMPARE(formatBytes(0), QStringLiteral("0 bytes"));
        QCOMPARE(formatBytes(1), QStringLiteral("1 byte"));
        QCOMPARE(formatBytes(512), QStringLiteral("512 bytes"));
        QCOMPARE(formatBytes(1023), QStringLiteral("1023 bytes"));
        QCOMPARE(formatBytes(1024), QStringLiteral("1 KB"));
        QCOMPARE(formatBytes(1536), QStringLiteral("2 KB"));         // 1.5 KB rounds up
        QCOMPARE(formatBytes(1'048'063), QStringLiteral("1023 KB")); // 1023.499 KB
        QCOMPARE(formatBytes(1'048'064), QStringLiteral("1.0 MB"));  // would round to 1024 KB
        QCOMPARE(formatBytes(kMiB), QStringLiteral("1.0 MB"));
        QCOMPARE(formatBytes(kLogEstimate), QStringLiteral("4.2 MB"));
        QCOMPARE(formatBytes(10 * kMiB - kMiB / 20 - 1), QStringLiteral("9.9 MB"));
        QCOMPARE(formatBytes(10 * kMiB - kMiB / 20), QStringLiteral("10 MB")); // 9.95 MB
        QCOMPARE(formatBytes(812 * kMiB), QStringLiteral("812 MB"));
        QCOMPARE(formatBytes(kGiB - kMiB / 2 - 1), QStringLiteral("1023 MB"));
        QCOMPARE(formatBytes(kGiB - kMiB / 2), QStringLiteral("1.0 GB"));
        QCOMPARE(formatBytes(kGiB), QStringLiteral("1.0 GB"));
        QCOMPARE(formatBytes(1'503'238'554), QStringLiteral("1.4 GB")); // 1.4 x 2^30
        QCOMPARE(formatBytes(4 * kGiB), QStringLiteral("4.0 GB"));
    }

    void theSettingIsAPositiveNumberRaisedToTheFloor() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        SettingsStore settings(
            std::filesystem::path(dir.filePath(QStringLiteral("settings.json")).toStdWString()));
        const QString key = QString::fromLatin1(kBudgetSettingKey);
        QVERIFY(!budgetFromSettings(settings).has_value()); // absent: the runtime keeps its budget
        settings.setValue(key, qint64{4} * qint64{1073741824});
        QCOMPARE(budgetFromSettings(settings), std::optional<std::size_t>(std::size_t{4} << 30));
        settings.setValue(key, qint64{536870912}); // 0.5 GiB: raised to the 1 GiB floor
        QCOMPARE(budgetFromSettings(settings), std::optional<std::size_t>(kMinBudgetBytes));
        settings.setValue(key, 3.5e9);
        QCOMPARE(budgetFromSettings(settings), std::optional<std::size_t>(std::size_t{3'500'000'000}));
        settings.setValue(key, 0.25); // positive, a fraction of a byte: the floor
        QCOMPARE(budgetFromSettings(settings), std::optional<std::size_t>(kMinBudgetBytes));
        settings.setValue(key, 1e30);
        QCOMPARE(budgetFromSettings(settings),
                 std::optional<std::size_t>(std::numeric_limits<std::size_t>::max()));
        for (const QVariant& bad :
             {QVariant(0), QVariant(-5), QVariant(-2.5), QVariant(QStringLiteral("8 GB")), QVariant(true)}) {
            settings.setValue(key, bad);
            QVERIFY2(!budgetFromSettings(settings).has_value(), qPrintable(bad.toString()));
        }
        settings.setValue(key, QVariant()); // removed
        QVERIFY(!budgetFromSettings(settings).has_value());
    }

    void theTextsCarryTheFigures() {
        OpenBudgetCheck check;
        check.path = QStringLiteral("C:/logs/big.ld");
        check.estimateBytes = kLogEstimate;
        check.budgetBytes = 2 * kMiB;
        check.residentBytes = 0;
        QCOMPARE(budgetWarningText(check),
                 QStringLiteral(
                     "needs about 4.2 MB of memory once loaded, above the 2.0 MB memory budget (0 bytes "
                     "in use). Continue to open it anyway, or Cancel."));

        PerformanceSnapshot s;
        QCOMPARE(performanceLine(s), QString()); // no runtime behind it
        QCOMPARE(performanceDetails(s), QString());
        s.budgetBytes = 4 * kGiB;
        s.cache.residentBytes = 412 * kMiB;
        s.cache.pinnedBytes = 12 * kMiB;
        s.cache.chunks = 1234;
        s.cache.pinnedChunks = 3;
        s.cache.hits = 5678;
        s.cache.misses = 90;
        s.pool.workers = 15;
        s.pool.running = 2;
        s.pool.queued = {1, 0, 4};
        s.pool.parked = 0;
        QCOMPARE(performanceLine(s), QStringLiteral("Resident 412 MB of 4.0 GB"));
        QCOMPARE(
            performanceDetails(s),
            QStringLiteral("Memory: 412 MB resident of the 4.0 GB budget (10 %), 12 MB pinned; 1,234 chunks "
                           "(3 pinned); 5,678 hits, 90 misses, 0 evictions. Tasks: 2 running, 5 queued (1 "
                           "interactive, 0 visible, 4 background), 0 parked; 15 workers."));
        s.budgetBytes = 2 * kMiB;
        s.cache.residentBytes = kLogEstimate;
        QCOMPARE(performanceLine(s), QStringLiteral("Resident 4.2 MB of 2.0 MB (over budget)"));
        QVERIFY(performanceDetails(s).startsWith(
            QStringLiteral("Memory: 4.2 MB resident of the 2.0 MB budget (210 %)")));
    }

    // The independent estimate of the log above agrees with its hand-worked total.
    void theHandEstimateOfTheLog() {
        QCOMPARE(chunkBytes(65'536), std::uint64_t{445'184});
        QCOMPARE(chunkBytes(20'392), std::uint64_t{138'624});
        QCOMPARE(3 * channelBytes(217'000), kLogEstimate);
    }

    // AC-M4-02: an open above the budget waits before publication; nothing of it is decoded until Continue.
    void anOpenAboveTheBudgetWaitsForContinue() {
        const std::filesystem::path ld = writeLog("budget-continue", "big");
        Loader l(true);
        QVERIFY(untouched(l.rt));
        l.loader->open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(l.exceeded->count(), 1, kTimeoutMs);
        QCOMPARE(l.loader->pendingBudgetOpens(), 1);
        QCOMPARE(l.loader->pendingOpens(), 0); // the open itself finished
        const OpenBudgetCheck check = l.check();
        QCOMPARE(check.estimateBytes, kLogEstimate);
        QCOMPARE(check.budgetBytes, 2 * kMiB);
        QCOMPARE(check.residentBytes, std::uint64_t{0});
        QVERIFY(check.exceeds());
        QVERIFY(std::filesystem::path(check.path.toStdWString()).filename() == ld.filename());
        const OpenBudgetCheck* waiting = l.loader->pendingBudgetCheck(l.pendingId());
        QVERIFY(waiting != nullptr);
        QCOMPARE(waiting->estimateBytes, kLogEstimate);
        QVERIFY(l.loader->pendingBudgetCheck(l.pendingId() + 1) == nullptr);

        // Held: not published, no Import Log yet, no task decoding a chunk or building the summary.
        QTRY_VERIFY_WITH_TIMEOUT(quiet(l.rt), kTimeoutMs);
        QCOMPARE(l.published->count(), 0);
        QCOMPARE(l.logs->count(), 0);
        QVERIFY(l.loader->sources().empty());
        QVERIFY(untouched(l.rt));

        l.loader->continueOverBudget(l.pendingId());
        QCOMPARE(l.published->count(), 1); // at once, as any open's publication
        QCOMPARE(l.loader->pendingBudgetOpens(), 0);
        QVERIFY(l.loader->pendingBudgetCheck(l.pendingId()) == nullptr);
        QCOMPARE(l.loader->sources().size(), std::size_t{1});
        QCOMPARE(l.logs->count(), 1); // the open's Import Log, now
        QVERIFY(!l.codes().contains(QStringLiteral("open.budget_cancelled")));
        // Now the summary pass runs: every logged channel gets its SummaryPyramid.
        const auto src = l.loader->sources().front();
        QTRY_VERIFY_WITH_TIMEOUT(quiet(l.rt), kTimeoutMs);
        for (const model::Channel& c : src->layers.front().channels) {
            QVERIFY2(store::seriesValues(c.series).summary().complete(), c.desc.name.c_str());
        }
        // Answering again does nothing.
        l.loader->continueOverBudget(l.pendingId());
        l.loader->cancelOverBudget(l.pendingId());
        QCOMPARE(l.published->count(), 1);
        QCOMPARE(l.exceeded->count(), 1);
    }

    void cancelDropsTheOpenCleanly() {
        const std::filesystem::path ld = writeLog("budget-cancel", "big");
        Loader l(true);
        l.loader->open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(l.exceeded->count(), 1, kTimeoutMs);
        l.loader->cancelOverBudget(l.pendingId());
        QCOMPARE(l.loader->pendingBudgetOpens(), 0);
        QCOMPARE(l.codes(), QStringList{QStringLiteral("open.budget_cancelled")});
        QTRY_VERIFY_WITH_TIMEOUT(quiet(l.rt), kTimeoutMs);
        QCOMPARE(l.published->count(), 0);
        QVERIFY(l.loader->sources().empty());
        QVERIFY(untouched(l.rt));
        // Nothing is left behind: with room in the budget the same file opens at once.
        l.rt.cache().setBudgetBytes(std::size_t{64} << 20);
        l.loader->open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(l.published->count(), 1, kTimeoutMs);
        QCOMPARE(l.exceeded->count(), 1);
        QCOMPARE(l.loader->sources().size(), std::size_t{1});
    }

    void cancelOpensDropsTheWaitingOnes() {
        const std::filesystem::path ld = writeLog("budget-drop", "big");
        Loader l(true);
        l.loader->open(ld);
        l.loader->open(ld); // the same file twice: two opens wait
        QTRY_COMPARE_WITH_TIMEOUT(l.exceeded->count(), 2, kTimeoutMs);
        QCOMPARE(l.loader->pendingBudgetOpens(), 2);
        l.loader->cancelOpens();
        QCOMPARE(l.loader->pendingBudgetOpens(), 0);
        QCOMPARE(l.codes().count(QStringLiteral("open.budget_cancelled")), 2);
        l.loader->continueOverBudget(l.pendingId(0)); // too late: nothing to publish
        QCOMPARE(l.published->count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(quiet(l.rt), kTimeoutMs);
        QVERIFY(untouched(l.rt));
    }

    void aSecondOpenOfTheSameFileIsAlreadyOpen() {
        const std::filesystem::path ld = writeLog("budget-twice", "big");
        Loader l(true);
        l.loader->open(ld);
        l.loader->open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(l.exceeded->count(), 2, kTimeoutMs);
        l.loader->continueOverBudget(l.pendingId(1));
        QCOMPARE(l.published->count(), 1);
        // Continue on the other open finds the file open (06 section 19 item 6), as a second open would.
        const model::SourceId id = l.loader->sources().front()->id;
        l.loader->continueOverBudget(l.pendingId(0));
        QCOMPARE(l.published->count(), 1);
        QCOMPARE(l.loader->pendingBudgetOpens(), 0);
        QVERIFY(l.codes().contains(QStringLiteral("open.already_open")));
        l.loader->close(id);
        QVERIFY(l.loader->sources().empty());
    }

    void closingASourceKeepsTheOpensWaiting() {
        const std::filesystem::path ld = writeLog("budget-close", "big");
        Loader l(true);
        l.loader->open(ld);
        l.loader->open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(l.exceeded->count(), 2, kTimeoutMs);
        l.loader->continueOverBudget(l.pendingId(0));
        QCOMPARE(l.published->count(), 1);
        l.loader->close(l.loader->sources().front()->id);
        QCOMPARE(l.loader->pendingBudgetOpens(), 1);
        l.loader->continueOverBudget(l.pendingId(1)); // the file is closed now: it publishes
        QCOMPARE(l.published->count(), 2);
        QCOMPARE(l.loader->sources().size(), std::size_t{1});
    }

    void anOpenWithinTheBudgetOrWithoutAGateNeverWaits() {
        const std::filesystem::path ld = writeLog("budget-within", "big");
        {
            Loader l(true);
            l.rt.cache().setBudgetBytes(kLogEstimate); // equal is not above (exceeds() is estimate > budget)
            l.loader->open(ld);
            QTRY_COMPARE_WITH_TIMEOUT(l.published->count(), 1, kTimeoutMs);
            QCOMPARE(l.exceeded->count(), 0);
        }
        {
            Loader l(false); // headless: no gate, whatever the budget
            QVERIFY(l.loader->budgetGate() == nullptr);
            l.loader->open(ld);
            QTRY_COMPARE_WITH_TIMEOUT(l.published->count(), 1, kTimeoutMs);
            QCOMPARE(l.exceeded->count(), 0);
            QCOMPARE(l.loader->pendingBudgetOpens(), 0);
        }
    }
};

QTEST_GUILESS_MAIN(TstMemoryBudget)
#include "tst_memory_budget.moc"
