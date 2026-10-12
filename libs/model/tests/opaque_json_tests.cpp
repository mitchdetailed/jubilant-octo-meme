// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// model::OpaqueJson is esses_base's JsonDoc (the canonical writer and its tests live in esses_base,
// libs/base/tests/json_doc_tests.cpp); these cases pin the behaviour the model's documents rely on.
#include <esses/base/json_doc.hpp>
#include <esses/model/opaque_json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>

using namespace esses;
using namespace esses::model;

TEST_CASE("opaque json: an alias of JsonDoc with the canonical text", "[unit]") {
    STATIC_REQUIRE(std::is_same_v<OpaqueJson, JsonDoc>);
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
    CHECK_FALSE(OpaqueJson::parse("1e999").ok());
}
