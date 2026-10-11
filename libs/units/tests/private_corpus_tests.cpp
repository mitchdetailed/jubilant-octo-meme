// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The unit spellings of the private S-1 corpus (fixtures/private/s1: the user's own logs, never committed)
// either resolve in data/units.json or are listed in tests/data/corpus_unmapped_units.json with a reason. The
// unit text of each channel record is taken with the unit rule of 05 section 3 (port specification R5) from
// the record bytes at the offsets the oracle output (tools/ld_ref_decode.py, fixtures/private/s1/ref/*.json)
// reports. Skips when the private directory is absent (08 section 6.2).
#include "units_test_support.hpp"

#include <esses/base/utf8.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace esses;
using namespace esses::units;

namespace {

// Code page 1252 bytes 0x80-0x9F (0 = undefined, kept as U+FFFD); 0xA0-0xFF map to U+00A0-U+00FF.
constexpr std::array<char32_t, 32> kCp1252High = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

// A fixed-width string field: bytes up to the first NUL, UTF-8 when valid, else code page 1252, trimmed.
std::string field(const std::vector<char>& bytes, std::size_t begin, std::size_t width) {
    std::string raw;
    for (std::size_t i = begin; i < begin + width && i < bytes.size() && bytes[i] != '\0'; ++i) {
        raw += bytes[i];
    }
    if (!utf8::isValid(raw)) {
        std::string decoded;
        for (const char c : raw) {
            const auto b = static_cast<unsigned char>(c);
            char32_t cp = b;
            if (b >= 0x80 && b < 0xA0) {
                cp = kCp1252High[b - 0x80] != 0 ? kCp1252High[b - 0x80] : utf8::kReplacement;
            }
            utf8::append(decoded, cp);
        }
        raw = std::move(decoded);
    }
    const auto first = raw.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    return raw.substr(first, raw.find_last_not_of(' ') - first + 1);
}

std::optional<std::vector<char>> readRange(std::ifstream& in, std::uint64_t offset, std::size_t size) {
    std::vector<char> out(size);
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    if (!in.read(out.data(), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return out;
}

} // namespace

TEST_CASE("private corpus: every unit spelling of the S-1 corpus resolves or is consciously unmapped",
          "[unit][oracle]") {
    const std::filesystem::path refDir = test::repoRoot() / "fixtures" / "private" / "s1" / "ref";
    if (!std::filesystem::is_directory(refDir)) {
        SKIP("fixtures/private/s1/ref is not present (private corpus)");
    }
    const UnitRegistry& reg = test::shipped();
    const auto unmappedDoc = nlohmann::json::parse(test::readFile(
        pathFromUtf8(ESSES_TEST_SOURCE_DIR) / "tests" / "data" / "corpus_unmapped_units.json"));
    std::map<std::string, std::string> unmapped;
    for (const auto& e : unmappedDoc["spellings"]) {
        const std::string s = e["spelling"].get<std::string>();
        INFO("unmapped list entry '" << s << "'");
        CHECK_FALSE(reg.find(s).has_value()); // an entry that now resolves must leave the list
        unmapped.emplace(s, e["reason"].get<std::string>());
    }
    std::vector<std::string> prefixes;
    for (const auto& e : unmappedDoc["prefixes"]) {
        prefixes.push_back(e["prefix"].get<std::string>());
    }

    std::size_t files = 0, missingFiles = 0, channels = 0, resolved = 0, listed = 0, shortNames = 0,
                empty = 0;
    std::map<std::string, std::size_t> unexpected;
    for (const auto& entry : std::filesystem::directory_iterator(refDir)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        const auto oracle = nlohmann::json::parse(test::readFile(entry.path()), nullptr, false);
        if (oracle.is_discarded() || !oracle.contains("file") || !oracle.contains("channels")) {
            continue;
        }
        const std::filesystem::path log = pathFromUtf8(oracle["file"].get<std::string>());
        std::ifstream in(log, std::ios::binary);
        if (!in) {
            ++missingFiles;
            continue;
        }
        ++files;
        const auto header = readRange(in, 0x40, 0x12);
        REQUIRE(header.has_value());
        // C-series logger layout: u16 at 0x40 is 0 (ld.md; header-rewritten copies keep it while the device
        // type field may change), where a rule-(c) string that does not resolve stays a short name.
        const bool cSeriesLayout = (*header)[0] == 0 && (*header)[1] == 0;
        for (const auto& c : oracle["channels"]) {
            const auto record = readRange(in, c["offset"].get<std::uint64_t>() + 0x40, 20);
            REQUIRE(record.has_value());
            ++channels;
            const bool nulInFirst8 =
                std::find(record->begin(), record->begin() + 8, '\0') != record->begin() + 8;
            std::string unit;
            if (!cSeriesLayout && !nulInFirst8) {
                unit = field(*record, 0, 20); // (a) one 20-byte unit
            } else if (const std::string at48 = field(*record, 8, 12); !at48.empty()) {
                unit = at48; // (b) short name at 0x40, unit at 0x48
            } else {
                unit = field(*record, 0, 8); // (c) the unit sits in the short-name field
                if (cSeriesLayout && !reg.find(unit)) {
                    ++shortNames; // stays a short name; the unit is empty
                    continue;
                }
            }
            if (unit.empty()) {
                ++empty;
            }
            if (reg.find(unit)) {
                ++resolved;
            } else if (unmapped.contains(unit) ||
                       std::any_of(prefixes.begin(), prefixes.end(),
                                   [&](const std::string& p) { return unit.starts_with(p); })) {
                ++listed;
            } else {
                ++unexpected[unit];
            }
        }
    }
    INFO("files " << files << " (missing " << missingFiles << "), channels " << channels << ": resolved "
                  << resolved << " (of which empty " << empty << "), consciously unmapped " << listed
                  << ", C-series short names " << shortNames);
    for (const auto& [spelling, count] : unexpected) {
        FAIL_CHECK(
            "unit spelling '" << spelling << "' (" << count
                              << " channels) neither resolves nor is listed in corpus_unmapped_units.json");
    }
    CHECK(files > 0);
    CHECK(unexpected.empty());
    WARN("S-1 corpus: " << files << " files, " << channels << " channels; " << resolved << " resolve ("
                        << empty << " unitless), " << listed << " consciously unmapped, " << shortNames
                        << " C-series short names");
}
