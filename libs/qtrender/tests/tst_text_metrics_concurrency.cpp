// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// QtTextMetrics thread safety (04 section 5.4): 8 threads measure the same strings while the GUI thread swaps
// schemes (new snapshots with new generations); every result equals the single-threaded result of the
// snapshot it came from. Also: snapshots sharing a generation never share a per-thread cache entry, and the
// metrics agree with SchemeFonts (the fonts RasterBackend draws with).

#include "qtrender_test_support.hpp"

#include <esses/qtrender/qt_text_metrics.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QTest>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace esses;

namespace {

const std::vector<std::string> kStrings = {"0",
                                           "12.345",
                                           "Engine RPM",
                                           "Corr Speed",
                                           "1:23.456",
                                           "Throttle Pos [%]",
                                           "\xC2\xB0"
                                           "C",
                                           "Lap 12"};
constexpr std::array<float, 3> kDprs = {1.0F, 1.5F, 2.0F};

struct Sample {
    std::vector<float> values; // advance, line height, ascent per (style, string, dpr)
};

Sample measure(const plot::ITextMetrics& m) {
    Sample s;
    for (std::size_t style = 0; style < qt::QtTextMetrics::kStyles; ++style) {
        const auto id = static_cast<plot::TextStyleId>(style);
        for (const float dpr : kDprs) {
            s.values.push_back(m.lineHeightPx(id, dpr));
            s.values.push_back(m.ascentPx(id, dpr));
            for (const std::string& text : kStrings) {
                s.values.push_back(m.advancePx(id, text, dpr));
            }
        }
    }
    return s;
}

} // namespace

class TestTextMetricsConcurrency : public QObject {
    Q_OBJECT
public:
    static void initMain() { qttest::prepareFontEnvironment(); }

private Q_SLOTS:
    void initTestCase() { QVERIFY(qttest::registerGoldenFonts() >= 0); }

    void metricsAgreeWithSchemeFonts() {
        const project::Scheme scheme = qttest::testScheme();
        const qt::SchemeFonts fonts(scheme);
        const qt::QtTextMetrics metrics(scheme, 7);
        QCOMPARE(metrics.generation(), std::uint64_t{7});
        for (std::size_t style = 0; style < qt::QtTextMetrics::kStyles; ++style) {
            const auto id = static_cast<plot::TextStyleId>(style);
            const QFontMetricsF fm(fonts.font(id));
            for (const float dpr : kDprs) {
                QCOMPARE(metrics.advancePx(id, "Corr Dist", dpr),
                         static_cast<float>(fm.horizontalAdvance(QStringLiteral("Corr Dist")) *
                                            static_cast<qreal>(dpr)));
                QCOMPARE(metrics.lineHeightPx(id, dpr),
                         static_cast<float>(fm.height() * static_cast<qreal>(dpr)));
                QCOMPARE(metrics.ascentPx(id, dpr),
                         static_cast<float>(fm.ascent() * static_cast<qreal>(dpr)));
            }
            QCOMPARE(QString::fromStdString(metrics.description(id).family), QStringLiteral("Noto Sans"));
        }
        QVERIFY(metrics.advancePx(plot::TextStyleId::Gauge, "88", 1.0F) >
                metrics.advancePx(plot::TextStyleId::Band, "88", 1.0F));
        QCOMPARE(metrics.advancePx(plot::TextStyleId::Axis, "", 1.0F), 0.0F);
    }

    void snapshotsSharingAGenerationDoNotShareCaches() {
        const qt::QtTextMetrics small(qttest::testScheme(0.0), 1);
        const qt::QtTextMetrics large(qttest::testScheme(6.0), 1); // same generation, other scheme
        const float a = small.advancePx(plot::TextStyleId::Key, "Corr Speed", 1.0F);
        const float b = large.advancePx(plot::TextStyleId::Key, "Corr Speed", 1.0F);
        QVERIFY2(
            b > a * 1.3F,
            qPrintable(QStringLiteral("%1 vs %2").arg(static_cast<double>(a)).arg(static_cast<double>(b))));
        QCOMPARE(small.advancePx(plot::TextStyleId::Key, "Corr Speed", 1.0F), a);
    }

    void eightThreadsWhileTheGuiSwapsSchemes() {
        // Reference results, single-threaded, for the two schemes the GUI alternates between.
        const std::array<project::Scheme, 2> schemes = {qttest::testScheme(0.0), qttest::testScheme(3.0)};
        std::array<Sample, 2> reference;
        for (std::size_t i = 0; i < 2; ++i) {
            reference[i] = measure(*qt::QtTextMetrics::create(schemes[i], 1000 + i));
        }
        QVERIFY(reference[0].values != reference[1].values);

        std::mutex mutex;
        std::shared_ptr<const qt::QtTextMetrics> current = qt::QtTextMetrics::create(schemes[0], 0);
        std::atomic<bool> stop{false};
        std::atomic<int> mismatches{0};
        std::atomic<int> rounds{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < 8; ++t) {
            workers.emplace_back([&] {
                while (!stop.load()) {
                    std::shared_ptr<const qt::QtTextMetrics> snapshot;
                    {
                        const std::scoped_lock lock(mutex);
                        snapshot = current; // a builder captures the shared_ptr (01 section 5.3 rule 9)
                    }
                    const Sample s = measure(*snapshot);
                    if (s.values != reference[snapshot->generation() % 2].values) {
                        mismatches.fetch_add(1);
                    }
                    rounds.fetch_add(1);
                }
            });
        }
        for (std::uint64_t generation = 1; generation <= 200; ++generation) {
            auto next = qt::QtTextMetrics::create(schemes[generation % 2], generation); // on the GUI thread
            {
                const std::scoped_lock lock(mutex);
                current = std::move(next);
            }
            QCoreApplication::processEvents();
            if (generation % 20 == 0) {
                std::this_thread::yield();
            }
        }
        while (rounds.load() < 64) {
            std::this_thread::yield();
        }
        stop.store(true);
        for (std::thread& w : workers) {
            w.join();
        }
        QCOMPARE(mismatches.load(), 0);
        QVERIFY(rounds.load() >= 64);
    }
};

QTEST_MAIN(TestTextMetricsConcurrency)
#include "tst_text_metrics_concurrency.moc"
