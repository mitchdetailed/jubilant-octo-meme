// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The fastest-lap candidate rule (06 section 5.2, D-135, release note TT 2485; AC-M1-07): trusted flying laps
// if any, else flying or partial laps; out, in and pit laps never.
#include "analysis_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace esses;
using namespace esses::analysis;
using model::LapClass;
using model::Range;

namespace {

Range lap(LapClass cls, int number, Micros durationS, bool trusted, bool enabled = true) {
    static Micros cursor = 0;
    Range r;
    r.kind = model::RangeKind::Lap;
    r.lapClass = cls;
    r.number = number;
    r.start = cursor;
    r.end = cursor + durationS * 1'000'000;
    cursor = r.end;
    r.trusted = trusted;
    r.enabled = enabled;
    return r;
}

std::vector<int> numbers(const std::vector<const Range*>& laps) {
    std::vector<int> out;
    out.reserve(laps.size());
    for (const Range* r : laps) {
        out.push_back(r->number);
    }
    return out;
}

} // namespace

TEST_CASE("fastest lap: trusted flying laps when any lap is trusted", "[unit][AC-M1-07]") {
    const std::vector<Range> laps = {
        lap(LapClass::Out, 0, 50, false),    lap(LapClass::Flying, 1, 92, true),
        lap(LapClass::Flying, 2, 88, false), lap(LapClass::Partial, 3, 40, false),
        lap(LapClass::Flying, 3, 90, true),  lap(LapClass::Flying, 4, 89, true, false),
        lap(LapClass::Pit, 5, 30, true),     lap(LapClass::In, 6, 20, false)};
    CHECK(numbers(fastestLapCandidates(laps)) ==
          std::vector<int>{1, 3}); // untrusted L2, disabled L4 excluded
    REQUIRE(fastestLap(laps) != nullptr);
    CHECK(fastestLap(laps)->number == 3);
}

TEST_CASE("fastest lap: without trusted flying laps, flying or partial laps of any trust (TT 2485)",
          "[unit][AC-M1-07]") {
    const std::vector<Range> laps = {lap(LapClass::Out, 0, 10, false), lap(LapClass::Flying, 1, 92, false),
                                     lap(LapClass::Partial, 2, 41, false),
                                     lap(LapClass::Flying, 2, 90, false), lap(LapClass::In, 3, 5, false)};
    CHECK(numbers(fastestLapCandidates(laps)) == std::vector<int>{1, 2, 2});
    REQUIRE(fastestLap(laps) != nullptr);
    CHECK(fastestLap(laps)->lapClass == LapClass::Partial); // the in and out laps are never fastest
    // A trusted out lap alone does not make untrusted flying laps ineligible.
    std::vector<Range> trustedOut = laps;
    trustedOut[0].trusted = true;
    CHECK(numbers(fastestLapCandidates(trustedOut)) == std::vector<int>{1, 2, 2});
}

TEST_CASE("fastest lap: ties go to the lowest number; no candidate gives nullptr", "[unit][AC-M1-07]") {
    const std::vector<Range> tie = {lap(LapClass::Flying, 4, 90, true), lap(LapClass::Flying, 2, 90, true),
                                    lap(LapClass::Flying, 3, 91, true)};
    REQUIRE(fastestLap(tie) != nullptr);
    CHECK(fastestLap(tie)->number == 2);
    const std::vector<Range> onlyInOut = {lap(LapClass::Out, 0, 10, false), lap(LapClass::In, 1, 10, false)};
    CHECK(fastestLap(onlyInOut) == nullptr);
    CHECK(fastestLapCandidates(onlyInOut).empty());
    CHECK(fastLapsWithin(onlyInOut, 1.0).empty());
    CHECK(fastestLap({}) == nullptr);
}

TEST_CASE("fastest lap: laps within a tolerance of the fastest", "[unit][AC-M1-07]") {
    const std::vector<Range> laps = {lap(LapClass::Flying, 1, 100, true), lap(LapClass::Flying, 2, 101, true),
                                     lap(LapClass::Flying, 3, 102, true), lap(LapClass::Flying, 4, 100, true),
                                     lap(LapClass::Flying, 5, 99, false)};
    CHECK(numbers(fastLapsWithin(laps, 1.0)) == std::vector<int>{1, 2, 4}); // <= 101 s, in lap order
    CHECK(numbers(fastLapsWithin(laps, 0.0)) == std::vector<int>{1, 4});
    CHECK(numbers(fastLapsWithin(laps, -5.0)) == std::vector<int>{1, 4});
    CHECK(numbers(fastLapsWithin(laps, 2.0)) == std::vector<int>{1, 2, 3, 4});
}
