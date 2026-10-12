// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ComponentSpec: the workbook-1 component header of 04 section 7.3 (D-019 unknown types preserved).
#include <esses/project/component_spec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace esses;
using namespace esses::project;

namespace {

constexpr const char* kFull =
    R"({"activeChannel":"Corr Speed","body":{"groups":[{"name":"Speed","traces":[{"channel":"Corr Speed"}]}]},)"
    R"("caption":{"auto":false,"show":true,"text":"Speed trace"},"id":"c1a0b2c3-d4e5-4f60-8172-839405a6b7c8",)"
    R"("legend":{"channels":false,"colour":true},"links":{"cursor":false,"datum":true,"zoom":true},)"
    R"("rect":[0.25,0,0.75,0.5],"type":"time_distance","version":1,"x-i2":{"ModeId":"0"},"z":3})";

} // namespace

TEST_CASE("component spec: the full header round-trips compact and canonical", "[unit]") {
    const auto c = ComponentSpec::parse(kFull);
    REQUIRE(c.ok());
    CHECK(c->id.str() == "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8");
    CHECK(c->type == "time_distance");
    CHECK(c->version == 1);
    CHECK(c->rect == Rect{0.25, 0.0, 0.75, 0.5});
    CHECK(c->z == 3);
    CHECK_FALSE(c->caption.autoText);
    CHECK(c->caption.text == "Speed trace");
    CHECK_FALSE(c->links.cursor);
    CHECK(c->legend.colour);
    CHECK_FALSE(c->legend.channels);
    CHECK(c->activeChannel == "Corr Speed");
    CHECK(c->known);
    CHECK(c->xI2.compact() == R"({"ModeId":"0"})");
    CHECK(c->toJson() == kFull);
    const auto again = ComponentSpec::parse(c->toJson());
    REQUIRE(again.ok());
    CHECK(*again == *c);
}

TEST_CASE("component spec: defaults fill a minimal header, which then writes in full", "[unit]") {
    const auto c =
        ComponentSpec::parse(R"({"id": "C1A0B2C3-D4E5-4F60-8172-839405A6B7C8", "type": "values_list"})");
    REQUIRE(c.ok());
    CHECK(c->rect == Rect{});
    CHECK(c->links == Links{});
    CHECK(c->caption == Caption{});
    CHECK(c->legend == Legend{});
    CHECK(c->body.isNull());
    CHECK(c->xI2.isNull());
    CHECK(c->known);
    CHECK(
        c->toJson() ==
        R"({"activeChannel":"","caption":{"auto":true,"show":true,"text":""},)"
        R"("id":"c1a0b2c3-d4e5-4f60-8172-839405a6b7c8","legend":{"channels":true,"colour":false},)"
        R"("links":{"cursor":true,"datum":true,"zoom":true},"rect":[0,0,1,1],"type":"values_list","version":1,"z":0})");
}

TEST_CASE("component spec: unknown types and newer versions are preserved, not rejected", "[unit]") {
    const std::string unknown =
        R"({"activeChannel":"","body":{"lambdaDelayUs":150000,"x":"Engine RPM","y":"Lambda 1","zz":[1,{"a":null}]},)"
        R"("caption":{"auto":true,"show":true,"text":""},"id":"c3c2d4e5-f607-4182-9394-a5b6c7d8e9fa",)"
        R"("legend":{"channels":true,"colour":false},"links":{"cursor":true,"datum":true,"zoom":true},)"
        R"("rect":[0.75,0.5,1,1],"type":"mixture_map","version":3,"x-i2":{"Weird":{"Nested":["a","b"]}},"z":2})";
    const auto c = ComponentSpec::parse(unknown);
    REQUIRE(c.ok());
    CHECK_FALSE(c->known);
    CHECK(c->toJson() == unknown);

    const auto newer =
        ComponentSpec::parse(R"({"id": "c3c2d4e5-f607-4182-9394-a5b6c7d8e9fa", "type": "histogram",
                                               "version": 2})");
    REQUIRE(newer.ok());
    CHECK_FALSE(newer->known);
    CHECK(isKnownComponentType("histogram"));
    CHECK(supportedComponentVersion("histogram") == 1);
    CHECK_FALSE(isKnownComponentType("mixture_map"));
    CHECK(supportedComponentVersion("mixture_map") == 0);
    for (const char* t : {"time_distance", "outing", "scatter", "histogram", "channel_report", "time_report",
                          "track_position", "values_list", "gauge_numeric", "gauge_bar"}) {
        CHECK(isKnownComponentType(t));
    }
}

TEST_CASE("component spec: malformed headers are Invalid at a JSON pointer", "[unit]") {
    struct Case {
        const char* json;
        const char* pointer;
    };
    const Case cases[] = {
        {R"({"type": "time_distance"})", ""},                                       // no id
        {R"({"id": "c1a0", "type": "time_distance"})", "/id"},                      // bad id
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8"})", ""},                  // no type
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": ""})", "/type"}, // empty type
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "rect": [0, 0, 1]})", "/rect"},
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "rect": [0, 0, 1.5, 1]})", "/rect/2"},
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "rect": [0.5, 0, 0.5, 1]})", "/rect"},
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "version": 0})", "/version"},
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "z": 1.5})", "/z"},
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "body": [1]})", "/body"},
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "x-i2": "a"})", "/x-i2"},
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "caption": {"bold": true}})",
         "/caption/bold"},
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "links": {"zoom": 1}})",
         "/links/zoom"},
        {R"({"id": "c1a0b2c3-d4e5-4f60-8172-839405a6b7c8", "type": "x", "colour": "red"})", "/colour"},
    };
    for (const Case& c : cases) {
        INFO(c.json);
        const auto r = ComponentSpec::parse(c.json);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        CHECK(r.error().context.detail == c.pointer);
    }
    const auto syntax = ComponentSpec::parse("{\"id\": ");
    REQUIRE_FALSE(syntax.ok());
    CHECK(syntax.error().code == ErrorCode::Format);
}
