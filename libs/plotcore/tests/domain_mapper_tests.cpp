// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DomainMapper, DistanceChain and the F9 rule (04 sections 3.1-3.4, D-016, D-134, AC-M1-09): chain continuity
// at lap joins with different stretches, round trips within one 0.25 m step, the stopped-car plateau, partial
// chains, time-domain exactness, and both F9 preservation rules.

#include "plotcore_test_support.hpp"

#include <esses/plotcore/domain_mapper.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <random>

using namespace esses;
using namespace esses::plot;
using Catch::Matchers::WithinAbs;

namespace {

constexpr Micros kS = 1'000'000;

// Three constant-speed laps: [0, 60 s) at 30 m/s, [60, 125 s) at 28 m/s, [125, 185 s) at 31 m/s.
std::vector<LapPiece> threeLaps(double s0 = 1.02, double s1 = 0.99, double s2 = 1.0) {
    std::vector<LapPiece> p(3);
    p[0].start = 0;
    p[0].end = 60 * kS;
    p[0].lookup = test::FakeLapDistance::constant(p[0].start, p[0].end, 30.0);
    p[0].stretch = s0;
    p[1].start = 60 * kS;
    p[1].end = 125 * kS;
    p[1].lookup = test::FakeLapDistance::constant(p[1].start, p[1].end, 28.0);
    p[1].stretch = s1;
    p[2].start = 125 * kS;
    p[2].end = 185 * kS;
    p[2].lookup = test::FakeLapDistance::constant(p[2].start, p[2].end, 31.0);
    p[2].stretch = s2;
    return p;
}

double speedAt(Micros t) {
    if (t < 60 * kS) {
        return 30.0 * 1.02;
    }
    return t < 125 * kS ? 28.0 * 0.99 : 31.0;
}

} // namespace

TEST_CASE("DistanceChain: three laps with different stretches", "[unit]") {
    const DistanceChain chain(threeLaps(), 60 * kS);
    REQUIRE(chain.complete());
    REQUIRE(chain.anyIndexed());

    SECTION("the origin lap equals its index exactly") {
        CHECK(chain.distAt(60 * kS) == 0.0);
        const auto lap = test::FakeLapDistance::constant(60 * kS, 125 * kS, 28.0);
        for (const Micros t : {61 * kS, 70 * kS + 123, 100 * kS, 124 * kS + 999'999}) {
            CHECK(chain.distAt(t) == lap->distAt(t) * 0.99);
        }
        // The next lap starts exactly at the stretched length of the origin lap.
        CHECK(chain.distAt(125 * kS) == 1820.0 * 0.99);
    }
    SECTION("negative before the origin and the extent over the outing") {
        CHECK_THAT(chain.distAt(30 * kS), WithinAbs(-(1800.0 - 900.0) * 1.02, 1e-9));
        const XSpan e = chain.extent();
        CHECK_THAT(e.a, WithinAbs(-1800.0 * 1.02, 1e-9));
        CHECK_THAT(e.b, WithinAbs(1820.0 * 0.99 + 1860.0, 1e-9));
        CHECK_THAT(chain.distAt(0), WithinAbs(e.a, 1e-9));
        CHECK_THAT(chain.distAt(185 * kS), WithinAbs(e.b, 1e-9));
    }
    SECTION("continuity at lap joins") {
        for (const Micros join : {60 * kS, 125 * kS}) {
            const double before = chain.distAt(join - 1);
            const double at = chain.distAt(join);
            CHECK(at >= before);
            CHECK(at - before < 1e-3);
        }
    }
    SECTION("timeAt(distAt(t)) within one 0.25 m step, 1000 random times") {
        std::mt19937_64 rng(test::testSeed());
        std::uniform_int_distribution<Micros> pick(0, 185 * kS - 1);
        for (int i = 0; i < 1000; ++i) {
            const Micros t = pick(rng);
            const Micros back = chain.timeAt(chain.distAt(t));
            const double tolUs = 0.25 / speedAt(t) * 1e6;
            INFO("t = " << t << " back = " << back);
            CHECK(static_cast<double>(std::llabs(back - t)) <= tolUs);
            CHECK_THAT(chain.distAt(back), WithinAbs(chain.distAt(t), 0.25));
        }
    }
    SECTION("timeAt clamps to the outing") {
        CHECK(chain.timeAt(-1e9) == 0);
        CHECK(chain.timeAt(1e9) == 185 * kS);
        CHECK(chain.timeAt(0.0) == 60 * kS);
    }
    SECTION("an origin inside a lap (section and outing ranges)") {
        const DistanceChain mid(threeLaps(), 70 * kS);
        CHECK(mid.distAt(70 * kS) == 0.0);
        CHECK_THAT(mid.distAt(60 * kS), WithinAbs(-280.0 * 0.99, 1e-9));
        CHECK(std::llabs(mid.timeAt(0.0) - 70 * kS) <= 1);
        const DistanceChain outing(threeLaps(), 0);
        CHECK(outing.distAt(0) == 0.0);
        CHECK_THAT(outing.extent().b, WithinAbs(1800.0 * 1.02 + 1820.0 * 0.99 + 1860.0, 1e-9));
    }
}

TEST_CASE("DistanceChain: a stopped-car plateau maps to the first time the distance is reached", "[unit]") {
    // 0..10 s to 100 m, stopped 10..20 s, 20..30 s to 200 m.
    std::vector<LapPiece> pieces(1);
    pieces[0].start = 0;
    pieces[0].end = 30 * kS;
    pieces[0].lookup = std::make_shared<const test::FakeLapDistance>(
        0, std::vector<Micros>{0, 10 * kS, 20 * kS, 30 * kS}, std::vector<double>{0.0, 100.0, 100.0, 200.0});
    const DistanceChain chain(std::move(pieces), 0);
    CHECK(chain.timeAt(100.0) == 10 * kS);
    CHECK(chain.distAt(15 * kS) == 100.0);
    CHECK(chain.timeAt(150.0) == 25 * kS);
}

TEST_CASE("DistanceChain: laps without an index collapse and are reported", "[unit]") {
    std::vector<LapPiece> pieces = threeLaps(1.0, 1.0, 1.0);
    pieces[1].lookup.reset(); // the middle lap has no Corr Dist
    const DistanceChain chain(pieces, 0);
    CHECK_FALSE(chain.complete());
    CHECK(chain.anyIndexed());
    CHECK(chain.indexedAt(10 * kS));
    CHECK_FALSE(chain.indexedAt(70 * kS));
    // The collapsed lap sits at the end of lap 0; lap 2 starts there.
    CHECK(chain.distAt(70 * kS) == 1800.0);
    CHECK(chain.distAt(125 * kS) == 1800.0);
    CHECK(chain.timeAt(1800.0) == 125 * kS);
    CHECK(chain.timeAt(1799.9) < 60 * kS);
    const auto spans = chain.unindexedSpans();
    REQUIRE(spans.size() == 1);
    CHECK(spans[0] == std::pair<Micros, Micros>{60 * kS, 125 * kS});
    CHECK(chain.coversTime(0, 50 * kS));
    CHECK_FALSE(chain.coversTime(50 * kS, 70 * kS));
    CHECK(chain.coversDistance(0.0, 1700.0));
    CHECK_FALSE(chain.coversDistance(1700.0, 1900.0));
    CHECK(chain.extent().b == 1800.0 + 1860.0);

    std::vector<LapPiece> none = threeLaps();
    for (LapPiece& p : none) {
        p.lookup.reset();
    }
    const auto noIndex = std::make_shared<const DistanceChain>(none, 0);
    CHECK_FALSE(noIndex->anyIndexed());
    const DomainMapper map(Domain::Distance, RangeGeom{0, 60 * kS}, OutingGeom{0, 185 * kS}, noIndex,
                           Rate{20, 1}, 0);
    CHECK_FALSE(map.hasDistance());
    const DomainMapper nullChain(Domain::Distance, RangeGeom{0, 60 * kS}, OutingGeom{0, 185 * kS}, nullptr,
                                 Rate{20, 1}, 0);
    CHECK_FALSE(nullChain.hasDistance());
    CHECK(nullChain.outingExtent() == XSpan{});
}

TEST_CASE("DomainMapper: time domain is exact integer arithmetic", "[unit]") {
    const RangeGeom r{10 * kS + 7, 70 * kS + 7, 333, 0.0, 1.0};
    const DomainMapper map(Domain::Time, r, OutingGeom{0, 200 * kS}, nullptr, Rate{512, 25}, 1234);
    std::mt19937_64 rng(test::testSeed() + 1);
    std::uniform_int_distribution<Micros> pick(-20 * kS, 150 * kS);
    for (int i = 0; i < 1000; ++i) {
        const auto x = static_cast<double>(pick(rng));
        CHECK(map.xOf(map.timeAt(x)) == x);
    }
    CHECK(map.xOf(r.start) == 333.0);
    CHECK(map.timeAt(0.0) == r.start - 333);
    CHECK(map.extent() == 60.0 * 1e6);
    CHECK(map.outingExtent() ==
          XSpan{-static_cast<double>(r.start), static_cast<double>(200 * kS - r.start)});
    // Integral sample indices use D-129's exact sample times.
    for (const SampleIdx i : {SampleIdx{0}, SampleIdx{1}, SampleIdx{77}, SampleIdx{1023}}) {
        CHECK(map.xOfSample(static_cast<double>(i)) == map.xOf(Rate{512, 25}.timeAt(i, 1234)));
        CHECK_THAT(map.sampleAt(map.xOfSample(static_cast<double>(i))),
                   WithinAbs(static_cast<double>(i), 1e-4));
    }
    const XSpan v{1000.0, 5000.0};
    CHECK(DomainMapper::pxOf(3000.0, v, 10.0, 200.0) == 110.0);
    CHECK(DomainMapper::xOfPx(110.0, v, 10.0, 200.0) == 3000.0);
}

TEST_CASE("DomainMapper: distance domain through the chain with the alignment offset", "[unit]") {
    const auto chain = std::make_shared<const DistanceChain>(threeLaps(), 60 * kS);
    const RangeGeom r{60 * kS, 125 * kS, 0, 12.5, 0.99};
    const DomainMapper map(Domain::Distance, r, OutingGeom{0, 185 * kS}, chain, Rate{20, 1}, 0);
    REQUIRE(map.hasDistance());
    CHECK(map.xOf(60 * kS) == 12.5);
    CHECK(map.timeAt(12.5) == 60 * kS);
    CHECK(map.extent() == 1820.0 * 0.99);
    CHECK(map.outingExtent() == chain->extent());
    std::mt19937_64 rng(test::testSeed() + 2);
    std::uniform_int_distribution<Micros> pick(0, 185 * kS - 1);
    for (int i = 0; i < 1000; ++i) {
        const Micros t = pick(rng);
        const Micros back = map.timeAt(map.xOf(t));
        CHECK(static_cast<double>(std::llabs(back - t)) <= 0.25 / speedAt(t) * 1e6);
    }
}

TEST_CASE("DomainMapper: an analysis::DistanceIndex piece behaves like its table", "[unit]") {
    // Integration with the real DistanceIndex once esses_analysis implements it (the M1 contract); while it
    // is still a stub this records the fact and checks nothing else.
    const std::vector<Micros> times{0, 10 * kS, 20 * kS, 30 * kS};
    const std::vector<double> dist{0.0, 300.0, 300.0, 600.0};
    analysis::DistanceIndexKey key;
    key.start = 0;
    key.end = 30 * kS;
    Result<analysis::DistanceIndex> idx = analysis::DistanceIndex::fromSamples(times, dist, key);
    if (!idx) {
        CHECK(idx.error().code == ErrorCode::Unsupported);
        WARN("analysis::DistanceIndex::fromSamples is not implemented yet: " << idx.error().message);
        return;
    }
    std::vector<LapPiece> pieces(1);
    pieces[0].start = 0;
    pieces[0].end = 30 * kS;
    pieces[0].idx = std::make_shared<const analysis::DistanceIndex>(std::move(*idx));
    const DistanceChain chain(std::move(pieces), 0);
    CHECK(chain.complete());
    CHECK_THAT(chain.extent().b, WithinAbs(600.0, 0.25));
    CHECK(std::llabs(chain.timeAt(300.0) - 10 * kS) <= kS / 100); // the plateau: first time reached
    CHECK_THAT(chain.distAt(25 * kS), WithinAbs(450.0, 0.25));
}

TEST_CASE("F9: through the Main chain and back within one sample (AC-M1-09)", "[unit]") {
    const auto chain = std::make_shared<const DistanceChain>(threeLaps(), 60 * kS);
    const RangeGeom main{60 * kS, 125 * kS};
    const OutingGeom outing{0, 185 * kS};
    const XSpan timeZoom{5.0 * 1e6, 40.0 * 1e6};
    const double ext = mainExtentIn(Domain::Time, main, chain.get());
    const ZoomConversion toDist = convertZoom(timeZoom, Domain::Time, Domain::Distance, timeZoom.a / ext,
                                              timeZoom.b / ext, main, outing, chain.get());
    REQUIRE(toDist.throughChain);
    CHECK(toDist.zoom.a == chain->distAt(main.start + 5 * kS));
    CHECK(toDist.zoom.b == chain->distAt(main.start + 40 * kS));
    const double extD = mainExtentIn(Domain::Distance, main, chain.get());
    CHECK(extD == 1820.0 * 0.99);
    const ZoomConversion back = convertZoom(toDist.zoom, Domain::Distance, Domain::Time, toDist.zoom.a / extD,
                                            toDist.zoom.b / extD, main, outing, chain.get());
    REQUIRE(back.throughChain);
    const double samplePeriodUs = 1e6 / 20.0; // a 20 Hz Main channel
    CHECK(std::abs(back.zoom.a - timeZoom.a) <= samplePeriodUs);
    CHECK(std::abs(back.zoom.b - timeZoom.b) <= samplePeriodUs);

    // A span reaching into the previous lap converts through the chain too (F2, panning).
    const XSpan wide{-30.0 * 1e6, 70.0 * 1e6};
    const ZoomConversion wideD = convertZoom(wide, Domain::Time, Domain::Distance, wide.a / ext, wide.b / ext,
                                             main, outing, chain.get());
    CHECK(wideD.throughChain);
    CHECK(wideD.zoom.a < 0.0);
    CHECK(wideD.zoom.b > extD);
}

TEST_CASE("F9: the fraction rule without Corr Dist (AC-M1-09)", "[unit]") {
    const RangeGeom main{60 * kS, 125 * kS};
    const OutingGeom outing{0, 185 * kS};
    const double lenT = mainExtentIn(Domain::Time, main, nullptr);
    CHECK(lenT == 65.0 * 1e6);
    for (const double trackLength : {1.0, 3500.0}) {
        const double lenD = mainExtentIn(Domain::Distance, main, nullptr, trackLength);
        CHECK(lenD == trackLength);
        const XSpan a{5.0 * 1e6, 40.0 * 1e6};
        const double fa = a.a / lenT;
        const double fb = a.b / lenT;
        const ZoomConversion d =
            convertZoom(a, Domain::Time, Domain::Distance, fa, fb, main, outing, nullptr, trackLength);
        CHECK_FALSE(d.throughChain);
        // a' = R.start' + (a - R.start) / len(R) * len(R'), with both starts at 0 in view coordinates.
        CHECK(d.zoom.a == fa * lenD);
        CHECK(d.zoom.b == fb * lenD);
        const ZoomConversion t =
            convertZoom(d.zoom, Domain::Distance, Domain::Time, fa, fb, main, outing, nullptr, trackLength);
        CHECK_FALSE(t.throughChain);
        CHECK_THAT(t.zoom.a, WithinAbs(a.a, 1e-6));
        CHECK_THAT(t.zoom.b, WithinAbs(a.b, 1e-6));
    }
    // The outing clamp: a span past the outing keeps only its inside part.
    const ZoomConversion clamped =
        convertZoom(XSpan{0.0, 1.0}, Domain::Distance, Domain::Time, -2.0, 0.5, main, outing, nullptr);
    CHECK(clamped.zoom.a == -60.0 * 1e6);
    CHECK(clamped.zoom.b == 0.5 * 65.0 * 1e6);
    const XSpan scaledOuting = outingExtentIn(Domain::Distance, main, outing, nullptr, 650.0);
    CHECK_THAT(scaledOuting.a, WithinAbs(-600.0, 1e-9));
    CHECK_THAT(scaledOuting.b, WithinAbs(1250.0, 1e-9));
    // from == to is the identity.
    const ZoomConversion same =
        convertZoom(XSpan{1.0, 2.0}, Domain::Time, Domain::Time, 0.0, 1.0, main, outing, nullptr);
    CHECK(same.zoom == XSpan{1.0, 2.0});
}

TEST_CASE("F9: a span crossing a lap without an index uses the fraction rule", "[unit]") {
    std::vector<LapPiece> pieces = threeLaps(1.0, 1.0, 1.0);
    pieces[0].lookup.reset();
    const auto chain = std::make_shared<const DistanceChain>(pieces, 60 * kS);
    const RangeGeom main{60 * kS, 125 * kS};
    const OutingGeom outing{0, 185 * kS};
    const XSpan span{-10.0 * 1e6, 20.0 * 1e6}; // starts in the unindexed lap
    const ZoomConversion d = convertZoom(span, Domain::Time, Domain::Distance, span.a / 65e6, span.b / 65e6,
                                         main, outing, chain.get());
    CHECK_FALSE(d.throughChain);
    const XSpan inside{1.0 * 1e6, 20.0 * 1e6};
    const ZoomConversion ok = convertZoom(inside, Domain::Time, Domain::Distance, inside.a / 65e6,
                                          inside.b / 65e6, main, outing, chain.get());
    CHECK(ok.throughChain);
}

// M1 integration (AC-M1-01 on the ACC stand-in): F9 while the Main lap's index is still pending and another
// lap of the outing already has one. The fallback span is in the fraction rule's normalised units, so the
// clamp must be too; the partial chain's metres used to clamp [fa, fb] to the indexed lap's extent before the
// origin (a span of [-4868 m, 0] on that file).
TEST_CASE("F9: a Main lap whose index is pending uses the fraction rule in normalised units", "[unit]") {
    std::vector<LapPiece> pieces = threeLaps(1.0, 1.0, 1.0);
    pieces[1].lookup.reset(); // Main = the middle lap, not indexed yet; the out lap is
    const auto chain = std::make_shared<const DistanceChain>(pieces, 60 * kS);
    REQUIRE(chain->anyIndexed());
    const RangeGeom main{60 * kS, 125 * kS};
    const OutingGeom outing{0, 185 * kS};
    CHECK(mainExtentIn(Domain::Distance, main, chain.get()) == 1.0);
    const XSpan limit = outingExtentIn(Domain::Distance, main, outing, chain.get());
    CHECK_THAT(limit.a, WithinAbs(-60.0 / 65.0, 1e-12));
    CHECK_THAT(limit.b, WithinAbs(125.0 / 65.0, 1e-12));
    const XSpan span{0.0, 30.0 * 1e6};
    const ZoomConversion d =
        convertZoom(span, Domain::Time, Domain::Distance, 0.0, 30.0 / 65.0, main, outing, chain.get());
    CHECK_FALSE(d.throughChain);
    CHECK(d.zoom.a == 0.0);
    CHECK_THAT(d.zoom.b, WithinAbs(30.0 / 65.0, 1e-12));
    // Once the Main lap is indexed the same chain geometry yields metres again.
    const auto full = std::make_shared<const DistanceChain>(threeLaps(1.0, 1.0, 1.0), 60 * kS);
    const XSpan metres = outingExtentIn(Domain::Distance, main, outing, full.get());
    CHECK_THAT(metres.a, WithinAbs(-1800.0, 1e-6));
    CHECK_THAT(metres.b, WithinAbs(1820.0 + 1860.0, 1e-6));
}
