// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The canonical JSON form of 02 section 6.5 rule 1 (private writer) and OpaqueJson.
#include "json_canonical.hpp"

#include <esses/model/opaque_json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>

using namespace esses;
using namespace esses::model;
using nlohmann::json;

namespace {

std::string canon(const json& j) {
    auto r = model::detail::canonicalJson(j);
    REQUIRE(r.ok());
    return *r;
}

} // namespace

TEST_CASE("canonical json: member order puts $schema and schemaVersion first, then code-unit order",
          "[unit]") {
    json j = json::object();
    j["b"] = 1;
    j["schemaVersion"] = 1;
    j["a"] = 2;
    j["$schema"] = "urn:esses:x-1";
    j["Z"] = 3;
    j["\xC3\xA9"] = 4; // U+00E9 sorts after every ASCII key
    j["$other"] = 5;
    CHECK(canon(j) == "{\n"
                      "  \"$schema\": \"urn:esses:x-1\",\n"
                      "  \"schemaVersion\": 1,\n"
                      "  \"$other\": 5,\n"
                      "  \"Z\": 3,\n"
                      "  \"a\": 2,\n"
                      "  \"b\": 1,\n"
                      "  \"\xC3\xA9\": 4\n"
                      "}\n");
    CHECK(model::detail::canonicalKeyLess("$schema", "schemaVersion"));
    CHECK(model::detail::canonicalKeyLess("schemaVersion", "$a"));
    CHECK(model::detail::canonicalKeyLess("Z", "a"));
}

TEST_CASE("canonical json: arrays are inline unless they hold an object", "[unit]") {
    const json j = json::parse(R"({"sync": [[0, 1250000], [5, 6]], "names": ["a.ldx", "b.ldx"], "empty": [],
                                   "obj": {}, "list": [{"x": 1}, 2], "nested": [[{"y": 1}]]})");
    CHECK(canon(j) == "{\n"
                      "  \"empty\": [],\n"
                      "  \"list\": [\n"
                      "    {\n"
                      "      \"x\": 1\n"
                      "    },\n"
                      "    2\n"
                      "  ],\n"
                      "  \"names\": [\"a.ldx\", \"b.ldx\"],\n"
                      "  \"nested\": [\n"
                      "    [\n"
                      "      {\n"
                      "        \"y\": 1\n"
                      "      }\n"
                      "    ]\n"
                      "  ],\n"
                      "  \"obj\": {},\n"
                      "  \"sync\": [[0, 1250000], [5, 6]]\n"
                      "}\n");
}

TEST_CASE("canonical json: numbers", "[unit]") {
    CHECK(canon(json(3.0)) == "3\n");
    CHECK(canon(json(-0.0)) == "0\n");
    CHECK(canon(json(0.1)) == "0.1\n");
    CHECK(canon(json(0.1 + 0.2)) == "0.30000000000000004\n");
    CHECK(canon(json(1e300)) == "1e+300\n");
    CHECK(canon(json(-120000)) == "-120000\n");
    CHECK(canon(json(std::numeric_limits<std::uint64_t>::max())) == "18446744073709551615\n");
    CHECK(canon(json(std::numeric_limits<std::int64_t>::min())) == "-9223372036854775808\n");
    // Shortest round trip: every double reads back exactly.
    for (const double d : {1.0 / 3.0, 6.02214076e23, 4.9e-324, 1.7976931348623157e308, 14.7, -3.9}) {
        const std::string text = canon(json(d));
        CHECK(json::parse(text).get<double>() == d);
    }
    const json bad = json::parse(R"({"a": [1, 2]})");
    json withNan = bad;
    withNan["a"][1] = std::numeric_limits<double>::quiet_NaN();
    const auto r = model::detail::canonicalJson(withNan);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Invalid);
    CHECK(r.error().context.detail == "/a/1");
    withNan["a"][1] = std::numeric_limits<double>::infinity();
    CHECK_FALSE(model::detail::compactCanonicalJson(withNan).ok());
}

TEST_CASE("canonical json: strings", "[unit]") {
    CHECK(canon(json("a\"b\\c/d")) == "\"a\\\"b\\\\c/d\"\n");
    CHECK(canon(json(std::string("\b\f\n\r\t\x01\x1f", 7))) == "\"\\b\\f\\n\\r\\t\\u0001\\u001f\"\n");
    CHECK(canon(json("\xC2\xB0"
                     "C \xE2\x86\x92")) == "\"\xC2\xB0"
                                           "C \xE2\x86\x92\"\n"); // UTF-8 as is
    json invalid = json::object();
    invalid["k"] = std::string("a\xFF"
                               "b");
    CHECK(canon(invalid) == "{\n  \"k\": \"a\xEF\xBF\xBD"
                            "b\"\n}\n"); // U+FFFD
    CHECK(canon(json(nullptr)) == "null\n");
    CHECK(canon(json(true)) == "true\n");
}

TEST_CASE("canonical json: compact form", "[unit]") {
    const json j = json::parse(R"({"b": [1, {"c": 2}], "a": "x", "$schema": "s"})");
    const auto r = model::detail::compactCanonicalJson(j);
    REQUIRE(r.ok());
    CHECK(*r == R"({"$schema":"s","a":"x","b":[1,{"c":2}]})");
}

TEST_CASE("opaque json: parse, canonical text and equality", "[unit]") {
    const auto a = OpaqueJson::parse(R"( { "z": 1.0, "a": [ 1, 2 ] } )");
    REQUIRE(a.ok());
    CHECK(a->compact() == R"({"a":[1,2],"z":1})");
    CHECK(a->isObject());
    CHECK_FALSE(a->isNull());
    CHECK(a->canonical() == "{\n  \"a\": [1, 2],\n  \"z\": 1\n}\n");
    const auto b = OpaqueJson::parse(R"({"a":[1,2],"z":1})");
    REQUIRE(b.ok());
    CHECK(*a == *b);
    CHECK(OpaqueJson::parse("[]")->isArray());
    const OpaqueJson none;
    CHECK(none.isNull());
    CHECK(none.canonical() == "null\n");

    const auto bad = OpaqueJson::parse(R"({"a": })");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Format);
    REQUIRE(bad.error().context.offset.has_value());
    CHECK(*bad.error().context.offset >= 6);
    const auto huge = OpaqueJson::parse("1e999");
    CHECK_FALSE(huge.ok());
}

TEST_CASE("canonical json: nesting beyond the limit is an error, not a stack overflow", "[unit]") {
    json deep = json::array();
    for (int i = 0; i < 300; ++i) {
        deep = json::array({std::move(deep)});
    }
    const Result<std::string> pretty = model::detail::canonicalJson(deep);
    const Result<std::string> compact = model::detail::compactCanonicalJson(deep);
    for (const Result<std::string>* r : {&pretty, &compact}) {
        REQUIRE_FALSE(r->ok());
        CHECK(r->error().code == ErrorCode::Invalid);
        CHECK(r->error().message.find("nested") != std::string::npos);
    }
    json fine = json::array();
    for (int i = 0; i < 100; ++i) {
        fine = json::array({std::move(fine)});
    }
    CHECK(model::detail::canonicalJson(fine).ok());
}
