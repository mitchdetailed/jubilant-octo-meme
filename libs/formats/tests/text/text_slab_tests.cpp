// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Pass 2 over several 65,536-row slabs (05 section 7.1): one sweep decodes every column of a slab and seals
// the siblings into the ChunkCache (insertGroup) or hands them to the threads already waiting for them;
// chunks decode alone, in any order and in parallel with identical results; hold columns carry their value
// across slab boundaries; sparse columns keep their own chunk index.
#include "text_test_support.hpp"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <thread>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::texttest;

namespace {

constexpr std::uint64_t kRows = 2 * 65536 + 1234;

// Uniform 100 Hz; a = row, b = row * 0.5, c every 1,000th row only (hold); d text with three labels.
std::string bigHoldFile(std::uint64_t rows = kRows) {
    std::string s = "Time,A,B,C,D\n";
    s.reserve(rows * 32);
    for (std::uint64_t r = 0; r < rows; ++r) {
        s += std::to_string(r / 100) + "." + (r % 100 < 10 ? "0" : "") + std::to_string(r % 100) + ",";
        s += std::to_string(r) + ",";
        s += std::to_string(r / 2) + (r % 2 != 0 ? ".5" : "") + ",";
        if (r % 1000 == 0) {
            s += std::to_string(r / 1000);
        }
        s += ",";
        s += (r % 3 == 0 ? "lo" : r % 3 == 1 ? "mid" : "hi");
        s += "\n";
    }
    return s;
}

TextProfile holdProfile() {
    std::vector<std::string> log;
    auto p = TextProfile::parse(
        R"({"$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "hold-test", "name": "H",
            "version": "1", "delimiter": ",", "decimal": ".", "sparse": "hold",
            "sniff": {"extensions": [".hold"], "confidence": 45}})",
        log);
    REQUIRE(p.ok());
    return std::move(p).value();
}

} // namespace

TEST_CASE("text slabs: one sweep seals every sibling column; hold values cross slab boundaries",
          "[unit][slow]") {
    Env env(false);
    REQUIRE(env.registry->addProfile(holdProfile()).ok());
    const std::string text = bigHoldFile();
    auto o = env.open(text, "big.hold");
    REQUIRE(o.ok());
    const ProbeResult& p = o->probe;
    REQUIRE(p.layers[0].channels.size() == 4);
    CHECK(std::get<Rate>(p.layers[0].channels[0].timing) == Rate{100, 1});
    CHECK(p.layers[0].channels[0].sampleCount == kRows);
    const auto& a = std::get<store::UniformSeries>(channel(*o, "A").series).values;
    const auto& b = std::get<store::UniformSeries>(channel(*o, "B").series).values;
    const auto& c = std::get<store::UniformSeries>(channel(*o, "C").series).values;
    REQUIRE(a.chunkCount() == 3);
    CHECK_FALSE(b.isResident(2));
    // Decoding A's chunk 2 seals B's, C's and D's chunk 2 in the same sweep.
    auto h = a.require(2 * 65536, 2 * 65536 + 10, Priority::Background, {});
    REQUIRE(h.ok());
    CHECK(b.isResident(2));
    CHECK(c.isResident(2));
    CHECK_FALSE(b.isResident(1));
    CHECK(h->at(2 * 65536 + 5) == 2.0 * 65536 + 5);

    const Dump da = dumpColumn(a);
    const Dump db = dumpColumn(b);
    const Dump dc = dumpColumn(c);
    const Dump dd = dumpSeries(channel(*o, "D").series);
    REQUIRE(da.values.size() == kRows);
    for (std::uint64_t r = 0; r < kRows; r += 977) {
        CHECK(da.values[r] == static_cast<double>(r));
        CHECK(db.values[r] == static_cast<double>(r) * 0.5);
        // C is logged every 1,000th row and held in between, across the slab boundaries at 65,536 rows.
        CHECK(dc.values[r] == static_cast<double>(r / 1000));
        CHECK(dd.values[r] == static_cast<double>(r % 3)); // lo, mid, hi in first-seen order
    }
    CHECK(dc.values[65536] == 65.0);
    CHECK(dc.values[2 * 65536 - 1] == 131.0);
    CHECK(dc.valid[kRows - 1]);
}

TEST_CASE("text slabs: parallel decodes of every column give the sequential result", "[unit][slow]") {
    Env env(false);
    REQUIRE(env.registry->addProfile(holdProfile()).ok());
    const std::string text = bigHoldFile();
    // Reference: a fresh open decoded column by column on this thread.
    std::vector<Dump> reference;
    {
        auto o = env.open(text, "big.hold");
        REQUIRE(o.ok());
        for (const model::Channel& ch : o->source->layers[0].channels) {
            reference.push_back(dumpSeries(ch.series));
        }
    }
    env.cache.clear();
    auto o = env.open(text, "big.hold");
    REQUIRE(o.ok());
    // Every chunk of every column requested at once from several threads (Rule W: a thread waits only for
    // a sweep another running thread is doing).
    std::vector<std::future<bool>> jobs;
    const auto& channels = o->source->layers[0].channels;
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const store::Column col = store::seriesValues(channels[c].series);
        for (store::SampleIdx k = col.chunkCount() - 1; k >= 0; --k) {
            jobs.push_back(std::async(std::launch::async, [col, k] {
                const store::SampleIdx first = store::chunkFirstSample(k);
                const store::SampleIdx last = std::min(col.size(), first + 65536) - 1;
                return col.require(first, last, Priority::Background, {}).ok();
            }));
        }
    }
    for (auto& j : jobs) {
        CHECK(j.get());
    }
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const Dump d = dumpSeries(channels[c].series);
        CHECK(d.values.size() == reference[c].values.size());
        CHECK(d.valid == reference[c].valid);
        bool same = true;
        for (std::size_t i = 0; i < d.values.size() && same; ++i) {
            same = d.values[i] == reference[c].values[i] ||
                   (std::isnan(d.values[i]) && std::isnan(reference[c].values[i]));
        }
        CHECK(same);
    }
}

TEST_CASE("text slabs: a sparse column across chunks keeps its own index and times", "[unit][slow]") {
    // Generic profile (sparse "timestamped"): S is logged on every third row only.
    std::string s = "Time,V,S\n";
    const std::uint64_t rows = 65536 * 3 + 500;
    for (std::uint64_t r = 0; r < rows; ++r) {
        s += std::to_string(r) + "," + std::to_string(r % 1000) + ",";
        if (r % 3 == 0) {
            s += std::to_string(r);
        }
        s += "\n";
    }
    Env env(false);
    ImportOptions opt;
    opt.importerId = std::string(kGenericCsvImporterId);
    auto o = env.open(s, "sparse.csv", opt);
    REQUIRE(o.ok());
    const ChannelInfo& si = o->probe.layers[0].channels[1];
    CHECK(std::holds_alternative<Timestamped>(si.timing));
    CHECK(si.sampleCount == (rows + 2) / 3);
    const auto& series = std::get<store::TimestampedSeries>(channel(*o, "S").series);
    CHECK(series.values.chunkCount() == 2);
    // Decode the last chunk first, then the rest: the same values and times as row order gives.
    REQUIRE(
        series.values.require(series.values.size() - 1, series.values.size() - 1, Priority::Background, {})
            .ok());
    const Dump d = dumpSeries(channel(*o, "S").series);
    REQUIRE(d.values.size() == si.sampleCount);
    for (std::size_t i = 0; i < d.values.size(); i += 4099) {
        CHECK(d.values[i] == static_cast<double>(i * 3));
        CHECK(d.times[i] == static_cast<Micros>(i * 3) * 1'000); // integer times above 10^4: ms (@auto)
    }
    CHECK(d.times.back() == static_cast<Micros>((si.sampleCount - 1) * 3) * 1'000);
    // V stays uniform (1 kHz, inferred).
    CHECK(detailOf(o->probe.warnings, "text.auto_time_unit") == "ms");
    CHECK(std::get<Rate>(o->probe.layers[0].channels[0].timing) == Rate{1000, 1});
}

namespace {

// Generic profile (sparse "timestamped"): Time, V on every row, C on every row but the rows in `shortRows`
// (the row ends before C: the cell is missing, a ragged row) and those in `emptyC` (present and empty).
std::string raggedSparseFile(std::uint64_t rows, const std::vector<std::uint64_t>& shortRows,
                             const std::vector<std::uint64_t>& emptyC) {
    std::string s = "Time,V,C\n";
    s.reserve(rows * 24);
    for (std::uint64_t r = 0; r < rows; ++r) {
        s += std::to_string(r) + "," + std::to_string(r % 1000);
        if (std::find(shortRows.begin(), shortRows.end(), r) == shortRows.end()) {
            s += ",";
            if (std::find(emptyC.begin(), emptyC.end(), r) == emptyC.end()) {
                s += std::to_string(r * 2);
            }
        }
        s += "\n";
    }
    return s;
}

// Pass 1's sample count of the sparse column C equals what pass 2 decodes; its times are strictly increasing;
// the samples at `invalidAt` (the ragged rows: a missing cell is an invalid sample, 05 section 7.1 pass 2)
// are the only invalid ones; no chunk reports text.source_changed (the file did not change).
void checkSparseAgreement(const std::string& text, std::uint64_t expected,
                          const std::vector<std::size_t>& invalidAt) {
    Env env(false);
    ImportOptions opt;
    opt.importerId = std::string(kGenericCsvImporterId);
    auto o = env.open(text, "ragged.csv", opt);
    REQUIRE(o.ok());
    const ChannelInfo& ci = o->probe.layers[0].channels[1];
    REQUIRE(ci.loggedName == "C");
    REQUIRE(std::holds_alternative<Timestamped>(ci.timing));
    CHECK(ci.sampleCount == expected);
    const Dump d = dumpSeries(channel(*o, "C").series);
    REQUIRE(d.values.size() == expected);
    REQUIRE(d.times.size() == expected);
    bool increasing = true;
    for (std::size_t i = 1; i < d.times.size() && increasing; ++i) {
        increasing = d.times[i] > d.times[i - 1];
        if (!increasing) {
            UNSCOPED_INFO("sample " << i << ": t=" << d.times[i] << " <= prev " << d.times[i - 1]);
        }
    }
    CHECK(increasing);
    std::vector<std::size_t> invalid;
    for (std::size_t i = 0; i < d.valid.size(); ++i) {
        if (!d.valid[i]) {
            invalid.push_back(i);
        }
    }
    CHECK(invalid == invalidAt);
    CHECK_FALSE(hasCode(o->decodeLog->snapshot(), "text.source_changed"));
}

} // namespace

TEST_CASE("text slabs: a ragged row before a sparse column's first empty cell agrees in both passes",
          "[unit]") {
    // One chunk: rows 0-19, row 5 without C (an invalid sample), C empty at rows 10 and 12 (no sample): 18
    // samples. Decode used to skip row 5 and pad the chunk with a made-up invalid sample.
    checkSparseAgreement(raggedSparseFile(20, {5}, {10, 12}), 18, {5});
    // A ragged row after the column became sparse is an invalid sample as well.
    checkSparseAgreement(raggedSparseFile(20, {5, 15}, {10, 12}), 18, {5, 13});
}

TEST_CASE("text slabs: a ragged row before a sparse column's first empty cell keeps chunk boundaries",
          "[unit][slow]") {
    // 140,000 rows, row 5 without C, C empty at row 100,000 only: every chunk boundary after the ragged row
    // used to repeat a sample (t <= the previous time at sample 65,536).
    constexpr std::uint64_t kRaggedRows = 140'000;
    checkSparseAgreement(raggedSparseFile(kRaggedRows, {5}, {100'000}), kRaggedRows - 1, {5});
}

TEST_CASE("text slabs: a file on disk decodes through guarded windows as it does from memory",
          "[unit][slow]") {
    // Over a file mapping both passes read guarded windows (4 MiB forwards, 64 KiB backwards for the hold
    // carry-ins), with lines across the window edges (D-190); the result equals the in-memory decode.
    const std::string text = bigHoldFile(400'000); // about 12 MB: three windows
    REQUIRE(text.size() > 2 * (std::size_t{4} << 20));
    const TempDir dir("guarded-windows");
    const auto path = dir.path() / "big.hold";
    writeText(path, text);
    Env env(false);
    REQUIRE(env.registry->addProfile(holdProfile()).ok());
    auto fromFile = env.registry->open(path, {}, env.context());
    REQUIRE(fromFile.ok());
    auto fromMemory = env.open(text, "big.hold");
    REQUIRE(fromMemory.ok());
    const auto& fileChannels = fromFile->source->layers[0].channels;
    const auto& memoryChannels = fromMemory->source->layers[0].channels;
    REQUIRE(fileChannels.size() == memoryChannels.size());
    for (std::size_t c = 0; c < fileChannels.size(); ++c) {
        INFO(fileChannels[c].desc.loggedName);
        const Dump a = dumpSeries(fileChannels[c].series);
        const Dump b = dumpSeries(memoryChannels[c].series);
        REQUIRE(a.values.size() == 400'000);
        CHECK(a.values.size() == b.values.size());
        CHECK(a.valid == b.valid);
        bool same = a.values.size() == b.values.size();
        for (std::size_t i = 0; i < a.values.size() && same; ++i) {
            same = a.values[i] == b.values[i] || (std::isnan(a.values[i]) && std::isnan(b.values[i]));
        }
        CHECK(same);
    }
    // The held column C crosses every slab with its last logged value (carried in through the windows).
    const Dump held = dumpSeries(fileChannels[2].series);
    CHECK(held.values[65536 * 3 + 10] == static_cast<double>((65536 * 3 + 10) / 1000));
    CHECK(held.values.back() == static_cast<double>(399'999 / 1000));
}

TEST_CASE("text slabs: a file that changed on disk fails its next chunk, not the process", "[unit]") {
    const TempDir dir("changed-on-disk");
    const auto path = dir.path() / "changed.csv";
    writeText(path, "Time,A\n0,1\n1,2\n2,3\n");
    Env env(false);
    ImportOptions opt;
    opt.importerId = std::string(kGenericCsvImporterId);
    auto o = env.registry->open(path, opt, env.context());
    REQUIRE(o.ok());
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::last_write_time(path, t + std::chrono::hours(1), ec);
    if (ec) {
        SKIP("this standard library cannot set the time of a mapped file: " << ec.message());
    }
    env.cache.clear(); // the next read decodes the chunk again
    const Dump a = dumpSeries(channel(*o, "A").series);
    REQUIRE(a.values.size() == 3);
    CHECK(std::none_of(a.valid.begin(), a.valid.end(), [](bool v) { return v; })); // sealed invalid
    CHECK(hasCode(o->decodeLog->snapshot(), "text.source_changed"));
}
