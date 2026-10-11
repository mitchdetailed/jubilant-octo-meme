// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ImportLog (05 section 1.1): coalescing after the third occurrence, canonical snapshot order, errors.
#include "ld/ld_codes.hpp"

#include <esses/formats/import_log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace esses::formats;
using Level = ImportLogEntry::Level;

TEST_CASE("import log: identical keys coalesce after the third occurrence", "[unit][import_log]") {
    ImportLog log;
    ImportLogEntry::Context ctx;
    ctx.channel = "RPM";
    for (int i = 0; i < 10; ++i) {
        log.add(Level::Info, "ld.zero_scale", "scale 0", ctx);
    }
    log.add(Level::Info, "ld.zero_scale", "scale 0", {}); // other channel key
    const auto entries = log.entries();
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].ctx.repeats == 1);
    CHECK(entries[1].ctx.repeats == 1);
    CHECK(entries[2].ctx.repeats == 8);
    CHECK(entries[3].ctx.repeats == 1);
    CHECK(log.count("ld.zero_scale") == 4);
    CHECK(log.version() == 11);
}

TEST_CASE("import log: snapshot is in canonical (code, layer, channel, offset) order", "[unit][import_log]") {
    ImportLog log;
    ImportLogEntry::Context a;
    a.channel = "b";
    a.offset = 9;
    ImportLogEntry::Context b;
    b.channel = "a";
    b.offset = 50;
    log.add(Level::Warning, "ld.zero_mul", "x", a);
    log.add(Level::Info, "ld.dps_from_dec", "y", b);
    log.add(Level::Warning, "ld.zero_mul", "z", b);
    const auto snap = log.snapshot();
    REQUIRE(snap.size() == 3);
    CHECK(snap[0].code == "ld.dps_from_dec");
    CHECK(snap[1].ctx.channel == "a");
    CHECK(snap[2].ctx.channel == "b");
    CHECK_FALSE(log.hasErrors());
    log.add(Level::Error, "ld.list_cycle", "cycle");
    CHECK(log.hasErrors());
    CHECK(importLogLevelName(Level::Warning) == "Warning");
}

TEST_CASE("import log: copies are independent and adds are thread-safe", "[unit][import_log]") {
    ImportLog log;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&log, t] {
            for (int i = 0; i < 100; ++i) {
                ImportLogEntry::Context ctx;
                ctx.channel = std::to_string(t);
                log.add(Level::Info, "ld.empty_channel", "n", ctx);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    std::uint32_t total = 0;
    for (const auto& e : log.entries()) {
        total += e.ctx.repeats;
    }
    CHECK(total == 400);
    ImportLog copy = log;
    copy.add(Level::Info, "ld.empty_channel", "extra");
    CHECK(copy.size() == log.size() + 1);
    DecodeLog decodeLog;
    decodeLog.add(Level::Warning, "ld.io_fault", "fault");
    CHECK(decodeLog.contains("ld.io_fault"));
}

TEST_CASE("import log: the .ld code table is unique and documented", "[unit][import_log]") {
    std::set<std::string_view> seen;
    for (const auto& c : esses::formats::ld::allCodes()) {
        CHECK(seen.insert(c.code).second);
        CHECK((c.code.starts_with("ld.") || c.code.starts_with("ldx.")));
        CHECK_FALSE(c.meaning.empty());
    }
    CHECK(esses::formats::ld::findCode("ld.count_mismatch") != nullptr);
    CHECK(esses::formats::ld::findCode("ld.no_such_code") == nullptr);
}
