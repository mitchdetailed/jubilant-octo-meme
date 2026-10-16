// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// JsonDoc, the canonical JSON form of 02 section 6.5 rule 1 and the nlohmann-json interop
// (json_nlohmann.hpp).
#include "test_support.hpp"

#include <esses/base/json_doc.hpp>
#include <esses/base/json_nlohmann.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

using namespace esses;
using nlohmann::json;

namespace {

std::string canon(const json& j) {
    auto r = jsonio::canonicalJson(j);
    REQUIRE(r.ok());
    return *r;
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("canonical json: member order puts $schema and schemaVersion first, then code-unit order",
          "[unit][json]") {
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
    CHECK(jsonio::canonicalKeyLess("$schema", "schemaVersion"));
    CHECK(jsonio::canonicalKeyLess("schemaVersion", "$a"));
    CHECK(jsonio::canonicalKeyLess("Z", "a"));
    CHECK_FALSE(jsonio::canonicalKeyLess("a", "a"));
}

TEST_CASE("canonical json: arrays are inline unless they hold an object", "[unit][json]") {
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

TEST_CASE("canonical json: numbers", "[unit][json]") {
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
    const auto r = jsonio::canonicalJson(withNan);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Invalid);
    CHECK(r.error().context.detail == "/a/1");
    withNan["a"][1] = std::numeric_limits<double>::infinity();
    CHECK_FALSE(jsonio::compactCanonicalJson(withNan).ok());
}

TEST_CASE("canonical json: strings", "[unit][json]") {
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

TEST_CASE("canonical json: compact form", "[unit][json]") {
    const json j = json::parse(R"({"b": [1, {"c": 2}], "a": "x", "$schema": "s"})");
    const auto r = jsonio::compactCanonicalJson(j);
    REQUIRE(r.ok());
    CHECK(*r == R"({"$schema":"s","a":"x","b":[1,{"c":2}]})");
}

TEST_CASE("canonical json: nesting beyond the limit is an error, not a stack overflow", "[unit][json]") {
    json deep = json::array();
    for (int i = 0; i < 300; ++i) {
        deep = json::array({std::move(deep)});
    }
    const Result<std::string> pretty = jsonio::canonicalJson(deep);
    const Result<std::string> compact = jsonio::compactCanonicalJson(deep);
    for (const Result<std::string>* r : {&pretty, &compact}) {
        REQUIRE_FALSE(r->ok());
        CHECK(r->error().code == ErrorCode::Invalid);
        CHECK(r->error().message.find("nested") != std::string::npos);
    }
    json fine = json::array();
    for (int i = 0; i < 100; ++i) {
        fine = json::array({std::move(fine)});
    }
    CHECK(jsonio::canonicalJson(fine).ok());
    // The text entry points refuse the same depth before parsing.
    const auto refused = JsonDoc::parse(std::string(300, '[') + std::string(300, ']'));
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::Format);
}

TEST_CASE("json doc: parse, canonical text and equality", "[unit][json]") {
    const auto a = JsonDoc::parse(R"( { "z": 1.0, "a": [ 1, 2 ] } )");
    REQUIRE(a.ok());
    CHECK(a->compact() == R"({"a":[1,2],"z":1})");
    CHECK(a->isObject());
    CHECK_FALSE(a->isNull());
    CHECK_FALSE(a->isArray());
    CHECK(a->canonical() == "{\n  \"a\": [1, 2],\n  \"z\": 1\n}\n");
    const auto b = JsonDoc::parse(R"({"a":[1,2],"z":1})");
    REQUIRE(b.ok());
    CHECK(*a == *b);
    CHECK(JsonDoc::parse("[]")->isArray());
    CHECK(JsonDoc::parse("\"x\"")->isString());
    CHECK(JsonDoc::parse("null")->isNull());
    const JsonDoc none;
    CHECK(none.isNull());
    CHECK(none.compact().empty());
    CHECK(none.canonical() == "null\n");
    CHECK(none.schemaId().empty());
    CHECK(none.schemaVersion() == 0);

    const auto bad = JsonDoc::parse(R"({"a": })");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Format);
    REQUIRE(bad.error().context.offset.has_value());
    CHECK(*bad.error().context.offset >= 6);
    CHECK_FALSE(JsonDoc::parse("1e999").ok());
    // A UTF-8 byte order mark is skipped.
    const auto bom = JsonDoc::parse("\xEF\xBB\xBF{\"k\": true}");
    REQUIRE(bom.ok());
    CHECK(bom->compact() == R"({"k":true})");
}

TEST_CASE("json doc: schema id and version", "[unit][json]") {
    const auto doc = JsonDoc::parse(R"({"schemaVersion": 3, "x": 1, "$schema": "urn:esses:thing-2"})");
    REQUIRE(doc.ok());
    CHECK(doc->schemaId() == "urn:esses:thing-2");
    CHECK(doc->schemaVersion() == 3);
    CHECK(doc->compact() == R"({"$schema":"urn:esses:thing-2","schemaVersion":3,"x":1})");
    const auto noSchema = JsonDoc::parse(R"({"schemaVersion": "1"})");
    REQUIRE(noSchema.ok());
    CHECK(noSchema->schemaId().empty());
    CHECK(noSchema->schemaVersion() == 0);
    CHECK(JsonDoc::parse("[1]")->schemaId().empty());
}

TEST_CASE("json doc: load and saveAtomic round-trip the canonical bytes", "[unit][json]") {
    test::TempDir dir("jsondoc");
    const auto path = dir.path() / "doc.json";
    const auto doc = JsonDoc::parse(R"({"b": [{"c": 1}], "$schema": "urn:esses:x-1", "a": "é"})");
    REQUIRE(doc.ok());
    REQUIRE(doc->saveAtomic(path).ok());
    const std::string bytes = readAll(path);
    CHECK(bytes == doc->canonical());
    CHECK(bytes == "{\n  \"$schema\": \"urn:esses:x-1\",\n  \"a\": \"\xC3\xA9\",\n  \"b\": [\n    {\n"
                   "      \"c\": 1\n    }\n  ]\n}\n");
    const auto loaded = JsonDoc::load(path);
    REQUIRE(loaded.ok());
    CHECK(*loaded == *doc);
    CHECK(loaded->canonical() == bytes);

    const auto missing = JsonDoc::load(dir.path() / "absent.json");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::Io);
    CHECK(missing.error().context.path == dir.path() / "absent.json");

    {
        std::ofstream out(dir.path() / "broken.json", std::ios::binary);
        out << "{\"a\": [1,}";
    }
    const auto broken = JsonDoc::load(dir.path() / "broken.json");
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().code == ErrorCode::Format);
    CHECK(broken.error().context.path == dir.path() / "broken.json");
    CHECK(broken.error().context.offset.has_value());
}

TEST_CASE("json interop: toDoc and fromDoc", "[unit][json]") {
    const json value = json::parse(R"({"k": [1, 2.5, "s", null, false]})");
    const auto doc = jsonio::toDoc(value);
    REQUIRE(doc.ok());
    CHECK(jsonio::fromDoc(*doc) == value);
    CHECK(jsonio::fromDoc(JsonDoc{}).is_null());
    json nan = json::object();
    nan["x"] = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(jsonio::toDoc(nan).ok());
}

TEST_CASE("json interop: typed readers report JSON pointers", "[unit][json]") {
    const json j = json::parse(R"({"s": "x", "i": 7, "big": 4294967296, "neg": -1, "d": 2.5, "b": true,
                                   "u": "7C0E9A2B-1D3F-5A6C-8E9F-0A1B2C3D4E5F", "n": null, "o": {"a~b/c": 1}})");
    const Result<int> ok = jsonio::guardJson([&]() -> Result<int> {
        jsonio::ObjectReader r(j, "");
        CHECK(jsonio::asString(r.required("s"), r.child("s")) == "x");
        CHECK(jsonio::asInt32(r.required("i"), r.child("i")) == 7);
        CHECK(jsonio::asU64(r.required("big"), r.child("big")) == 4294967296ULL);
        CHECK(jsonio::asNumber(r.required("d"), r.child("d")) == 2.5);
        CHECK(jsonio::asNumber(r.required("i"), r.child("i")) == 7.0);
        CHECK(jsonio::asBool(r.required("b"), r.child("b")));
        CHECK(jsonio::asUuid(r.required("u"), r.child("u")).str() == "7c0e9a2b-1d3f-5a6c-8e9f-0a1b2c3d4e5f");
        CHECK(r.optional("n") == nullptr);
        CHECK(r.optional("absent") == nullptr);
        r.skip("neg");
        const json rest = r.unread();
        CHECK(rest == json::parse(R"({"o": {"a~b/c": 1}})"));
        CHECK(jsonio::pointerChild("/o", "a~b/c") == "/o/a~0b~1c");
        CHECK(jsonio::pointerIndex("/list", 3) == "/list/3");
        return 1;
    });
    CHECK(ok.ok());

    const auto failAt = [&](auto&& fn) {
        const Result<int> r = jsonio::guardJson([&]() -> Result<int> {
            fn();
            return 0;
        });
        REQUIRE_FALSE(r.ok());
        return r.error();
    };
    CHECK(failAt([&] { (void)jsonio::asU32(j.at("big"), "/big"); }).context.detail == "/big");
    CHECK(failAt([&] { (void)jsonio::asU64(j.at("neg"), "/neg"); }).context.detail == "/neg");
    CHECK(failAt([&] { (void)jsonio::asInt32(j.at("big"), "/big"); }).message.find("32-bit") !=
          std::string::npos);
    CHECK(failAt([&] { (void)jsonio::asString(j.at("i"), "/i"); }).message ==
          "expected a string, found an integer (at '/i')");
    CHECK(failAt([&] { (void)jsonio::asUuid(j.at("s"), "/s"); }).code == ErrorCode::Invalid);
    const Error missing = failAt([&] {
        jsonio::ObjectReader r(j, "");
        (void)r.required("zzz");
    });
    CHECK(missing.message == "required member 'zzz' is missing (at document root)");
    const Error closed = failAt([&] {
        jsonio::ObjectReader r(j.at("o"), "/o");
        r.closed();
    });
    CHECK(closed.context.detail == "/o/a~0b~1c");
}

TEST_CASE("json interop: schema ids in every accepted form", "[unit][json]") {
    const auto check = [](std::string_view id) {
        json j = json::object();
        j["$schema"] = std::string(id);
        return jsonio::guardJson([&]() -> Result<int> {
            jsonio::checkSchemaId(j, "workbook", 1);
            return 1;
        });
    };
    CHECK(check("urn:esses:workbook-1").ok());
    CHECK(check("esses:workbook-1").ok());
    CHECK(check("https://example.org/schemas/workbook-1.schema.json").ok());
    CHECK(check("https://example.org/workbook-1.json").ok());
    const auto other = check("urn:esses:project-1");
    REQUIRE_FALSE(other.ok());
    CHECK(other.error().code == ErrorCode::Invalid);
    CHECK(other.error().context.detail == "/$schema");
    const auto newer = check("urn:esses:workbook-2");
    REQUIRE_FALSE(newer.ok());
    CHECK(newer.error().code == ErrorCode::Unsupported);
    CHECK(newer.error().context.schemaId == "urn:esses:workbook-2");
    CHECK(check("workbook-1").error().code == ErrorCode::Invalid);
    const auto noSchema = jsonio::guardJson([]() -> Result<int> {
        jsonio::checkSchemaId(json::object(), "workbook", 1);
        return 1;
    });
    CHECK_FALSE(noSchema.ok());
}

TEST_CASE("json interop: readFile", "[unit][json]") {
    test::TempDir dir("jsonread");
    const auto path = dir.path() / "t.txt";
    {
        std::ofstream out(path, std::ios::binary);
        out << "a\r\nb";
    }
    const auto text = jsonio::readFile(path);
    REQUIRE(text.ok());
    CHECK(*text == "a\r\nb");
    CHECK(jsonio::readFile(dir.path() / "none.txt").error().code == ErrorCode::Io);
}

TEST_CASE("json interop: readFile of a directory is an Io error, never an exception", "[unit][json]") {
    // A directory opens as a file on POSIX and its read fails (EISDIR); libstdc++ throws that from the
    // stream buffer whatever the stream's exception mask. Windows refuses the open. Io either way.
    test::TempDir dir("jsonreaddir");
    const auto folder = dir.path() / "document.json";
    std::filesystem::create_directories(folder);
    Result<std::string> text = fail(ErrorCode::Internal, "not called");
    REQUIRE_NOTHROW(text = jsonio::readFile(folder));
    REQUIRE_FALSE(text.ok());
    CHECK(text.error().code == ErrorCode::Io);
    CHECK(text.error().context.path == folder);
    Result<JsonDoc> doc = fail(ErrorCode::Internal, "not called");
    REQUIRE_NOTHROW(doc = JsonDoc::load(folder));
    REQUIRE_FALSE(doc.ok());
    CHECK(doc.error().code == ErrorCode::Io);
}
