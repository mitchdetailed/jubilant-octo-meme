// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Regressions from the M0 acceptance audit and its two adversarial reviews (docs/milestones/M0.md): record
// rules from layout evidence, the [mi2] fingerprint, gap-pair conventions, work budgets for shared gap and
// aux blocks and the enumeration scan, linear duplicate renaming, the terminator rule, device-event array
// bounds, enumeration references the reader cannot apply, duplicate table indexes and the scaled-integer
// dtype rule.
#include "ld/ld_codes.hpp"
#include "ld_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <set>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::ld;
using namespace esses::formats::ld::test;
using esses::store::Dtype;
using esses::store::Interp;
using esses::store::SampleKind;
namespace L = esses::formats::ld::layout;

namespace {

void put32(std::vector<std::byte>& b, std::uint64_t o, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        b[static_cast<std::size_t>(o) + k] = static_cast<std::byte>((v >> (8 * k)) & 0xFFu);
    }
}
void put16(std::vector<std::byte>& b, std::uint64_t o, std::uint16_t v) {
    b[static_cast<std::size_t>(o)] = static_cast<std::byte>(v & 0xFFu);
    b[static_cast<std::size_t>(o) + 1] = static_cast<std::byte>(v >> 8);
}

std::vector<std::byte> build(const LdWriteInput& in, const LdWriteOptions& o = {}) {
    auto b = LdWriter::build(in, o);
    REQUIRE(b.ok());
    return std::move(*b);
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

LdWriteInput loggerChannels() {
    LdWriteInput in;
    auto a = intChannel("Counter 1", L::kTypeInt3, 2, 10, ramp(20));
    a.shortName = "Ct1"; // a C-series short name, no unit
    a.interpByte = 0;    // "unset" on C-series records
    in.channels.push_back(a);
    auto b = intChannel("Log Memory Busy", L::kTypeInt3, 2, 10, ramp(20));
    b.shortName = "LogBusy1"; // all 8 bytes: no NUL in 0x40..0x47
    b.interpByte = 0;
    in.channels.push_back(b);
    auto c = intChannel("Oil Pressure", L::kTypeInt3, 2, 10, ramp(20));
    c.shortName = "OilP";
    c.unit = "kPa";
    in.channels.push_back(c);
    return in;
}

} // namespace

TEST_CASE("ld review: the C-series record rules follow the record layout, not the device type",
          "[unit][ld_review]") {
    // A header-modified copy of a logger file (docs/formats/ld.md "Producer fingerprints"): the logger's
    // channel records byte for byte, but device type MTCA2LD and 0x0080 at 0x054.
    const LdWriteInput in = loggerChannels();
    LdWriteOptions logger;
    logger.dialect = WriteDialect::CSeries;
    logger.deviceType = "C125";
    LdWriteOptions copy = logger;
    copy.deviceType = "MTCA2LD";
    auto copyBytes = build(in, copy);
    put16(copyBytes, L::kHdrUnknown054, 0x0080);

    const auto original = parseBytes(build(in, logger));
    const auto modified = parseBytes(copyBytes);
    CHECK(original->dialectString() == "c-series");
    CHECK(modified->dialectString().find("c-series") == std::string::npos); // the label needs C<digits>
    CHECK(modified->header().unknown040 == 0);
    CHECK(modified->header().eventPtr > 0x10000u);
    for (const auto* f : {original.get(), modified.get()}) {
        INFO("device type " << f->header().deviceType);
        const LdChannel& ct = f->channels()[0];
        CHECK(ct.shortName == "Ct1"); // not the unit 'Ct1'
        CHECK(ct.unit.empty());
        CHECK(ct.interp == Interp::Previous); // byte 0 is "unset": the Integer kind default, not Closest
        const LdChannel& busy = f->channels()[1];
        CHECK(busy.shortName == "LogBusy1"); // not glued with the unit field into one 20-byte unit
        CHECK(busy.unit.empty());
        const LdChannel& oil = f->channels()[2];
        CHECK(oil.shortName == "OilP");
        CHECK(oil.unit == "kPa");
        CHECK(hasCode(*f, code::kUnitFromShortNameCandidate));
        CHECK_FALSE(hasCode(*f, code::kUnitFromShortName));
    }
    // read -> write -> read keeps the logger layout, so the copy's records keep reading the same.
    auto again = LdWriter::inputFromFile(*modified);
    REQUIRE(again.ok());
    const auto back = parseBytes(build(*again, LdWriter::optionsFromFile(*modified)));
    CHECK(back->header().eventPtr > 0x10000u);
    for (std::size_t k = 0; k < back->channels().size(); ++k) {
        CHECK(back->channels()[k].shortName == modified->channels()[k].shortName);
        CHECK(back->channels()[k].unit == modified->channels()[k].unit);
        CHECK(back->channels()[k].interp == modified->channels()[k].interp);
    }
}

TEST_CASE("ld review: the [mi2] fingerprint needs the writer's 0 at 0x040 outside the logger layout",
          "[unit][ld_review]") {
    LdWriteInput in;
    in.channels.push_back(floatChannel("Gbox Oil Temp", L::kTypeFloat, 4, 2, {70.0, 71.0}));
    LdWriteOptions pc;
    pc.deviceType = "C185"; // logger data re-saved by the PC software: u16 2 at 0x040, event record at 0x6E2
    auto b = build(in, pc);
    put16(b, L::kHdrUnknown05A, 0x0064);
    put16(b, L::kHdrUnknown05C, 1); // u32 0x00010064 at 0x05A, as on 20251009-1552000.ld
    const auto resaved = parseBytes(b);
    CHECK(resaved->header().unknown040 == 2);
    CHECK(resaved->dialectString() == "pc-layout");

    put16(b, L::kHdrUnknown040, 0); // the public writer's 0 at 0x040
    CHECK(parseBytes(b)->dialectString() == "pc-layout,sim-exporter");

    LdWriteOptions logger;
    logger.dialect = WriteDialect::CSeries;
    auto c = build(in, logger); // 0 at 0x040, but the logger layout
    put16(c, L::kHdrUnknown05A, 0x0064);
    put16(c, L::kHdrUnknown05C, 1);
    CHECK(parseBytes(c)->dialectString() == "c-series");
}

TEST_CASE("ld review: gap pairs in the shifted convention", "[unit][ld_review]") {
    const auto file = [](const std::vector<std::pair<int, int>>& events, GapConvention convention) {
        LdWriteInput in;
        auto c = intChannel("RPM", L::kTypeInt3, 2, 10, ramp(100));
        c.rawGapEvents.emplace();
        for (const auto& [kind, idx] : events) {
            c.rawGapEvents->emplace_back(static_cast<std::uint16_t>(kind), static_cast<std::uint32_t>(idx));
        }
        in.channels.push_back(c);
        LdOptions o;
        o.gapConvention = convention;
        return roundTrip(in, {}, o);
    };
    // The converter's Python writer: (first invalid + 1, last invalid). One invalid sample (20) gives a
    // reversed pair, two (30, 31) an empty pair, five (40..44) a normal pair.
    const std::vector<std::pair<int, int>> events = {{0, 21}, {1, 20}, {0, 31}, {1, 31}, {0, 41}, {1, 44}};
    SECTION("as written: a reversed pair is [exit, entry), not dropped; the convention is reported") {
        const auto f = file(events, GapConvention::Standard);
        CHECK(f->channels()[0].invalid == std::vector<LdInterval>{{20, 21}, {41, 44}});
        CHECK(hasCode(*f, code::kGapUnsorted));
        CHECK(hasCode(*f, code::kGapConventionShifted));
    }
    SECTION("ld.gapConvention shifted reads [entry - 1, exit + 1)") {
        const auto f = file(events, GapConvention::Shifted);
        CHECK(f->channels()[0].invalid == std::vector<LdInterval>{{20, 21}, {30, 32}, {40, 45}});
        CHECK(hasCode(*f, code::kGapConventionShifted));
    }
    SECTION("standard pairs raise no convention code") {
        const auto f = file({{0, 10}, {1, 15}}, GapConvention::Standard);
        CHECK(f->channels()[0].invalid == std::vector<LdInterval>{{10, 15}});
        CHECK_FALSE(hasCode(*f, code::kGapConventionShifted));
        CHECK_FALSE(hasCode(*f, code::kGapUnsorted));
    }
}

TEST_CASE("ld review: records sharing one large gap block cost O(file), not O(records x events)",
          "[unit][ld_review]") {
    constexpr std::uint64_t kPairs = 20'000;
    constexpr int kSharers = 300;
    LdWriteInput in;
    auto big = intChannel("big", L::kTypeInt3, 2, 10, ramp(static_cast<std::int64_t>(2 * kPairs), 0, 0));
    for (std::uint64_t i = 0; i < kPairs; ++i) {
        big.invalid.push_back({2 * i, 2 * i + 1});
    }
    in.channels.push_back(big);
    for (int k = 0; k < kSharers; ++k) {
        in.channels.push_back(intChannel("s" + std::to_string(k), L::kTypeInt3, 2, 10, ramp(4)));
    }
    auto b = build(in);
    const auto plain = parseBytes(b);
    const std::uint32_t gap = plain->channels()[0].enc.gapPtr;
    REQUIRE(gap != 0);
    for (int k = 1; k <= kSharers; ++k) {
        put32(b, plain->channels()[k].enc.recordOffset + L::kRecGapPtr, gap);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto f = parseBytes(b);
    const double s = secondsSince(t0);
    CHECK(f->channels()[0].invalid.size() == kPairs);
    // The interval budget (size / 6) lets only a few records through; the rest log and ignore the block.
    const std::uint64_t budget = b.size() / L::kGapEventSize;
    std::uint64_t processed = 0;
    for (const LdChannel& c : f->channels()) {
        processed += c.invalid.empty() ? 0 : kPairs;
    }
    CHECK(processed <= budget + kPairs);
    CHECK(hasCode(*f, code::kGapBlockShared));
    CHECK(hasCode(*f, code::kGapBlockMalformed));
    CHECK(s < 10.0); // generous: the shape (O(file)) is what the budget asserts above
}

TEST_CASE("ld review: records sharing one aux content read at most the file's bytes", "[unit][ld_review]") {
    constexpr int kSharers = 300;
    LdWriteInput in;
    auto named = intChannel("n", L::kTypeInt3, 2, 10, ramp(4));
    named.longName = std::string(60'000, 'x');
    in.channels.push_back(named);
    for (int k = 0; k < kSharers; ++k) {
        in.channels.push_back(intChannel("s" + std::to_string(k), L::kTypeInt3, 2, 10, ramp(4)));
    }
    auto b = build(in);
    const auto plain = parseBytes(b);
    const std::uint32_t aux = plain->channels()[0].enc.auxPtr;
    REQUIRE(aux != 0);
    for (int k = 1; k <= kSharers; ++k) {
        put32(b, plain->channels()[k].enc.recordOffset + L::kRecAuxPtr, aux);
    }
    const auto f = parseBytes(b);
    std::uint64_t longNameBytes = 0;
    for (const LdChannel& c : f->channels()) {
        longNameBytes += c.longName.size();
    }
    CHECK(f->channels()[0].longName.size() == 60'000);
    CHECK(longNameBytes <= b.size());
    CHECK(hasCode(*f, code::kAuxOutOfRange));
    CHECK(f->channels().size() == kSharers + 1); // names fall back to the 32-byte field, nothing is lost
}

TEST_CASE("ld review: the enumeration scan is linear and bounded", "[unit][ld_review]") {
    // Channel 0's sample array is the crafted region: its count is set to 0 afterwards, so the region lies
    // between the last record and the first sample array that the scan searches (05 section 3.9).
    const auto fileWith = [](std::vector<std::byte> region) {
        LdWriteInput in;
        auto blob = intChannel("blob", L::kTypeInt0, 1, 10, {});
        blob.rawData = std::move(region);
        in.channels.push_back(blob);
        auto e = intChannel("State", L::kTypeInt3, 2, 10, {0, 1, 2});
        e.enumRef = std::uint16_t{65000};
        in.channels.push_back(e);
        auto b = build(in);
        const auto plain = parseBytes(b);
        put32(b, plain->channels()[0].enc.recordOffset + L::kRecCount, 0);
        REQUIRE(parseBytes(b)->header().enumPtr == 0);
        return b;
    };
    const auto table = [](std::vector<std::byte>& out, std::uint16_t index) {
        const std::size_t o = out.size();
        out.resize(o + 21);
        put32(out, o, L::kEnumTableMarker);
        put16(out, o + 4, 1); // one value
        put16(out, o + 6, L::kEnumIndexSize);
        put16(out, o + 8, index);
        put32(out, o + 10, L::kEnumValueMarker);
        put16(out, o + 14, 0);
        put16(out, o + 16, 3); // i16 value + "" + NUL
    };
    SECTION("100,000 minimal tables, none of them referenced: no quadratic table lookup") {
        std::vector<std::byte> region;
        for (std::uint32_t i = 0; i < 100'000; ++i) {
            table(region, static_cast<std::uint16_t>(i % 60'000));
        }
        const auto b = fileWith(std::move(region));
        const auto t0 = std::chrono::steady_clock::now();
        const auto f = parseBytes(b);
        CHECK(secondsSince(t0) < 10.0);
        CHECK(f->enumTables().empty()); // unreferenced tables found by the heuristic scan are not kept
        CHECK(hasCode(*f, code::kEnumScanFailed));
        CHECK(hasCode(*f, code::kEnumRefMissing));
    }
    SECTION("the referenced table is found and the scan stops there") {
        std::vector<std::byte> region;
        for (std::uint32_t i = 0; i < 1'000; ++i) {
            table(region, static_cast<std::uint16_t>(i));
        }
        table(region, 65000);
        for (std::uint32_t i = 0; i < 1'000; ++i) {
            table(region, static_cast<std::uint16_t>(i));
        }
        const auto f = parseBytes(fileWith(std::move(region)));
        REQUIRE(f->findEnumTable(65000) != nullptr);
        CHECK(f->enumTables().size() == 1);
        CHECK(f->channels()[1].kind == SampleKind::Enum);
        CHECK_FALSE(hasCode(*f, code::kEnumScanFailed));
    }
    SECTION("candidates that share one long value chain stop at the work budget") {
        // Every value's label holds a table header whose first value is the next value of the chain, and the
        // chain breaks every 498 values: each candidate walks up to 498 values before it fails.
        std::vector<std::byte> region;
        for (std::uint32_t v = 0; v < 100'000; ++v) {
            const std::size_t o = region.size();
            region.resize(o + 20);
            put32(region, o, v % 498 == 497 ? 9u : L::kEnumValueMarker); // a broken value now and then
            put16(region, o + 4, 0);
            put16(region, o + 6, 12); // i16 + 10 label bytes
            put32(region, o + 10, L::kEnumTableMarker);
            put16(region, o + 14, 499);
            put16(region, o + 16, L::kEnumIndexSize);
            put16(region, o + 18, static_cast<std::uint16_t>(v % 60'000));
        }
        const auto b = fileWith(std::move(region));
        const auto t0 = std::chrono::steady_clock::now();
        const auto f = parseBytes(b);
        CHECK(secondsSince(t0) < 10.0);
        CHECK(hasCode(*f, code::kEnumScanFailed));
    }
}

TEST_CASE("ld review: duplicate names are renamed in linear time", "[unit][ld_review]") {
    constexpr int kCount = 3'000;
    LdWriteInput in;
    for (int k = 0; k < kCount; ++k) {
        in.channels.push_back(intChannel("Dup", L::kTypeInt3, 2, 10, {1}));
    }
    in.channels.push_back(intChannel("Dup (2)", L::kTypeInt3, 2, 10, {1})); // an original name in the way
    const auto b = build(in);
    const auto t0 = std::chrono::steady_clock::now();
    const auto f = parseBytes(b);
    CHECK(secondsSince(t0) < 10.0);
    std::set<std::string> names;
    for (const LdChannel& c : f->channels()) {
        names.insert(c.loggedName);
    }
    CHECK(names.size() == f->channels().size());
    CHECK(f->channels()[0].loggedName == "Dup");
    CHECK(f->channels()[1].loggedName == "Dup (3)"); // "Dup (2)" is taken by the last channel
    CHECK(f->channels()[kCount - 1].loggedName == "Dup (" + std::to_string(kCount + 1) + ")");
    CHECK(f->channels()[kCount].loggedName == "Dup (2)");
}

TEST_CASE("ld review: only the all-zero last record is the terminator", "[unit][ld_review]") {
    LdWriteInput in;
    in.channels.push_back(floatChannel("Before", L::kTypeFloat, 4, 10, {1.0}));
    in.channels.push_back(intChannel("Never Logged", L::kTypeInt5, 4, 10, {}));
    in.channels.push_back(floatChannel("After", L::kTypeFloat, 4, 10, {2.0}));
    LdWriteOptions m1;
    m1.dialect = WriteDialect::M1; // writes the terminator record
    auto b = build(in, m1);
    const auto plain = parseBytes(b);
    REQUIRE(plain->channels().size() == 3);
    put16(b, plain->channels()[1].enc.recordOffset + L::kRecRate, 0); // named, rate 0, count 0
    const auto f = parseBytes(b);
    CHECK(f->channels().size() == 2);
    CHECK(hasCode(*f, code::kZeroRate)); // a skipped channel, said so
    CHECK(hasCode(*f, code::kTerminatorEntry));
    for (const auto& e : f->log().entries()) {
        if (e.code == code::kTerminatorEntry) {
            CHECK(e.ctx.repeats == 1); // only the real terminator
        }
    }
}

TEST_CASE("ld review: a device-event record array inside the header is not read", "[unit][ld_review]") {
    LdWriteInput in;
    in.channels.push_back(floatChannel("A", L::kTypeFloat, 4, 10, {1.0}));
    in.events = {{0, 0.0}, {3, 10.0}};
    auto b = build(in);
    const std::uint32_t p = parseBytes(b)->header().deviceEventPtr;
    REQUIRE(p != 0);
    put32(b, p + 4, 80); // 80 records ...
    put32(b, p + 8, 0);  // ... at offset 0: the header bytes
    const auto f = parseBytes(b);
    CHECK(f->deviceEvents().empty());
    CHECK(hasCode(*f, code::kEventBlockMalformed));
}

TEST_CASE("ld review: enumeration references the reader cannot apply, duplicate table indexes",
          "[unit][ld_review]") {
    LdWriteInput in;
    auto scaled = intChannel("BattU Lo", L::kTypeInt5, 4, 10, {1, 2, 16383});
    scaled.dec = 1;
    scaled.enumRef = std::uint16_t{4};
    in.channels.push_back(scaled);
    auto real = floatChannel("ABS LatAccel", L::kTypeFloat, 4, 10, {0.5, 255.0});
    real.enumRef = std::uint16_t{4};
    in.channels.push_back(real);
    auto gear = intChannel("Gear", L::kTypeInt3, 2, 10, {0, 1});
    gear.enumRef = std::uint16_t{4};
    in.channels.push_back(gear);
    LdEnumTable first;
    first.index = 4;
    first.entries = {{0, "Off", false, 0}, {1, "On", false, 0}};
    LdEnumTable second;
    second.index = 4;
    second.entries = {{0, "Other", false, 0}};
    in.enumTables = {first, second};
    const auto f = roundTrip(in);
    CHECK(f->enumTables().size() == 2); // both kept for a verbatim write
    REQUIRE(f->findEnumTable(4) != nullptr);
    CHECK(f->findEnumTable(4)->entries[0].label == "Off"); // the first one wins
    CHECK(hasCode(*f, code::kEnumIndexDuplicate));
    CHECK(f->channels()[0].kind == SampleKind::Real);
    CHECK(f->channels()[1].kind == SampleKind::Real);
    CHECK(f->channels()[2].kind == SampleKind::Enum);
    std::set<std::string> reasons;
    for (const auto& e : f->log().entries()) {
        if (e.code == code::kEnumRefIgnored) {
            reasons.insert(e.ctx.detail.value_or(""));
        }
    }
    CHECK(reasons == std::set<std::string>{"scaled", "float"});
}

TEST_CASE("ld review: scaled integers keep every raw step (F64 where F32 would lose them)",
          "[unit][ld_review]") {
    LdWriteInput in;
    auto m1 = intChannel("Fuel Film Volume Main", L::kTypeInt5, 4, 10, {1'700'000'001, -3, 5});
    m1.scale = 10000; // M1 carries decimals in scale (E-24): dec 3 + 4 = 7 decimals
    m1.dec = 3;
    in.channels.push_back(m1);
    auto wide = intChannel("Odometer", L::kTypeInt5, 4, 10, {33'554'433, 0}); // > 2^24 raw
    wide.dec = 2;
    in.channels.push_back(wide);
    auto narrow = intChannel("Throttle", L::kTypeInt5, 4, 10, {1000, 0});
    narrow.scale = 10;
    narrow.dec = 1; // 2 decimals, raw range far below 2^24
    in.channels.push_back(narrow);
    auto shortRaw = intChannel("Lambda", L::kTypeInt3, 2, 10, {1000, 0});
    shortRaw.scale = 10000;
    shortRaw.dec = 1; // 5 decimals: F64 like dec >= 5 always was
    in.channels.push_back(shortRaw);
    const auto f = roundTrip(in);
    CHECK(f->channels()[0].dtype == Dtype::F64);
    CHECK(decodeAll(*f, 0)[0] == 1'700'000'001.0 / 1e7);
    CHECK(f->channels()[1].dtype == Dtype::F64);
    CHECK(decodeAll(*f, 1)[0] == 335'544.33);
    CHECK(f->channels()[2].dtype == Dtype::F32);
    CHECK(f->channels()[3].dtype == Dtype::F64);
}
