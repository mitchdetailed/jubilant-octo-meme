// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli helpers: SHA-256 against the FIPS 180-4 examples, the file-name encoding of channel names, the
// expected-channel CSV format and its comparison rules, Windows argument quoting, schema-id parsing and the
// canonical JSON text. The commands themselves run end to end in the fixture tests (fixtures.*, cli.*).
#include "cli_common.hpp"
#include "expect_channels.hpp"
#include "process.hpp"
#include "sha256.hpp"
#include "validate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>

using namespace esses;
using namespace esses::cli;

TEST_CASE("sha256 matches the FIPS 180-4 examples", "[unit][cli]") {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 s;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) {
        s.update(chunk);
    }
    CHECK(s.hexDigest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Every split point of a two-block message gives the same digest.
    const std::string msg(130, 'x');
    const std::string whole = sha256Hex(msg);
    for (std::size_t cut = 0; cut <= msg.size(); cut += 13) {
        Sha256 t;
        t.update(std::string_view(msg).substr(0, cut));
        t.update(std::string_view(msg).substr(cut));
        CHECK(t.hexDigest() == whole);
    }
}

TEST_CASE("channel names round-trip through the file-name encoding", "[unit][cli]") {
    CHECK(percentEncode("Engine Speed") == "Engine Speed");
    CHECK(percentEncode("Speed/Dist: a*b?") == "Speed%2FDist%3A a%2Ab%3F");
    CHECK(percentEncode("100%") == "100%25");
    CHECK(percentEncode("trailing.") == "trailing%2E");
    CHECK(percentEncode("trailing ") == "trailing%20");
    CHECK(percentEncode("\xC2\xB0"
                        "C") == "%C2%B0C");
    for (const std::string s : {"Engine Speed", "a/b\\c:d*e?f\"g<h>i|j", "100% ", "x.",
                                "Temp \xC2\xB0"
                                "C",
                                ""}) {
        CHECK(percentDecode(percentEncode(s)) == s);
    }
    CHECK(percentDecode("bad%2") == "bad%2");
    CHECK(splitList("a,b,,c") == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("expected channel files: names and CSV rows", "[unit][cli]") {
    CHECK(expectedFileName(7, "Engine Speed") == "007_Engine Speed.csv");
    CHECK(expectedFileName(1234, "a/b") == "1234_a%2Fb.csv");
    const auto parsed = parseExpectedFileName("1234_a%2Fb.csv");
    REQUIRE(parsed.has_value());
    CHECK(parsed->first == 1234);
    CHECK(parsed->second == "a/b");
    CHECK_FALSE(parseExpectedFileName("notes.txt").has_value());
    CHECK_FALSE(parseExpectedFileName("_x.csv").has_value());

    auto rows = parseExpectedCsv("index,timeUs,value,valid\n0,0,1.5,1\n1,100000,nan,0\n3,300000,-inf,0\n");
    REQUIRE(rows.ok());
    REQUIRE(rows->size() == 3);
    CHECK((*rows)[0].value == 1.5);
    CHECK((*rows)[1].valid == false);
    CHECK(std::isnan((*rows)[1].value));
    CHECK((*rows)[2].index == 3);
    CHECK(std::isinf((*rows)[2].value));
    CHECK_FALSE(parseExpectedCsv("index,time,value,valid\n").ok());
    CHECK_FALSE(parseExpectedCsv("index,timeUs,value,valid\n0,0,1,1\n0,0,1,1\n").ok()); // indices ascend
    CHECK_FALSE(parseExpectedCsv("index,timeUs,value,valid\n0,0,1,2\n").ok());
    CHECK_FALSE(parseExpectedCsv("index,timeUs,value,valid\n0,0,x,1\n").ok());

    const std::string marked = markInvalid("index,timeUs,value,valid\n0,0,1,1\n1,1,2,1\n2,2,3,1\n", {{1, 2}});
    CHECK(marked == "index,timeUs,value,valid\n0,0,1,1\n1,1,2,0\n2,2,3,1\n");
}

TEST_CASE("comparison tolerances count ulps", "[unit][cli]") {
    CHECK(ulpDistanceF32(1.0, 1.0) == 0);
    CHECK(ulpDistanceF32(1.0, static_cast<double>(std::nextafter(1.0f, 2.0f))) == 1);
    CHECK(ulpDistanceF32(-0.0, 0.0) == 0);
    CHECK(ulpDistanceF32(std::nan(""), std::nan("")) == 0);
    CHECK(ulpDistanceF32(std::nan(""), 1.0) == std::numeric_limits<std::uint64_t>::max());
    CHECK(ulpDistanceF64(1.0, std::nextafter(1.0, 2.0)) == 1);
    CHECK(ulpDistanceF64(-1.0, std::nextafter(-1.0, 0.0)) == 1);
    auto tol = parseTolerance(
        nlohmann::json{{"F32", "1ulp"}, {"F64", "exact"}, {"I32", "exact"}, {"timesUs", "exact"}});
    REQUIRE(tol.ok());
    CHECK(tol->f32Ulps == 1);
    CHECK(tol->f64Ulps == 0);
    CHECK_FALSE(parseTolerance(nlohmann::json{{"F32", "1 ulp"}, {"F64", "exact"}}).ok());
}

TEST_CASE("reader names match the oracle's 32-byte names", "[unit][cli]") {
    CHECK(namesCompatible("Engine Speed", "Engine Speed"));
    CHECK(namesCompatible("Engine Speed (2)", "Engine Speed"));
    CHECK(namesCompatible("A channel name far longer than thirty-two bytes",
                          "A channel name far longer than th"));
    CHECK(namesCompatible("Temp \xC2\xB0"
                          "C",
                          "Temp \xC3\x82\xC2\xB0"
                          "C")); // UTF-8 bytes seen as cp1252
    CHECK(namesCompatible("Euro \xE2\x82\xAC", "Euro \xC3\xA2\xE2\x80\x9A\xC2\xAC"));
    CHECK_FALSE(namesCompatible("Engine Speed", "Gear"));
    CHECK_FALSE(namesCompatible("Gear", "Gear Position"));
}

TEST_CASE("Windows command-line quoting", "[unit][cli]") {
    CHECK(quoteWindowsArgument("plain") == "plain");
    CHECK(quoteWindowsArgument("") == "\"\"");
    CHECK(quoteWindowsArgument("with space") == "\"with space\"");
    CHECK(quoteWindowsArgument("C:\\Program Files\\x\\") == "\"C:\\Program Files\\x\\\\\"");
    CHECK(quoteWindowsArgument("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(quoteWindowsArgument("a\\\"b") == "\"a\\\\\\\"b\"");
    CHECK(quoteWindowsArgument("no\\quote") == "no\\quote");
}

TEST_CASE("schema ids and canonical JSON", "[unit][cli]") {
    CHECK(SchemaStore::schemaName("urn:esses:probe-1") == std::optional<std::string>("probe-1"));
    CHECK(SchemaStore::schemaName("esses:laps-1") == std::optional<std::string>("laps-1"));
    CHECK(SchemaStore::schemaName("https://example.org/schemas/fixture-1.schema.json") ==
          std::optional<std::string>("fixture-1"));
    CHECK_FALSE(SchemaStore::schemaName("urn:other:probe-1").has_value());
    CHECK_FALSE(SchemaStore::schemaName("urn:esses:probe").has_value());
    CHECK(SchemaStore::isMetaSchema("http://json-schema.org/draft-07/schema#"));

    nlohmann::json j = {
        {"b", 1}, {"a", {{"y", 0.5}, {"x", -0.0}}}, {"schemaVersion", 1}, {"$schema", "urn:esses:x-1"}};
    auto text = canonicalText(j);
    REQUIRE(text.ok());
    CHECK(*text ==
          "{\n  \"$schema\": \"urn:esses:x-1\",\n  \"schemaVersion\": 1,\n  \"a\": {\n    \"x\": 0,\n"
          "    \"y\": 0.5\n  },\n  \"b\": 1\n}\n");
    CHECK_FALSE(canonicalText(nlohmann::json{{"v", std::numeric_limits<double>::infinity()}}).ok());
    auto parsed = parseJson("{\"a\": [1, 2");
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().code == ErrorCode::Format);
}
