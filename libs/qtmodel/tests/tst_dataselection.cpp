// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataSelectionModel (06 sections 5.1-5.6, D-017, D-109, D-110, D-135; AC-M1-01, AC-M1-07 candidate rule):
// one test per row of the 06 section 5.2 table, the changed() bits of every operation, compact renumbering,
// the TT 2485 fastest-lap rule, filters, re-resolution after a sidecar edit, close and restore, and the
// invariants over a random operation sequence.

#include "qtmodel_test_support.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/qtmodel/data_selection_model.hpp>

#include <QSignalSpy>
#include <QTest>
#include <random>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;
using Change = DataSelectionModel::Change;
using Changes = DataSelectionModel::Changes;

Q_DECLARE_METATYPE(esses::plot::SelectionSnapshot)
Q_DECLARE_METATYPE(esses::qt::DataSelectionModel::Changes)

namespace {

// Laps of source A (Outings:Laps): 0 Out Lap 40 s, 1 Lap 1 50 s, 2 Lap 2 48 s, 3 Lap 3 48.3 s, 4 In Lap 30 s.
// The out and in laps are shorter than every flying lap, so CTRL+F proves they are never candidates.
SyntheticOptions optionsA() {
    SyntheticOptions o;
    o.path = "a.ld";
    o.lapSeconds = {40.0, 50.0, 48.0, 48.3, 30.0};
    return o;
}

// Source B: 0 Out Lap, 1 Lap 1 45 s, 2 Lap 2 44 s, 3 In Lap.
SyntheticOptions optionsB() {
    SyntheticOptions o;
    o.path = "b.ld";
    o.lapSeconds = {30.0, 45.0, 44.0, 20.0};
    return o;
}

struct Fixture {
    store::ChunkCache cache{std::size_t{256} << 20};
    std::shared_ptr<const model::DataSource> a = makeSource(optionsA(), cache);
    std::shared_ptr<const model::DataSource> b = makeSource(optionsB(), cache);
    DataSelectionModel selection;

    Fixture() {
        selection.setSidecarCommit(
            [this](std::shared_ptr<const model::DataSource> current, model::SidecarDocument doc) {
                selection.sourceReplaced(current->withSidecar(
                    std::move(doc), nullptr, analysis::RuleRunner::circuitDefault().rebuildFunction()));
            });
    }
    model::RangeRef lapA(std::size_t i) const { return rangeRef(*selection.source(a->id), i); }
    model::RangeRef lapB(std::size_t i) const { return rangeRef(*selection.source(b->id), i); }
};

// 06 section 5.2 invariants (also asserted in debug builds inside the model).
void checkInvariants(const DataSelectionModel& m) {
    const plot::SelectionSnapshot& s = m.snapshot();
    if (m.hasMain()) {
        QVERIFY(!(s.overlay0 && *s.overlay0 == s.main));
        QVERIFY(std::find(s.overlays.begin(), s.overlays.end(), s.main) == s.overlays.end());
    }
    if (s.overlay0) {
        QVERIFY(std::find(s.overlays.begin(), s.overlays.end(), *s.overlay0) == s.overlays.end());
        QCOMPARE(m.overlayNumber(*s.overlay0), 0);
    }
    for (std::size_t i = 0; i < s.overlays.size(); ++i) {
        QCOMPARE(m.overlayNumber(s.overlays[i]), static_cast<int>(i) + 1); // 1..n contiguous
        QVERIFY(m.resolve(s.overlays[i]) != nullptr);
    }
}

Changes lastChanges(const QSignalSpy& spy) {
    return spy.isEmpty() ? Changes() : spy.last().at(1).value<Changes>();
}

} // namespace

class TstDataSelection : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<plot::SelectionSnapshot>();
        qRegisterMetaType<Changes>();
    }

    void fixtureHasTheExpectedLaps() {
        Fixture f;
        QCOMPARE(rangeCount(*f.a), std::size_t{5});
        QCOMPARE(rangeAt(*f.a, 0).lapClass, model::LapClass::Out);
        QCOMPARE(rangeAt(*f.a, 1).lapClass, model::LapClass::Flying);
        QCOMPARE(rangeAt(*f.a, 1).number, 1);
        QCOMPARE(rangeAt(*f.a, 4).lapClass, model::LapClass::In);
        QVERIFY(rangeAt(*f.a, 2).trusted);
        QVERIFY(!rangeAt(*f.a, 0).trusted);
        QCOMPARE(rangeCount(*f.a, "Outings"), std::size_t{1});
    }

    // sourcePublished: the first source sets Main (first displayed range); later ones leave the selection.
    void sourcePublishedSetsMainOnce() {
        Fixture f;
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        QSignalSpy sources(&f.selection, &DataSelectionModel::sourcesChanged);
        QVERIFY(!f.selection.hasMain());
        f.selection.sourcePublished(f.a);
        QCOMPARE(sources.count(), 1);
        QVERIFY(f.selection.hasMain());
        QCOMPARE(f.selection.snapshot().main, f.lapA(0));
        QCOMPARE(changed.count(), 1);
        QVERIFY(lastChanges(changed).testFlag(Change::Main));
        f.selection.sourcePublished(f.b);
        QCOMPARE(sources.count(), 2);
        QCOMPARE(changed.count(), 1); // selection unchanged
        QCOMPARE(f.selection.sources().size(), std::size_t{2});
        QCOMPARE(f.selection.snapshot().main, f.lapA(0));
    }

    void sourcePublishedSelectsFastestWithTheOption() {
        Fixture f;
        f.selection.setSelectFastestOnLoad(true);
        f.selection.sourcePublished(f.a);
        QCOMPARE(f.selection.snapshot().main, f.lapA(2));
    }

    // setMain (ENTER): one slot per range (D-109); overlays renumber.
    void setMainVacatesOverlaySlots() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.toggleOverlay0(f.lapA(1));
        f.selection.toggleOverlay(f.lapA(2));
        f.selection.toggleOverlay(f.lapA(3));
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.setMain(f.lapA(2)); // was Overlay 1
        QCOMPARE(changed.count(), 1);
        QCOMPARE(lastChanges(changed), Changes(Change::Main) | Change::Overlays);
        QCOMPARE(f.selection.snapshot().main, f.lapA(2));
        QCOMPARE(f.selection.overlayNumber(f.lapA(3)), 1); // renumbered compactly
        f.selection.setMain(f.lapA(1));                    // was Overlay 0
        QCOMPARE(lastChanges(changed), Changes(Change::Main) | Change::Overlay0);
        QVERIFY(!f.selection.snapshot().overlay0);
        f.selection.setMain(f.lapA(1)); // already Main: no change
        QCOMPARE(changed.count(), 2);
        checkInvariants(f.selection);
    }

    void setMainRejectsUnknownRanges() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        QSignalSpy hint(&f.selection, &DataSelectionModel::hint);
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.setMain(rangeRef(*f.b, 1)); // B is not open
        QCOMPARE(hint.count(), 1);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(f.selection.snapshot().main, f.lapA(0));
    }

    // toggleOverlay0 (CTRL+ENTER): radio; not on Main; leaves the numbered overlays.
    void toggleOverlay0IsARadio() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        QSignalSpy hint(&f.selection, &DataSelectionModel::hint);
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.toggleOverlay0(f.lapA(0)); // Main
        QCOMPARE(hint.count(), 1);
        QCOMPARE(changed.count(), 0);
        f.selection.toggleOverlay0(f.lapA(1));
        QCOMPARE(*f.selection.snapshot().overlay0, f.lapA(1));
        QCOMPARE(lastChanges(changed), Changes(Change::Overlay0));
        f.selection.toggleOverlay0(f.lapA(2)); // radio: replaces
        QCOMPARE(*f.selection.snapshot().overlay0, f.lapA(2));
        f.selection.toggleOverlay0(f.lapA(2)); // toggles off
        QVERIFY(!f.selection.snapshot().overlay0);
        f.selection.toggleOverlay(f.lapA(3));
        f.selection.toggleOverlay(f.lapA(1));
        f.selection.toggleOverlay0(f.lapA(3)); // Overlay 1 moves to Overlay 0
        QCOMPARE(lastChanges(changed), Changes(Change::Overlay0) | Change::Overlays);
        QCOMPARE(f.selection.overlayNumber(f.lapA(3)), 0);
        QCOMPARE(f.selection.overlayNumber(f.lapA(1)), 1);
        checkInvariants(f.selection);
    }

    // toggleOverlay (ALT+ENTER): appended in selection order, removal renumbers compactly (D-017).
    void toggleOverlayRenumbersCompactly() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.sourcePublished(f.b);
        f.selection.toggleOverlay(f.lapA(3));
        f.selection.toggleOverlay(f.lapB(1)); // overlays may come from another source
        f.selection.toggleOverlay(f.lapA(1));
        QCOMPARE(f.selection.overlayNumber(f.lapA(3)), 1);
        QCOMPARE(f.selection.overlayNumber(f.lapB(1)), 2);
        QCOMPARE(f.selection.overlayNumber(f.lapA(1)), 3);
        QCOMPARE(*f.selection.referenceOverlay(), f.lapA(3)); // lowest number without Overlay 0
        f.selection.toggleOverlay(f.lapB(1));
        QCOMPARE(f.selection.overlayNumber(f.lapA(1)), 2);
        QCOMPARE(f.selection.overlayNumber(f.lapB(1)), -1);
        QSignalSpy hint(&f.selection, &DataSelectionModel::hint);
        f.selection.toggleOverlay(f.lapA(0)); // Main
        f.selection.toggleOverlay0(f.lapA(2));
        f.selection.toggleOverlay(f.lapA(2)); // Overlay 0
        QCOMPARE(hint.count(), 2);
        QCOMPARE(*f.selection.referenceOverlay(), f.lapA(2)); // Overlay 0 wins
        checkInvariants(f.selection);
    }

    // toggleOverlaysHidden (F4) keeps the sets; clearOverlays (SHIFT+F4) resets them and the hidden flag.
    void hideAndClearOverlays() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.toggleOverlay0(f.lapA(1));
        f.selection.toggleOverlay(f.lapA(2));
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.toggleOverlaysHidden();
        QCOMPARE(lastChanges(changed), Changes(Change::Hidden));
        QVERIFY(f.selection.snapshot().overlaysHidden);
        QVERIFY(f.selection.visibleOverlays().empty());
        QVERIFY(!f.selection.referenceOverlay()); // 06 section 24 item 4
        QCOMPARE(f.selection.snapshot().overlays.size(), std::size_t{1});
        QCOMPARE(f.selection.snapshot().visibleSlots().size(), std::size_t{1});
        f.selection.clearOverlays();
        QCOMPARE(lastChanges(changed), Changes(Change::Overlay0) | Change::Overlays | Change::Hidden);
        QVERIFY(!f.selection.snapshot().overlay0);
        QVERIFY(f.selection.snapshot().overlays.empty());
        QVERIFY(!f.selection.snapshot().overlaysHidden);
        const int before = static_cast<int>(changed.count());
        f.selection.clearOverlays(); // nothing to clear
        QCOMPARE(static_cast<int>(changed.count()), before);
    }

    // swapMainOverlay0 (Q).
    void swapMainAndOverlay0() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        QSignalSpy hint(&f.selection, &DataSelectionModel::hint);
        f.selection.swapMainOverlay0();
        QCOMPARE(hint.count(), 1); // no Overlay 0
        f.selection.setMain(f.lapA(1));
        f.selection.toggleOverlay0(f.lapA(2));
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.swapMainOverlay0();
        QCOMPARE(lastChanges(changed), Changes(Change::Main) | Change::Overlay0);
        QCOMPARE(f.selection.snapshot().main, f.lapA(2));
        QCOMPARE(*f.selection.snapshot().overlay0, f.lapA(1));
    }

    // nextRange/prevRange (N/P): displayed order within the Main source, no wrap, vacates overlay slots.
    void nextAndPreviousRange() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.sourcePublished(f.b);
        f.selection.setMain(f.lapA(3));
        f.selection.toggleOverlay(f.lapA(4));
        f.selection.nextRange();
        QCOMPARE(f.selection.snapshot().main, f.lapA(4));
        QVERIFY(f.selection.snapshot().overlays.empty()); // left its overlay slot
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.nextRange(); // the last range: no wrap, never into source B
        QCOMPARE(changed.count(), 0);
        QCOMPARE(f.selection.snapshot().main, f.lapA(4));
        for (int i = 0; i < 6; ++i) {
            f.selection.prevRange();
        }
        QCOMPARE(f.selection.snapshot().main, f.lapA(0));
        QCOMPARE(changed.count(), 4);
    }

    void nextRangeFollowsTheFilteredOrder() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.setMain(f.lapA(1));
        f.selection.setFilter(f.a->id, FilterSpec{FilterSpec::Kind::LapNumbers, "1, 3"});
        const auto shown = f.selection.displayedRanges(f.a->id);
        QCOMPARE(shown.size(), std::size_t{2}); // Lap 1 and Lap 3
        f.selection.nextRange();
        QCOMPARE(f.selection.snapshot().main, f.lapA(3));
        // Filters never hide a slot (06 section 5.4): Main stays displayed.
        f.selection.setFilter(f.a->id, FilterSpec{FilterSpec::Kind::LapNumbers, "1"});
        QVERIFY(f.selection.filterAccepts(f.lapA(3)));
        QCOMPARE(f.selection.displayedRanges(f.a->id).size(), std::size_t{2});
        f.selection.prevRange();
        QCOMPARE(f.selection.snapshot().main, f.lapA(1));
    }

    // selectFastest (CTRL+F): trusted flying laps if any (never out/in/pit; TT 2485).
    void selectFastestUsesTheCandidateRule() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.sourcePublished(f.b);
        f.selection.setMain(f.lapA(4));
        f.selection.toggleOverlay(f.lapA(2));
        f.selection.selectFastest();
        QCOMPARE(f.selection.snapshot().main, f.lapA(2)); // 48 s; the 40 s out and 30 s in laps never count
        QVERIFY(f.selection.snapshot().overlays.empty());
        // Untrust Lap 2: the fastest trusted flying lap is Lap 3 (48.3 s).
        f.selection.setTrusted(f.lapA(2), false);
        QVERIFY(!f.selection.resolve(f.lapA(2))->trusted);
        f.selection.selectFastest();
        QCOMPARE(f.selection.snapshot().main, f.lapA(3));
        // No trusted flying lap at all: flying/partial laps, trusted or not; still never the out/in laps.
        f.selection.setTrusted(f.lapA(1), false);
        f.selection.setTrusted(f.lapA(3), false);
        f.selection.setMain(f.lapA(0));
        f.selection.selectFastest();
        QCOMPARE(f.selection.snapshot().main, f.lapA(2));
        // The Main source decides: B's fastest is its Lap 2.
        f.selection.setMain(f.lapB(0));
        f.selection.selectFastest();
        QCOMPARE(f.selection.snapshot().main, f.lapB(2));
    }

    void selectFastestWithoutCandidatesIsAHint() {
        store::ChunkCache cache{std::size_t{64} << 20};
        SyntheticOptions o;
        o.path = "outin.ld";
        o.lapSeconds = {40.0, 30.0}; // an out lap and an in lap only
        const auto src = makeSource(o, cache);
        DataSelectionModel selection;
        selection.sourcePublished(src);
        QSignalSpy hint(&selection, &DataSelectionModel::hint);
        QSignalSpy changed(&selection, &DataSelectionModel::changed);
        selection.selectFastest();
        selection.selectAllFast();
        QCOMPARE(hint.count(), 2);
        QCOMPARE(changed.count(), 0);
    }

    // selectAllFast (CTRL+ALT+F): fastest as Main, the other candidates within the tolerance as overlays.
    void selectAllFastUsesTheTolerance() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.toggleOverlay0(f.lapA(4));
        f.selection.toggleOverlay(f.lapA(1));
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.selectAllFast();
        QCOMPARE(f.selection.snapshot().main, f.lapA(2));
        QCOMPARE(f.selection.snapshot().overlays,
                 std::vector<model::RangeRef>{f.lapA(3)});     // 48.3 <= 48 x 1.01
        QCOMPARE(*f.selection.snapshot().overlay0, f.lapA(4)); // untouched
        QCOMPARE(lastChanges(changed), Changes(Change::Main) | Change::Overlays);
        f.selection.setFastLapTolerancePct(5.0); // 50 <= 50.4
        f.selection.selectAllFast();
        QCOMPARE(f.selection.snapshot().overlays, (std::vector<model::RangeRef>{f.lapA(1), f.lapA(3)}));
        checkInvariants(f.selection);
    }

    void singleColourMain() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.setSingleColourMain(true);
        QVERIFY(f.selection.snapshot().singleColourMain);
        QCOMPARE(lastChanges(changed), Changes(Change::Colour));
        f.selection.setSingleColourMain(true);
        QCOMPARE(changed.count(), 1);
    }

    // setTrusted writes a range override through the sidecar commit; the slots follow by RangeId.
    void setTrustedRoundTripsThroughTheSidecar() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.setMain(f.lapA(2));
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.setTrusted(f.lapA(0), true); // the out lap
        QVERIFY(f.selection.resolve(f.lapA(0))->trusted);
        QVERIFY(lastChanges(changed).testFlag(Change::Sources));
        QCOMPARE(f.selection.snapshot().main, f.lapA(2));
        const auto src = f.selection.source(f.a->id);
        QCOMPARE(src->version, f.a->version + 1);
        QCOMPARE(src->sidecar.layer("Normal").rangeOverrides.size(), std::size_t{1});
        f.selection.setTrusted(f.lapA(0), false);
        QVERIFY(!f.selection.resolve(f.lapA(0))->trusted);
        QCOMPARE(f.selection.source(f.a->id)->sidecar.layer("Normal").rangeOverrides.size(), std::size_t{1});
        DataSelectionModel noCommit;
        noCommit.sourcePublished(f.a);
        QSignalSpy hint(&noCommit, &DataSelectionModel::hint);
        noCommit.setTrusted(rangeRef(*f.a, 1), false);
        QCOMPARE(hint.count(), 1);
    }

    // sourceReplaced: by RangeId, else the same lap number, else the slot is dropped (06 section 5.6).
    void sourceReplacedReResolvesSlots() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.setMain(f.lapA(2));       // starts at the second beacon
        f.selection.toggleOverlay(f.lapA(1)); // starts at the first beacon
        f.selection.toggleOverlay(f.lapA(3));
        // Ignore the second beacon: Lap 1 now runs to the third beacon; old Lap 2's start marker is gone, old
        // Lap 3 becomes Lap 2 (same RangeId) and the in lap follows.
        model::SidecarDocument doc = f.a->sidecar;
        model::MarkerEdit ignore;
        ignore.op = model::MarkerEditOp::Ignore;
        ignore.ref = rangeAt(*f.a, 2).startMarker;
        QVERIFY(doc.applyEdit("Normal", ignore).ok());
        const auto next =
            f.a->withSidecar(doc, nullptr, analysis::RuleRunner::circuitDefault().rebuildFunction());
        QCOMPARE(rangeCount(*next), std::size_t{4});
        QSignalSpy hint(&f.selection, &DataSelectionModel::hint);
        f.selection.sourceReplaced(next);
        // Main fell back to lap number 2, which is old Lap 3 (by id) - that overlay leaves its slot (D-109).
        QCOMPARE(f.selection.snapshot().main, rangeRef(*next, 2));
        QCOMPARE(f.selection.resolve(f.selection.snapshot().main)->number, 2);
        QCOMPARE(f.selection.snapshot().overlays, std::vector<model::RangeRef>{rangeRef(*next, 1)});
        QCOMPARE(f.selection.source(f.a->id), next);
        checkInvariants(f.selection);
        // Remove every beacon: no lap survives; Main falls back to the first displayed range.
        model::SidecarDocument doc2 = next->sidecar;
        for (const model::Marker& m : next->layers.front().markers) {
            if (m.cls == model::MarkerClass::BCN) {
                QVERIFY(doc2.applyEdit("Normal",
                                       model::MarkerEdit{m.id, model::MarkerEditOp::Ignore, 0, 0.0, "m"})
                            .ok());
            }
        }
        const auto bare =
            next->withSidecar(doc2, nullptr, analysis::RuleRunner::circuitDefault().rebuildFunction());
        f.selection.sourceReplaced(bare);
        QVERIFY(f.selection.hasMain());
        QCOMPARE(f.selection.snapshot().main.groupPath, std::string("Outings"));
        QVERIFY(f.selection.snapshot().overlays.empty());
        QVERIFY(hint.count() >= 2);
        checkInvariants(f.selection);
    }

    // closeSource: Main moves to the next source (the previous when none follows); slots of the source leave.
    void closeSourceMovesMain() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.sourcePublished(f.b);
        f.selection.setMain(f.lapA(2));
        f.selection.toggleOverlay0(f.lapA(1));
        f.selection.toggleOverlay(f.lapB(1));
        f.selection.toggleOverlay(f.lapA(3));
        QSignalSpy changed(&f.selection, &DataSelectionModel::changed);
        f.selection.closeSource(f.a->id);
        QCOMPARE(lastChanges(changed),
                 Changes(Change::Main) | Change::Overlay0 | Change::Overlays | Change::Sources);
        QCOMPARE(f.selection.sources().size(), std::size_t{1});
        QCOMPARE(f.selection.snapshot().main, f.lapB(0)); // the next source's first displayed range
        QVERIFY(!f.selection.snapshot().overlay0);
        QCOMPARE(f.selection.snapshot().overlays, std::vector<model::RangeRef>{f.lapB(1)}); // renumbered to 1
        checkInvariants(f.selection);
        f.selection.closeAllSources();
        QVERIFY(!f.selection.hasMain());
        QVERIFY(f.selection.sources().empty());
    }

    void closeSourceKeepsOtherOverlays() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.sourcePublished(f.b);
        f.selection.setMain(f.lapB(2));
        f.selection.toggleOverlay(f.lapA(1));
        f.selection.toggleOverlay(f.lapB(1));
        f.selection.closeSource(f.a->id);
        QCOMPARE(f.selection.snapshot().main, f.lapB(2));
        QCOMPARE(f.selection.snapshot().overlays, std::vector<model::RangeRef>{f.lapB(1)}); // renumbered to 1
        f.selection.sourcePublished(f.a);
        f.selection.closeSource(f.b->id); // Main's source is the last: the previous source takes over
        QCOMPARE(f.selection.snapshot().main.src, f.a->id);
    }

    // restore: by (hash, layer, groupPath, startMarker), else (path, layer, groupPath, name); drops the rest.
    void restoreRoundTripsTheKeys() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.sourcePublished(f.b);
        f.selection.setMain(f.lapA(2));
        f.selection.toggleOverlay0(f.lapB(1));
        f.selection.toggleOverlay(f.lapA(3));
        f.selection.toggleOverlaysHidden();
        f.selection.setSingleColourMain(true);
        project::SelectionKeys keys = f.selection.selectionKeys();
        QCOMPARE(keys.main->source.hash, "xxh3:" + f.a->id.hex());
        QCOMPARE(keys.main->name, std::string("Lap 2"));
        QVERIFY(!keys.main->startMarker.empty());
        // A key that matches nothing, and one that only matches by path and name.
        project::RangeKey ghost = *keys.main;
        ghost.source.hash = "xxh3:00000000000000000000000000000000";
        ghost.startMarker.clear();
        ghost.source.path = "nowhere.ld";
        keys.overlays.push_back(ghost);
        project::RangeKey byName = *keys.overlay0;
        byName.source.hash = "xxh3:00000000000000000000000000000000";
        keys.overlay0 = byName;

        DataSelectionModel other;
        other.sourcePublished(f.a);
        other.sourcePublished(f.b);
        QSignalSpy hint(&other, &DataSelectionModel::hint);
        other.restore(keys);
        QCOMPARE(hint.count(), 1); // the ghost
        QCOMPARE(other.snapshot().main, f.lapA(2));
        QCOMPARE(*other.snapshot().overlay0, f.lapB(1));
        QCOMPARE(other.snapshot().overlays, std::vector<model::RangeRef>{f.lapA(3)});
        QVERIFY(other.snapshot().overlaysHidden);
        QVERIFY(other.snapshot().singleColourMain);
    }

    void filtersAndApplyToAll() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.sourcePublished(f.b);
        QSignalSpy filterChanged(&f.selection, &DataSelectionModel::filterChanged);
        f.selection.setFilter(f.a->id, FilterSpec{FilterSpec::Kind::Fastest3, {}});
        QCOMPARE(filterChanged.count(), 1);
        // Fastest 3 of A: Lap 2, Lap 3, Lap 1 (all flying laps) plus Main (the out lap) which a filter never
        // hides.
        QCOMPARE(f.selection.displayedRanges(f.a->id).size(), std::size_t{4});
        f.selection.setFilter(f.a->id, FilterSpec{FilterSpec::Kind::Fastest, {}});
        QCOMPARE(f.selection.displayedRanges(f.a->id).size(), std::size_t{2});
        f.selection.setFilter(f.a->id, FilterSpec{FilterSpec::Kind::WithinOnePercent, {}});
        QCOMPARE(f.selection.displayedRanges(f.a->id).size(), std::size_t{3}); // Main, Lap 2, Lap 3
        f.selection.applyFilterToAllSources();
        QVERIFY(f.selection.applyToAllSources());
        QCOMPARE(f.selection.filter(f.b->id).kind, FilterSpec::Kind::WithinOnePercent);
        f.selection.setFilter(f.b->id, FilterSpec{});
        QVERIFY(!f.selection.applyToAllSources()); // a per-source change clears the flag
        f.selection.setFilter(f.a->id, FilterSpec{FilterSpec::Kind::Selected, {}});
        QCOMPARE(f.selection.displayedRanges(f.a->id), std::vector<model::RangeRef>{f.lapA(0)});
    }

    void parseLapNumbers() {
        using P = std::vector<std::pair<int, int>>;
        QCOMPARE(*DataSelectionModel::parseLapNumbers("3"), (P{{3, 3}}));
        QCOMPARE(*DataSelectionModel::parseLapNumbers(" 3 - 7 "), (P{{3, 7}}));
        QCOMPARE(*DataSelectionModel::parseLapNumbers("7-3,9"), (P{{3, 7}, {9, 9}}));
        QCOMPARE(*DataSelectionModel::parseLapNumbers("3,5,9"), (P{{3, 3}, {5, 5}, {9, 9}}));
        QVERIFY(!DataSelectionModel::parseLapNumbers(""));
        QVERIFY(!DataSelectionModel::parseLapNumbers("3,"));
        QVERIFY(!DataSelectionModel::parseLapNumbers("a"));
        QVERIFY(!DataSelectionModel::parseLapNumbers("3-"));
    }

    // Invariants over a seeded random sequence of every operation.
    void invariantsHoldUnderRandomOperations() {
        Fixture f;
        f.selection.sourcePublished(f.a);
        f.selection.sourcePublished(f.b);
        std::mt19937 rng(20261011);
        std::vector<model::RangeRef> all;
        for (std::size_t i = 0; i < 5; ++i) {
            all.push_back(f.lapA(i));
        }
        for (std::size_t i = 0; i < 4; ++i) {
            all.push_back(f.lapB(i));
        }
        std::uint64_t version = f.selection.snapshot().version;
        for (int step = 0; step < 2000; ++step) {
            const model::RangeRef& r = all[rng() % all.size()];
            switch (rng() % 10) {
            case 0:
                f.selection.setMain(r);
                break;
            case 1:
                f.selection.toggleOverlay0(r);
                break;
            case 2:
            case 3:
                f.selection.toggleOverlay(r);
                break;
            case 4:
                f.selection.toggleOverlaysHidden();
                break;
            case 5:
                f.selection.clearOverlays();
                QVERIFY(!f.selection.snapshot().overlaysHidden);
                break;
            case 6:
                f.selection.swapMainOverlay0();
                break;
            case 7:
                (rng() % 2 == 0) ? f.selection.nextRange() : f.selection.prevRange();
                break;
            case 8:
                f.selection.selectAllFast();
                break;
            default:
                f.selection.selectFastest();
                break;
            }
            checkInvariants(f.selection);
            QVERIFY(f.selection.hasMain());
            QVERIFY(f.selection.snapshot().version >= version);
            version = f.selection.snapshot().version;
        }
    }
};

QTEST_GUILESS_MAIN(TstDataSelection)
#include "tst_dataselection.moc"
