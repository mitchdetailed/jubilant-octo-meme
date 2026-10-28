// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// M3 item 18 (D-210 (b)): SourceLoader announces the end of its summary pass (02 section 3.2 rule 10) with
// summariesReady(id), so a component built while the SummaryPyramids were still incomplete can rebuild its
// extents. Checked against the store itself: when the signal arrives every logged channel's pyramid is
// complete (SummaryPyramid::complete()), it comes after sourcePublished and once per publication, never for a
// file closed before its pass ended, and a file closed and opened again is announced by its own pass.

#include "../qtmodel_test_support.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/store/series.hpp>

#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

Q_DECLARE_METATYPE(std::shared_ptr<const esses::model::DataSource>)
Q_DECLARE_METATYPE(esses::model::SourceId)

namespace {

AppRuntimeOptions runtimeOptions() {
    AppRuntimeOptions o;
    o.workers = 4;
    o.cacheBudget = std::size_t{256} << 20;
    return o;
}

// Every task of the pool has completed and every result it posted has been delivered.
bool quiet(AppRuntime& rt) {
    const TaskPool::Stats s = rt.pool().stats();
    std::size_t queued = 0;
    for (const std::size_t q : s.queued) {
        queued += q;
    }
    return queued == 0 && s.parked == 0 && s.running == 0 && rt.dispatcher().pending() == 0;
}

// Whether every logged channel of `src` has its complete SummaryPyramid; `channels` counts them.
bool everyPyramidComplete(const model::DataSource& src, int& channels) {
    channels = 0;
    bool complete = true;
    for (const model::DataLayer& layer : src.layers) {
        for (const model::Channel& c : layer.channels) {
            if (c.desc.source != model::ChannelSource::Logged) {
                continue;
            }
            ++channels;
            complete = complete && store::seriesValues(c.series).summary().complete();
        }
    }
    return complete;
}

// One announcement as the test saw it.
struct Seen {
    model::SourceId id;
    bool published = false; // sourcePublished of that source came first
    bool complete = false;  // every logged pyramid was complete then
    int channels = 0;
};

struct Rig {
    AppRuntime rt{runtimeOptions()};
    SourceLoader loader{rt.pool(), rt.dispatcher(), shippedUnits()};
    std::vector<model::SourceId> published;
    std::vector<Seen> seen;

    Rig() {
        loader.setUseSidecars(false);
        QObject::connect(
            &loader, &SourceLoader::sourcePublished, &loader,
            [this](const std::shared_ptr<const model::DataSource>& s) { published.push_back(s->id); });
        QObject::connect(&loader, &SourceLoader::summariesReady, &loader, [this](const model::SourceId& id) {
            Seen s;
            s.id = id;
            s.published = std::find(published.begin(), published.end(), id) != published.end();
            if (const std::shared_ptr<const model::DataSource> src = loader.source(id)) {
                s.complete = everyPyramidComplete(*src, s.channels);
            }
            seen.push_back(s);
        });
    }
    std::filesystem::path writeSource(const std::string& dir) {
        SyntheticOptions o;
        o.path = dir + ".ld";
        return writeLd(*makeSource(o, rt.cache()), scratchDir(dir), "summaries");
    }
};

} // namespace

class TstSummariesReady : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
            "std::shared_ptr<const esses::model::DataSource>");
        qRegisterMetaType<model::SourceId>("esses::model::SourceId");
    }

    // After sourcePublished, once, with every logged channel's pyramid complete.
    void announcedOnceAfterThePassWithEveryPyramidComplete() {
        Rig r;
        r.loader.open(r.writeSource("sr-once"));
        QTRY_COMPARE_WITH_TIMEOUT(r.seen.size(), std::size_t{1}, 60000);
        QTRY_VERIFY_WITH_TIMEOUT(quiet(r.rt), 60000);
        QCOMPARE(r.seen.size(), std::size_t{1}); // nothing more once everything has landed
        QCOMPARE(r.published.size(), std::size_t{1});
        const Seen& s = r.seen.front();
        QCOMPARE(s.id, r.published.front());
        QVERIFY(s.published);
        QVERIFY(s.complete);
        QVERIFY(s.channels >= 3); // the wheel speeds and the throttle of the synthetic source
        r.loader.closeAll();
    }

    // A file closed in the same turn as its publication: the pass is cancelled or finds it closed.
    void neverAnnouncedForASourceClosedBeforeItsPass() {
        Rig r;
        QObject::connect(&r.loader, &SourceLoader::sourcePublished, &r.loader,
                         [&r](const std::shared_ptr<const model::DataSource>& s) { r.loader.close(s->id); });
        r.loader.open(r.writeSource("sr-closed"));
        QTRY_COMPARE_WITH_TIMEOUT(r.published.size(), std::size_t{1}, 60000);
        QTRY_VERIFY_WITH_TIMEOUT(quiet(r.rt), 60000);
        QVERIFY(r.loader.sources().empty());
        QVERIFY(r.seen.empty());
    }

    // The same file (the same SourceId) closed and opened again before its first pass ended: one
    // announcement, for the second publication, when its own pyramids are complete.
    void aReopenedFileIsAnnouncedByItsOwnPass() {
        Rig r;
        const std::filesystem::path ld = r.writeSource("sr-reopen");
        bool reopened = false;
        QObject::connect(&r.loader, &SourceLoader::sourcePublished, &r.loader,
                         [&](const std::shared_ptr<const model::DataSource>& s) {
                             if (!reopened) {
                                 reopened = true;
                                 r.loader.close(s->id);
                                 r.loader.open(ld);
                             }
                         });
        r.loader.open(ld);
        QTRY_COMPARE_WITH_TIMEOUT(r.published.size(), std::size_t{2}, 60000);
        QTRY_COMPARE_WITH_TIMEOUT(r.seen.size(), std::size_t{1}, 60000);
        QTRY_VERIFY_WITH_TIMEOUT(quiet(r.rt), 60000);
        QCOMPARE(r.seen.size(), std::size_t{1});
        QCOMPARE(r.published[0], r.published[1]);
        QCOMPARE(r.seen.front().id, r.published[1]);
        QVERIFY(r.seen.front().complete);
        QCOMPARE(r.loader.sources().size(), std::size_t{1});
        r.loader.closeAll();
    }
};

QTEST_GUILESS_MAIN(TstSummariesReady)
#include "tst_summaries_ready.moc"
