// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Hostile and malformed input (port specification Q-R02-Q-R13, 05 section 3.4): pointer wrap-around, cycles,
// out-of-range blocks, malformed gap/aux/enumeration/event blocks, and a seeded mutation smoke test. The
// reader must never read outside the file, never loop, and log a code for every decision.
#include "ld/ld_codes.hpp"
#include "ld_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::ld;
using namespace esses::formats::ld::test;
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
std::uint32_t get32(const std::vector<std::byte>& b, std::uint64_t o) {
    std::uint32_t v = 0;
    for (int k = 3; k >= 0; --k) {
        v = (v << 8) | static_cast<std::uint32_t>(b[static_cast<std::size_t>(o) + k]);
    }
    return v;
}

LdWriteInput baseInput() {
    LdWriteInput in;
    auto a = intChannel("Engine Speed", L::kTypeInt5, 4, 100, ramp(200, 1000, 5));
    a.invalid = {{0, 10}, {150, 160}};
    in.channels.push_back(a);
    auto b = intChannel("Gear State With A Name Over Thirty-Two Bytes", L::kTypeInt3, 2, 10, {0, 1, 2, 1, 0});
    b.enumRef = std::uint16_t{4};
    in.channels.push_back(b);
    in.channels.push_back(floatChannel("Lambda", L::kTypeFloat, 4, 20, {0.9, 1.0, 1.1}));
    LdEnumTable t1;
    t1.index = 4;
    t1.entries = {{0, "Off", false, 0}, {1, "On", true, 1}, {2, "Fault", true, 3}};
    LdEnumTable t2;
    t2.index = 5;
    t2.entries = {{0, "A", false, 0}};
    in.enumTables = {t1, t2};
    in.events = {{0, 0.0}, {3, 10.0}, {4, 2'000'000.0}};
    return in;
}

std::vector<std::byte> baseBytes(const LdWriteOptions& o = {}) {
    auto b = LdWriter::build(baseInput(), o);
    REQUIRE(b.ok());
    return std::move(*b);
}

struct Layout {
    std::vector<std::uint64_t> records;
    std::vector<std::uint32_t> gap, aux, data;
};
Layout layoutOf(const std::vector<std::byte>& b) {
    const auto f = parseBytes(b);
    Layout l;
    for (const LdChannel& c : f->channels()) {
        l.records.push_back(c.enc.recordOffset);
        l.gap.push_back(c.enc.gapPtr);
        l.aux.push_back(c.enc.auxPtr);
        l.data.push_back(static_cast<std::uint32_t>(c.enc.dataOffset));
    }
    return l;
}

// Decodes every channel completely and touches every validity helper: must never fault.
void exercise(const LdFile& f) {
    for (std::uint32_t k = 0; k < f.channels().size(); ++k) {
        const LdChannel& c = f.channels()[k];
        const std::uint64_t n = std::min<std::uint64_t>(c.sampleCount, 4096);
        auto v = f.decodeAsDouble(k, 0, n);
        REQUIRE(v.ok());
        auto r = f.rawValues(k, c.sampleCount - n, n);
        REQUIRE(r.ok());
        std::vector<std::uint64_t> words(64, ~std::uint64_t{0});
        f.clearInvalidBits(k, 0, n, words);
        static_cast<void>(f.validity(k, 0, c.sampleCount));
        for (const LdInterval& iv : c.invalid) {
            REQUIRE(iv.begin < iv.end);
            REQUIRE(iv.end <= c.sampleCount);
        }
    }
}

} // namespace

TEST_CASE("ld hostile: files shorter than the header are refused", "[unit][ld_hostile]") {
    auto r = LdFile::fromBuffer(share(std::vector<std::byte>(L::kHeaderSize - 1)));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Format);
    CHECK(r.error().context.detail == std::optional<std::string>(std::string(code::kTooSmall)));
    // Exactly a header of zeros: magic warning, meta pointer error, no channels, no crash.
    const auto f = parseBytes(std::vector<std::byte>(L::kHeaderSize));
    CHECK(hasCode(*f, code::kMagicUnexpected));
    CHECK(hasCode(*f, code::kMetaPtrOutOfRange));
    CHECK(f->channels().empty());
}

TEST_CASE("ld hostile: header pointers outside the file", "[unit][ld_hostile]") {
    SECTION("meta_ptr beyond the end") {
        auto b = baseBytes();
        put32(b, L::kHdrMetaPtr, static_cast<std::uint32_t>(b.size() + 10));
        const auto f = parseBytes(b);
        CHECK(f->channels().empty());
        CHECK(hasCode(*f, code::kMetaPtrOutOfRange));
        CHECK(f->log().hasErrors());
    }
    SECTION("meta_ptr inside the header") {
        auto b = baseBytes();
        put32(b, L::kHdrMetaPtr, 0x100);
        CHECK(hasCode(*parseBytes(b), code::kMetaPtrOutOfRange));
    }
    SECTION("data_ptr beyond the end is informational") {
        auto b = baseBytes();
        put32(b, L::kHdrDataPtr, 0xFFFFFFF0u);
        const auto f = parseBytes(b);
        CHECK(f->channels().size() == 3);
        CHECK(hasCode(*f, code::kDataPtrOutOfRange));
        CHECK(hasCode(*f, code::kDataPtrMismatch));
    }
    SECTION("event, venue, vehicle and weather pointers that do not fit") {
        auto b = baseBytes();
        put32(b, L::kHeaderSize + L::kEvVenuePtr, 0xFFFFFF00u);
        put32(b, L::kHeaderSize + L::kEvWeatherPtr, static_cast<std::uint32_t>(b.size() - 100));
        const auto f = parseBytes(b);
        CHECK(f->eventRecord().has_value());
        CHECK_FALSE(f->venueRecord().has_value());
        CHECK_FALSE(f->vehicleRecord().has_value());
        CHECK_FALSE(f->weatherRecord().has_value());
        CHECK(f->log().count(code::kBlockOutOfRange) == 2);
        put32(b, L::kHdrEventPtr, 0x10); // inside the header
        const auto g = parseBytes(b);
        CHECK_FALSE(g->eventRecord().has_value());
        CHECK(g->channels().size() == 3);
    }
}

TEST_CASE("ld hostile: channel list cycles and wild next pointers", "[unit][ld_hostile]") {
    auto b = baseBytes();
    const Layout l = layoutOf(b);
    SECTION("self loop") {
        put32(b, l.records[0] + L::kRecNext, static_cast<std::uint32_t>(l.records[0]));
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kListCycle));
        CHECK(f->channels().size() == 1);
    }
    SECTION("back edge") {
        put32(b, l.records[2] + L::kRecNext, static_cast<std::uint32_t>(l.records[0]));
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kListCycle));
        CHECK(hasCode(*f, code::kListBackwards));
        CHECK(f->channels().size() == 3);
        exercise(*f);
    }
    SECTION("next pointer near 2^32 does not wrap") {
        put32(b, l.records[0] + L::kRecNext, 0xFFFFFFF0u);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kEntryOutOfRange));
        CHECK(f->channels().size() == 1);
    }
    SECTION("next pointer into the last bytes of the file") {
        put32(b, l.records[0] + L::kRecNext, static_cast<std::uint32_t>(b.size() - 10));
        CHECK(hasCode(*parseBytes(b), code::kEntryOutOfRange));
    }
    SECTION("prev pointer mismatch") {
        put32(b, l.records[1] + L::kRecPrev, 12345);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kListPrevMismatch));
        CHECK(f->channels().size() == 3);
    }
    SECTION("overlapping records: the walk is bounded") {
        put32(b, l.records[2] + L::kRecNext, static_cast<std::uint32_t>(l.records[2] + 1));
        const auto f = parseBytes(b);
        CHECK(f->log().hasErrors());
        exercise(*f);
    }
}

TEST_CASE("ld hostile: sample arrays, rates and type pairs", "[unit][ld_hostile]") {
    auto b = baseBytes();
    const Layout l = layoutOf(b);
    SECTION("count beyond the file is clamped without overflow") {
        put32(b, l.records[0] + L::kRecCount, 0xFFFFFFFFu);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kDataOutOfRange));
        const LdChannel& c = f->channels()[0];
        CHECK(c.sampleCount == (b.size() - l.data[0]) / 4);
        CHECK(c.enc.storedCount == 0xFFFFFFFFu);
        exercise(*f);
    }
    SECTION("data pointer near 2^32: no samples") {
        put32(b, l.records[1] + L::kRecData, 0xFFFFFFF0u);
        const auto f = parseBytes(b);
        CHECK(f->channels()[1].sampleCount == 0);
        CHECK(hasCode(*f, code::kDataOutOfRange));
        exercise(*f);
    }
    SECTION("rate 0 with samples: skipped") {
        put16(b, l.records[1] + L::kRecRate, 0);
        const auto f = parseBytes(b);
        CHECK(f->channels().size() == 2);
        CHECK(hasCode(*f, code::kZeroRate));
        CHECK_FALSE(hasCode(*f, code::kCountMismatch)); // still walked and counted
    }
    SECTION("width 3: not decodable, skipped") {
        put16(b, l.records[2] + L::kRecWidth, 3);
        const auto f = parseBytes(b);
        CHECK(f->channels().size() == 2);
        CHECK(hasCode(*f, code::kUnknownDtype));
    }
    SECTION("an empty channel") {
        put32(b, l.records[2] + L::kRecCount, 0);
        const auto f = parseBytes(b);
        CHECK(f->channels()[2].sampleCount == 0);
        CHECK(hasCode(*f, code::kEmptyChannel));
        exercise(*f);
    }
}

TEST_CASE("ld hostile: gap blocks", "[unit][ld_hostile]") {
    const auto withEvents = [](const std::vector<std::pair<int, int>>& events) {
        LdWriteInput in;
        auto c = intChannel("c", L::kTypeInt3, 2, 10, ramp(100));
        c.rawGapEvents.emplace();
        for (const auto& [kind, idx] : events) {
            c.rawGapEvents->emplace_back(static_cast<std::uint16_t>(kind), static_cast<std::uint32_t>(idx));
        }
        in.channels.push_back(c);
        return roundTrip(in);
    };
    SECTION("an entry without exit runs to the end") {
        const auto f = withEvents({{0, 10}});
        CHECK(f->channels()[0].invalid == std::vector<LdInterval>{{10, 100}});
        CHECK(hasCode(*f, code::kGapUnpaired));
    }
    SECTION("an exit without entry is ignored; a second entry keeps the first start") {
        const auto f = withEvents({{1, 5}, {0, 10}, {0, 15}, {1, 20}});
        CHECK(f->channels()[0].invalid == std::vector<LdInterval>{{10, 20}});
        CHECK(hasCode(*f, code::kGapUnpaired));
    }
    SECTION("unsorted and overlapping intervals are merged") {
        const auto f = withEvents({{0, 50}, {1, 60}, {0, 10}, {1, 55}, {0, 70}, {1, 70}});
        CHECK(f->channels()[0].invalid == std::vector<LdInterval>{{10, 60}});
        CHECK(hasCode(*f, code::kGapUnsorted));
    }
    SECTION("indexes beyond the count are clamped") {
        const auto f = withEvents({{0, 90}, {1, 500}, {0, 700}, {1, 800}});
        CHECK(f->channels()[0].invalid == std::vector<LdInterval>{{90, 100}});
        CHECK(hasCode(*f, code::kGapOutOfRange));
    }
    SECTION("a reversed pair is read as [exit, entry)") {
        const auto f = withEvents({{0, 50}, {1, 40}});
        CHECK(f->channels()[0].invalid == std::vector<LdInterval>{{40, 50}});
        CHECK(hasCode(*f, code::kGapUnsorted));
    }
    SECTION("an unknown event kind") {
        const auto f = withEvents({{7, 3}, {0, 1}, {1, 2}});
        CHECK(f->channels()[0].invalid == std::vector<LdInterval>{{1, 2}});
        CHECK(hasCode(*f, code::kGapBlockMalformed));
    }
    SECTION("block, count and rate damage") {
        auto b = baseBytes();
        const Layout l = layoutOf(b);
        auto c = b;
        put16(c, l.gap[0] + L::kGapRate, 7);
        CHECK(hasCode(*parseBytes(c), code::kGapRateMismatch));
        c = b;
        put32(c, l.gap[0] + L::kGapCount, 0xFFFFFFFFu);
        const auto f = parseBytes(c);
        CHECK(hasCode(*f, code::kGapBlockMalformed));
        exercise(*f);
        c = b;
        put32(c, l.records[0] + L::kRecGapPtr, 0xFFFFFFFFu);
        const auto g = parseBytes(c);
        CHECK(hasCode(*g, code::kGapBlockMalformed));
        CHECK(g->channels()[0].invalid.empty());
        c = b;
        put32(c, l.gap[0] + L::kGapArrayPtr, 0xFFFFFFF0u);
        CHECK(hasCode(*parseBytes(c), code::kGapBlockMalformed));
    }
}

TEST_CASE("ld hostile: aux blocks", "[unit][ld_hostile]") {
    auto b = baseBytes();
    const Layout l = layoutOf(b);
    REQUIRE(l.aux[1] != 0);
    const std::uint64_t content = l.aux[1] + L::kAuxContentOffset;
    auto c = b;
    SECTION("aux pointer beyond the file") {
        put32(c, l.records[1] + L::kRecAuxPtr, 0xFFFFFFFEu);
        const auto f = parseBytes(c);
        CHECK(hasCode(*f, code::kAuxOutOfRange));
        CHECK(f->channels()[1].loggedName == "Gear State With A Name Over Thir");
        CHECK_FALSE(f->channels()[1].enumRef.has_value());
    }
    SECTION("content pointer beyond the file") {
        put32(c, l.aux[1] + L::kAuxContentPtr, 0xFFFFFFF0u);
        CHECK(hasCode(*parseBytes(c), code::kAuxOutOfRange));
    }
    SECTION("content marker not 1") {
        put32(c, content, 9);
        CHECK(hasCode(*parseBytes(c), code::kAuxUnknownArg));
    }
    SECTION("unknown argument type stops the scan after the known ones") {
        // First argument is the enumeration reference (type 5); damage the second (the long name).
        put32(c, content + 8 + 10, 9);
        const auto f = parseBytes(c);
        CHECK(hasCode(*f, code::kAuxUnknownArg));
        CHECK(f->channels()[1].enumRef == std::optional<std::uint16_t>(std::uint16_t{4}));
        CHECK(f->channels()[1].longName.empty());
    }
    SECTION("long name of length 0 and of a length beyond the file") {
        put16(c, content + 8 + 10 + 6, 0);
        CHECK(hasCode(*parseBytes(c), code::kAuxUnknownArg));
        put16(c, content + 8 + 10 + 6, 0xFFFF);
        auto d = c;
        d.resize(static_cast<std::size_t>(content + 8 + 10 + 8 + 100)); // cut the file inside the long name
        const auto f = parseBytes(d);
        CHECK(hasCode(*f, code::kAuxOutOfRange));
    }
    SECTION("enumeration reference with an index size other than 2") {
        put16(c, content + 8 + 6, 4);
        CHECK(hasCode(*parseBytes(c), code::kAuxUnknownArg));
    }
    SECTION("argument count far beyond the block") {
        put32(c, content + 4, 0xFFFFFFFFu);
        const auto f = parseBytes(c);
        CHECK((hasCode(*f, code::kAuxUnknownArg) || hasCode(*f, code::kAuxOutOfRange)));
        CHECK(f->channels()[1].loggedName == "Gear State With A Name Over Thirty-Two Bytes");
    }
}

TEST_CASE("ld hostile: enumeration blocks", "[unit][ld_hostile]") {
    auto b = baseBytes();
    const auto f0 = parseBytes(b);
    const std::uint32_t enumPtr = f0->header().enumPtr;
    REQUIRE(enumPtr != 0);
    REQUIRE(f0->enumTables().size() == 2);
    const std::uint64_t t2 = f0->enumTables()[1].offset;
    SECTION("block pointer beyond the file") {
        put32(b, L::kHdrEnumPtr, 0xFFFFFFF0u);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kEnumBlockMalformed));
        CHECK(f->enumTables().empty());
        CHECK(hasCode(*f, code::kEnumRefMissing));
        CHECK(f->channels()[1].kind == store::SampleKind::Integer);
    }
    SECTION("a damaged second table keeps the first") {
        put32(b, t2, 9);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kEnumBlockMalformed));
        REQUIRE(f->enumTables().size() == 1);
        CHECK(f->channels()[1].enumResolved);
    }
    SECTION("item size below 3") {
        put16(b, t2 + 10 + 6, 2);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kEnumBlockMalformed));
        CHECK(f->enumTables().size() == 1);
    }
    SECTION("severity marker damaged") {
        // The second value of table 4 is extended: its severity block follows the label "On".
        const std::uint64_t v0 = f0->enumTables()[0].offset + 10;
        const std::uint64_t v1 = v0 + 8 + 3 + 3;  // "Off": item size 6
        const std::uint64_t sev = v1 + 8 + 2 + 3; // "On": item size 5
        REQUIRE(get32(b, sev) == L::kEnumSeverityMarker);
        put32(b, sev, 5);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kEnumBlockMalformed));
        CHECK(f->enumTables().empty());
    }
    SECTION("table count far beyond the block") {
        put32(b, enumPtr + 4, 0xFFFFFFFFu);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kEnumBlockMalformed));
        CHECK(f->enumTables().size() == 2);
    }
}

TEST_CASE("ld hostile: the enumeration scan finds tables when the 0x690 pointer is 0", "[unit][ld_hostile]") {
    LdWriteOptions o;
    o.dataAlignment = 4096; // slack between the last record and the first sample array
    auto b = baseBytes(o);
    const auto f0 = parseBytes(b);
    const std::uint32_t enumPtr = f0->header().enumPtr;
    std::uint64_t lastRecordEnd = 0;
    for (const LdChannel& c : f0->channels()) {
        lastRecordEnd = std::max(lastRecordEnd, c.enc.recordOffset + L::kRecordSize);
        if (c.enc.auxPtr != 0) {
            lastRecordEnd = std::max<std::uint64_t>(lastRecordEnd, c.enc.auxPtr + 200);
        }
    }
    const std::uint64_t firstData = f0->channels()[0].enc.dataOffset;
    const std::uint64_t blockSize = f0->channels()[0].enc.recordOffset - enumPtr;
    REQUIRE(lastRecordEnd + blockSize < firstData);
    SECTION("tables placed after the records are found") {
        // Copy the enumeration block (without its 8-byte block header) into the slack, then drop the pointer.
        std::copy_n(b.begin() + enumPtr + 8, blockSize - 8,
                    b.begin() + static_cast<std::ptrdiff_t>(lastRecordEnd));
        put32(b, L::kHdrEnumPtr, 0);
        const auto f = parseBytes(b);
        CHECK(f->findEnumTable(4) != nullptr);
        CHECK(f->channels()[1].enumResolved);
        CHECK_FALSE(hasCode(*f, code::kEnumScanFailed));
    }
    SECTION("nothing to find") {
        put32(b, L::kHdrEnumPtr, 0);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kEnumScanFailed));
        CHECK(hasCode(*f, code::kEnumRefMissing));
        LdOptions noScan;
        noScan.enumScanFallback = false;
        const auto g = parseBytes(b, noScan);
        CHECK_FALSE(hasCode(*g, code::kEnumScanFailed));
        CHECK(hasCode(*g, code::kEnumRefMissing));
    }
}

TEST_CASE("ld hostile: device-event blocks", "[unit][ld_hostile]") {
    auto b = baseBytes();
    const std::uint32_t p = parseBytes(b)->header().deviceEventPtr;
    REQUIRE(p != 0);
    SECTION("another block type is ignored") {
        put32(b, p, 2);
        const auto f = parseBytes(b);
        CHECK(f->deviceEvents().empty());
        CHECK(hasCode(*f, code::kEventBlockUnknown));
    }
    SECTION("count beyond the file keeps what fits") {
        put32(b, p + 4, 0xFFFFFFFFu);
        const auto f = parseBytes(b);
        CHECK(hasCode(*f, code::kEventBlockMalformed));
        CHECK(f->deviceEvents().size() >= 3);
    }
    SECTION("non-finite times are skipped") {
        const std::uint64_t rec = p + L::kEventBlockHeaderSize + L::kEventRecordBytes; // second record
        for (int k = 0; k < 8; ++k) {
            b[static_cast<std::size_t>(rec + 2 + k)] = std::byte{0xFF}; // NaN
        }
        const auto f = parseBytes(b);
        CHECK(f->deviceEvents().size() == 2);
        CHECK(hasCode(*f, code::kEventBlockMalformed));
    }
    SECTION("block pointer beyond the file") {
        put32(b, L::kHdrDeviceEventPtr, 0xFFFFFFF0u);
        const auto f = parseBytes(b);
        CHECK(f->deviceEvents().empty());
        CHECK(hasCode(*f, code::kEventBlockMalformed));
    }
}

TEST_CASE("ld hostile: seeded mutations never crash the reader", "[unit][ld_hostile][property]") {
    LdWriteOptions o;
    o.dialect = WriteDialect::M1;
    const auto base = baseBytes(o);
    const Layout l = layoutOf(base);
    // Hot spots: header pointers and counts, record fields, gap/aux/enum/event blocks.
    std::vector<std::uint64_t> hot = {L::kHdrMetaPtr,      L::kHdrDataPtr,        L::kHdrEventPtr,
                                      L::kHdrChannelCount, L::kHdrDeviceEventPtr, L::kHdrEnumPtr};
    for (std::size_t k = 0; k < l.records.size(); ++k) {
        for (std::uint32_t off = 0; off < L::kRecordSize; off += 2) {
            hot.push_back(l.records[k] + off);
        }
        if (l.gap[k] != 0) {
            for (std::uint32_t off = 0; off < 64; off += 2) {
                hot.push_back(l.gap[k] + off);
            }
        }
        if (l.aux[k] != 0) {
            for (std::uint32_t off = 0; off < 140; off += 2) {
                hot.push_back(l.aux[k] + off);
            }
        }
    }
    const auto f0 = parseBytes(base);
    for (std::uint32_t off = 0; off < 160; off += 2) {
        hot.push_back(f0->header().enumPtr + off);
        hot.push_back(f0->header().deviceEventPtr + off);
    }
    std::mt19937_64 rng(testSeed());
    for (int iter = 0; iter < 3000; ++iter) {
        std::vector<std::byte> b = base;
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int e = 0; e < edits; ++e) {
            const std::uint64_t at = hot[rng() % hot.size()];
            if (at + 4 > b.size()) {
                continue;
            }
            switch (rng() % 4) {
            case 0:
                put32(b, at, static_cast<std::uint32_t>(rng()));
                break;
            case 1:
                put32(b, at, static_cast<std::uint32_t>(b.size() - (rng() % 64)));
                break;
            case 2:
                put32(b, at, 0xFFFFFFFFu - static_cast<std::uint32_t>(rng() % 16));
                break;
            default:
                b[static_cast<std::size_t>(at)] = static_cast<std::byte>(rng());
                break;
            }
        }
        if (rng() % 8 == 0) {
            b.resize(static_cast<std::size_t>(L::kHeaderSize + rng() % (b.size() - L::kHeaderSize)));
        }
        auto r = LdFile::fromBuffer(share(std::move(b)));
        REQUIRE(r.ok());
        exercise(**r);
    }
}
