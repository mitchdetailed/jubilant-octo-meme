// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportSpec and TimeReportSpec JSON (04 sections 12 item 8 and 13 item 7) against the shipped schema
// examples, and StatCache (01 section 7.1): LRU order, the budget, updates and concurrent use.
#include "report_test_support.hpp"

#include <esses/analysis/time_report_engine.hpp>
#include <esses/base/json_nlohmann.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace esses;
using namespace esses::analysis;
using namespace esses::analysis::test;

namespace {

JsonDoc doc(std::string_view text) {
    Result<JsonDoc> d = JsonDoc::parse(text);
    REQUIRE(d.ok());
    return std::move(d).value();
}

// A schema example body (schemas/examples/components/<type>-1/valid/<name>.json).
JsonDoc exampleBody(std::string_view type, std::string_view name) {
    Result<JsonDoc> d = JsonDoc::load(repoRoot() / "schemas" / "examples" / "components" /
                                      (std::string(type) + "-1") / "valid" / (std::string(name) + ".json"));
    REQUIRE(d.ok());
    return std::move(d).value();
}

JsonDoc member(const JsonDoc& object, const char* key) {
    const nlohmann::json j = jsonio::fromDoc(object);
    Result<JsonDoc> d = jsonio::toDoc(j.at(key));
    REQUIRE(d.ok());
    return std::move(d).value();
}

std::string detailOf(const Error& e) {
    return e.context.detail.value_or("");
}

StatKey keyOf(int n) {
    StatKey k;
    k.channel.v.b[0] = static_cast<std::uint8_t>(n);
    k.range.v.b[1] = static_cast<std::uint8_t>(n >> 8);
    k.version = 7;
    return k;
}

} // namespace

TEST_CASE("report_spec: the channel_report example body reads, writes and round-trips", "[unit]") {
    const Result<ReportSpec> spec =
        ReportSpec::fromJson(member(exampleBody("channel_report", "full"), "report"));
    REQUIRE(spec.ok());
    CHECK(spec->scope == ReportScope::SelectedRanges);
    CHECK(spec->headers == std::vector<std::string>{"Outings:Laps"});
    REQUIRE(spec->channels.size() == 1);
    CHECK(spec->channels[0] == ReportChannel{"Corr Speed", {ReportStat::Max, ReportStat::Avg}});
    CHECK(spec->options == ReportOptions{});
    const Result<ReportSpec> again = ReportSpec::fromJson(spec->toJson());
    REQUIRE(again.ok());
    CHECK(*again == *spec);

    ReportSpec custom;
    custom.scope = ReportScope::AllOutings;
    custom.headers = {"Driver", "Outings:Laps:Track Sections:Default:Corners", "Outings:Laps"};
    custom.channels = {
        ReportChannel{"Brake Pres", {ReportStat::AbsMax, ReportStat::Delta, ReportStat::StdDev}},
        ReportChannel{"Gear", {}}};
    custom.options = ReportOptions{true, true, true, 2.5, true};
    const Result<ReportSpec> back = ReportSpec::fromJson(custom.toJson());
    REQUIRE(back.ok());
    CHECK(*back == custom);
    // The defaults: an empty object, and the absent value.
    CHECK(*ReportSpec::fromJson(doc("{}")) == ReportSpec{});
    CHECK(*ReportSpec::fromJson(JsonDoc{}) == ReportSpec{});
}

TEST_CASE("report_spec: a wrong member is Invalid with its pointer", "[unit]") {
    const auto failure = [](std::string_view text) {
        const Result<ReportSpec> r = ReportSpec::fromJson(doc(text));
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        return detailOf(r.error());
    };
    CHECK(failure(R"({"channels": [{"name": "Speed", "stats": ["max", "median"]}]})") ==
          "/channels/0/stats/1");
    CHECK(failure(R"({"scope": "everything"})") == "/scope");
    CHECK(failure(R"({"headers": "Outings:Laps"})") == "/headers");
    CHECK(failure(R"({"headers": ["Outings", 3]})") == "/headers/1");
    CHECK(failure(R"({"options": {"probeSeconds": "5"}})") == "/options/probeSeconds");
    CHECK(failure(R"({"options": {"probeSeconds": -1}})") == "/options/probeSeconds");
    CHECK(failure(R"({"channels": [{"name": 7}]})") == "/channels/0/name");
    CHECK(failure(R"([1, 2])").empty());
}

TEST_CASE("report_spec: the time_report example body and its errors", "[unit]") {
    const Result<TimeReportSpec> spec = TimeReportSpec::fromJson(exampleBody("time_report", "full"));
    REQUIRE(spec.ok());
    CHECK(spec->scope == ReportScope::SelectedOutings);
    CHECK(spec->rowGroup == "Outings:Laps:Track Sections:Default");
    CHECK_FALSE(spec->includeUntrusted);
    CHECK(spec->summaries == TimeReportSummaries{});
    REQUIRE(spec->bands.size() == 2);
    CHECK(spec->bands[0] == TimeReportBand{1.0, 0x30C030FF, true});
    CHECK(spec->bands[1] == TimeReportBand{3.0, 0xC0C030FF, true});
    CHECK(*TimeReportSpec::fromJson(exampleBody("time_report", "empty")) == TimeReportSpec{});

    const Result<TimeReportSpec> other = TimeReportSpec::fromJson(
        doc(R"({"scope": "selectedRanges", "rowGroup": "Outings:Laps:Track Sections:Fast", "options":
            {"includeUntrusted": true}, "summaries": {"eclectic": false, "rollingMin": false, "total": false},
            "bands": [{"pct": 0.5, "colour": "#11223380", "visible": false}, {"pct": 2}]})"));
    REQUIRE(other.ok());
    CHECK(other->scope == ReportScope::SelectedRanges);
    CHECK(other->includeUntrusted);
    CHECK(other->summaries == TimeReportSummaries{false, false, false});
    CHECK(other->bands[0] == TimeReportBand{0.5, 0x11223380, false});
    CHECK(other->bands[1] == TimeReportBand{2.0, TimeReportBand{}.colour, true});

    const auto failure = [](std::string_view text) {
        const Result<TimeReportSpec> r = TimeReportSpec::fromJson(doc(text));
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        return detailOf(r.error());
    };
    CHECK(failure(R"({"scope": "allOutings"})") == "/scope");
    CHECK(failure(R"({"rowGroup": ""})") == "/rowGroup");
    CHECK(failure(R"({"bands": [{"pct": "1"}]})") == "/bands/0/pct");
    CHECK(failure(R"({"bands": [{"pct": 1, "colour": "green"}]})") == "/bands/0/colour");
    CHECK(failure(R"({"summaries": {"total": 1}})") == "/summaries/total");
}

TEST_CASE("report_cache: least recently used cells leave first", "[unit]") {
    StatCache cache(3 * StatCache::kBytesPerCell);
    CHECK(cache.budgetBytes() == 3 * StatCache::kBytesPerCell);
    for (int i = 1; i <= 3; ++i) {
        cache.insert(keyOf(i), StatValue{static_cast<double>(i), i, true});
    }
    CHECK(cache.size() == 3);
    CHECK(cache.bytes() > 0);
    CHECK(cache.bytes() <= cache.budgetBytes());
    REQUIRE(cache.find(keyOf(1)).has_value()); // 1 is now the most recent: 2 is the least recent
    cache.insert(keyOf(4), StatValue{4.0, 4, true});
    CHECK(cache.size() == 3);
    CHECK_FALSE(cache.find(keyOf(2)).has_value());
    CHECK(cache.find(keyOf(1)) == StatValue{1.0, 1, true});
    CHECK(cache.find(keyOf(3)) == StatValue{3.0, 3, true});
    CHECK(cache.find(keyOf(4)) == StatValue{4.0, 4, true});
    // An insert of a held key replaces its value without growing.
    cache.insert(keyOf(3), StatValue{30.0, 3, false});
    CHECK(cache.find(keyOf(3)) == StatValue{30.0, 3, false});
    CHECK(cache.size() == 3);
    // Every field is part of the key.
    StatKey k = keyOf(1);
    k.stat = ReportStat::Max;
    CHECK_FALSE(cache.find(k).has_value());
    k = keyOf(1);
    k.clipEnd = 5;
    CHECK_FALSE(cache.find(k).has_value());
    k = keyOf(1);
    k.optionsHash = 9;
    CHECK_FALSE(cache.find(k).has_value());
    k = keyOf(1);
    k.version = 8;
    CHECK_FALSE(cache.find(k).has_value());
    cache.clear();
    CHECK(cache.size() == 0);
    CHECK_FALSE(cache.find(keyOf(1)).has_value());
    // A budget below one cell caches nothing.
    StatCache tiny(StatCache::kBytesPerCell - 1);
    tiny.insert(keyOf(1), StatValue{1.0, 0, true});
    CHECK(tiny.size() == 0);
}

TEST_CASE("report_cache: concurrent lookups and inserts keep the budget", "[unit]") {
    StatCache cache(500 * StatCache::kBytesPerCell);
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&cache, &wrong, t] {
            for (int i = 0; i < 2000; ++i) {
                const int n = (i * 7 + t * 13) % 1000;
                cache.insert(keyOf(n), StatValue{static_cast<double>(n), n, true});
                if (const std::optional<StatValue> v = cache.find(keyOf(n)); v && v->value != n) {
                    ++wrong;
                }
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    CHECK(wrong.load() == 0);
    CHECK(cache.size() == 500);
    CHECK(cache.bytes() <= cache.budgetBytes());
}

TEST_CASE("report_cache: the memory the cache holds stays within its budget when full", "[unit]") {
    // 01 section 7.1: the budget is a cap on what the cache allocates, not an accounting unit. Full, with
    // churn (every key evicted and inserted again), the slots and index stay within it and every key that
    // is held is found.
    for (const std::size_t cells : {std::size_t{1}, std::size_t{7}, std::size_t{1000}, std::size_t{4096}}) {
        INFO(cells << " cells");
        StatCache cache(cells * StatCache::kBytesPerCell);
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < static_cast<int>(2 * cells); ++i) {
                cache.insert(keyOf(i), StatValue{static_cast<double>(i), i, true});
                CHECK(cache.bytes() <= cache.budgetBytes());
            }
        }
        CHECK(cache.size() == cells);
        // The last `cells` keys inserted are the ones held.
        for (int i = static_cast<int>(cells); i < static_cast<int>(2 * cells); ++i) {
            CHECK(cache.find(keyOf(i)) == StatValue{static_cast<double>(i), i, true});
        }
        CHECK_FALSE(cache.find(keyOf(static_cast<int>(cells) - 1)).has_value());
        CHECK(cache.bytes() > cells * StatCache::kBytesPerCell / 2); // the real footprint, not a count
    }
}
