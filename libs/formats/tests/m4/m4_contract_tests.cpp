// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The final parts of the esses_formats M4 contract (D-206, docs/milestones/M4.md section 2):
// ResidentEstimate's sum and accumulation, the SourceFileState names, and the signatures the budget and
// reload packages implemented (D-207, D-208; no contract stub is left, D-216); their behaviour is tested by
// those packages, tests/m4/budget_*.cpp and tests/m4/reload_*.cpp.

#include <esses/formats/resident_estimate.hpp>
#include <esses/formats/source_file_check.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using namespace esses;
using namespace esses::formats;

TEST_CASE("m4 contract: a resident estimate totals its four byte counts", "[unit][m4]") {
    ResidentEstimate a;
    a.sampleBytes = 262'144;
    a.pyramidBytes = 174'760;
    a.bitmapBytes = 8'192;
    a.overheadBytes = 88;
    a.channels = 1;
    a.chunks = 1;
    CHECK(a.total() == 262'144U + 174'760U + 8'192U + 88U);

    ResidentEstimate b;
    b.sampleBytes = 4;
    b.pyramidBytes = 3;
    b.bitmapBytes = 2;
    b.overheadBytes = 1;
    b.channels = 2;
    b.chunks = 5;
    ResidentEstimate sum = a;
    sum += b;
    CHECK(sum.sampleBytes == 262'148U);
    CHECK(sum.pyramidBytes == 174'763U);
    CHECK(sum.bitmapBytes == 8'194U);
    CHECK(sum.overheadBytes == 89U);
    CHECK(sum.channels == 3U);
    CHECK(sum.chunks == 6U);
    CHECK(sum.total() == a.total() + 10U);
    CHECK(ResidentEstimate{}.total() == 0U);
    CHECK(ResidentEstimate{} == ResidentEstimate{});
    CHECK_FALSE(sum == a);
}

TEST_CASE("m4 contract: the source file states have stable names", "[unit][m4]") {
    CHECK(sourceFileStateName(SourceFileState::Unchanged) == "unchanged");
    CHECK(sourceFileStateName(SourceFileState::Changed) == "changed");
    CHECK(sourceFileStateName(SourceFileState::Missing) == "missing");
    CHECK(SourceFileCheck{}.state == SourceFileState::Unchanged);
}

TEST_CASE("m4 contract: the estimate and check functions keep their signatures", "[unit][m4]") {
    // The packages replace the bodies; the signatures are what SourceLoader and the CLI call.
    static_assert(
        std::is_same_v<decltype(&estimateChannel), ResidentEstimate (*)(store::Dtype, std::uint64_t)>);
    static_assert(
        std::is_same_v<decltype(static_cast<ResidentEstimate (*)(const ProbeResult&)>(&estimateResident)),
                       ResidentEstimate (*)(const ProbeResult&)>);
    static_assert(std::is_same_v<decltype(static_cast<ResidentEstimate (*)(const model::DataSource&)>(
                                     &estimateResident)),
                                 ResidentEstimate (*)(const model::DataSource&)>);
    static_assert(std::is_same_v<decltype(static_cast<SourceFileCheck (*)(const model::SourceProvenance&)>(
                                     &checkSourceFile)),
                                 SourceFileCheck (*)(const model::SourceProvenance&)>);
    // An empty probe and an empty source estimate nothing, whatever the implementation.
    CHECK(estimateResident(ProbeResult{}).total() == 0U);
    CHECK(estimateResident(model::DataSource{}).total() == 0U);
    CHECK(estimateChannel(store::Dtype::F32, 0).total() == 0U);
}
