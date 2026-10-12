// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The time_distance component: the body record (04 section 8.14) and the behaviour of its three builders,
// plus the DrawList JSON goldens of the eight M1 worksheet cases at DPR 1 and 2 (AC-M1-04, 08 section 6.4).
// Goldens live in tests/golden/drawlist/time_distance/<case>@<dpr>x.json; ESSES_UPDATE_GOLDENS=1 rewrites
// them (they are platform independent, so a local update is valid); a mismatch writes
// <case>@<dpr>x.actual.json next to the test binary.

#include "td_golden_cases.hpp"

#include <esses/base/env.hpp>
#include <esses/plotcore/time_distance_builder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace esses;
using namespace esses::plot;

namespace {

std::filesystem::path goldenDir() {
    return std::filesystem::path(ESSES_TEST_REPO_ROOT) / "tests" / "golden" / "drawlist" / "time_distance";
}

std::string readText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c != '\r') {
            out += c;
        }
    }
    return out;
}

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string goldenText(const std::string& name, float dpr, const ComponentBuild& build,
                       const DrawList& dynamic, const DrawList& chrome) {
    std::string s = "{\"case\":\"" + name + "\",\"dpr\":" + std::to_string(static_cast<int>(dpr)) + ",\n";
    s += "\"static\":" + build.list.toJson();
    s += ",\"dynamic\":" + dynamic.toJson();
    s += ",\"chrome\":" + chrome.toJson();
    s += "}\n";
    return s;
}

std::size_t countKind(const DrawList& list, CmdKind kind) {
    std::size_t n = 0;
    for (const DrawCmd& c : list.cmds) {
        n += c.kind == kind ? 1 : 0;
    }
    return n;
}

bool hasText(const DrawList& list, std::string_view needle) {
    for (const TextRun& r : list.texts) {
        if (list.textOf(r).find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("TimeDistanceBody: parse, typed members and a lossless round trip", "[unit]") {
    const auto src = model::OpaqueJson::parse(
        R"({"display":{"traceStyle":"line","lineWidth":2,"showGrid":false,"bandMode":"sections",)"
        R"("key":{"mode":"labels","pos":"right","values":true,"stats":false},"axis":{"showX":true,"showY":false}},)"
        R"("groups":[{"name":"Speed","scaleMode":"channel","edges":[0,0.33],"lo":0,"hi":0.33,)"
        R"("lines":{"zero":{"enabled":false},"min":{"enabled":false}},)"
        R"("traces":[{"channel":"Corr Speed","visible":true,"displayUnit":"km/h","dps":1},{"channel":"GPS Speed"}]},)"
        R"({"name":"Throttle/Brake","scaleMode":"manual","scaleMin":{"v":0,"unit":"%"},"scaleMax":{"v":100,"unit":"%"},)"
        R"("edges":[0.33,0.66],"traces":[{"channel":"Throttle Pos"},{"channel":"Brake Pres","displayUnit":"bar"}]}],)"
        R"("x-unknown":{"keep":[1,2,3]}})");
    REQUIRE(src.ok());
    const Result<TimeDistanceBody> body = TimeDistanceBody::parse(*src);
    REQUIRE(body.ok());
    CHECK(body->lineWidth == 2.0F);
    CHECK_FALSE(body->showGrid);
    CHECK(body->keyPos == "right");
    CHECK_FALSE(body->showYAxis);
    REQUIRE(body->groups.size() == 2);
    CHECK(body->groups[0].edgeBottom == 0.33);
    CHECK_FALSE(body->groups[0].zeroLine);
    CHECK(body->groups[0].traces[0].dps == 1);
    CHECK(body->groups[1].scaleMode == ScaleMode::Manual);
    CHECK(body->groups[1].scaleMin == 0.0);
    CHECK(body->groups[1].scaleMax == 100.0);
    CHECK(body->groups[1].scaleUnit == "%");
    CHECK(body->channelNames() ==
          std::vector<std::string>{"Corr Speed", "GPS Speed", "Throttle Pos", "Brake Pres"});
    CHECK(body->toJson() == *src); // unknown members and untouched defaults survive

    TimeDistanceBody edited = *body;
    edited.groups[0].traces[1].visible = false;
    edited.lineWidth = 1.0F;
    TdGroup added;
    added.name = "Gear";
    added.traces.push_back(TdTrace{"Gear", true, "", std::nullopt});
    edited.groups.push_back(added);
    const Result<TimeDistanceBody> again = TimeDistanceBody::parse(edited.toJson());
    REQUIRE(again.ok());
    CHECK_FALSE(again->groups[0].traces[1].visible);
    CHECK(again->groups.size() == 3);
    CHECK(again->groups[2].traces[0].channel == "Gear");
    CHECK(again->lineWidth == 1.0F);
    CHECK(again->toJson().compact().find("x-unknown") != std::string::npos);

    CHECK_FALSE(TimeDistanceBody::parse(*model::OpaqueJson::parse(R"({"groups":{}})")).ok());
    const auto bad =
        TimeDistanceBody::parse(*model::OpaqueJson::parse(R"({"groups":[{"traces":[{"dps":"x"}]}]})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().context.detail == "/groups/0/traces/0/dps");
    CHECK(TimeDistanceBody::parse(model::OpaqueJson{})->groups.empty());
}

TEST_CASE("TimeDistanceBuilder: layout, draw order, colours and hit regions", "[unit]") {
    const test::Fixture fx;
    const auto cases = test::goldenCases(fx);
    const test::GoldenCase& overlays = cases[6]; // time_overlays
    REQUIRE(overlays.name == "time_overlays");
    const ComponentBuildInputs in = test::inputsOf(fx, overlays, 1.0F);
    const TdLayout L = TimeDistanceBuilder::layoutOf(in);
    REQUIRE(L.groups.size() == 2);
    CHECK(L.caption.h == 18.0F);
    CHECK(L.groups[0].panel.y == L.traceArea.y);
    CHECK(L.groups[1].panel.y + L.groups[1].panel.h == L.traceArea.y + L.traceArea.h);
    CHECK(L.keyColumn.x == 0.0F);
    CHECK(L.traceArea.x == L.keyColumn.w + 36.0F);
    CHECK(L.groups[0].rows.size() == 1);
    CHECK(L.groups[0].rows[0].slotValues.size() == 4); // one value line per visible slot

    const Result<ComponentBuild> build = TimeDistanceBuilder::buildStatic(in, CancelToken{});
    REQUIRE(build.ok());
    const DrawList& list = build->list;
    // Overlays n..1, Overlay 0, Main: the Main (single colour) polyline is the last trace of the Speed panel.
    std::vector<Rgba> speedTraceColours;
    bool inSpeed = false;
    for (const DrawCmd& c : list.cmds) {
        if (c.kind == CmdKind::Scissor) {
            inSpeed = c.rect == L.groups[0].panel;
            continue;
        }
        if (inSpeed && c.kind == CmdKind::Polyline &&
            (speedTraceColours.empty() || speedTraceColours.back() != c.colour)) {
            speedTraceColours.push_back(c.colour);
        }
    }
    const project::Scheme scheme = test::lightScheme();
    // Overlays 2 and 1 share the plain channel colour (D-100), then Overlay 0, then the single-colour Main.
    REQUIRE(speedTraceColours.size() == 3);
    CHECK(speedTraceColours[0] == scheme.channelColour(0));
    CHECK(speedTraceColours[1] == scheme.role("overlay0"));
    CHECK(speedTraceColours[2] == scheme.role("mainSingleColour"));
    CHECK(hasText(list, "Corr Speed [km/h]"));
    CHECK(hasText(list, "0:40"));

    const HitResult panel = build->hits.hit(L.groups[1].panel.x + 5.0F, L.groups[1].panel.y + 5.0F);
    CHECK(panel.kind == HitKind::Panel);
    CHECK(panel.group == 1);
    const RectPx row = L.groups[0].keyGeometry.rowRects[0];
    const HitResult label = build->hits.hit(L.groups[0].key.x + 12.0F, L.groups[0].key.y + row.y + 2.0F);
    CHECK(label.kind == HitKind::KeyLabel);
    CHECK(label.trace == 0);
    CHECK(build->hits.hit(L.xAxis.x + 3.0F, L.xAxis.y + 3.0F).kind == HitKind::Axis);
}

TEST_CASE("TimeDistanceBuilder: Dynamic cursor and values, pending and missing", "[unit]") {
    const test::Fixture fx;
    const auto cases = test::goldenCases(fx);
    ComponentBuildInputs in = test::inputsOf(fx, cases[0], 1.0F); // single_trace, cursor at 20 s
    // Before any Static build the chunks are not resident: values show the pending ellipsis.
    const DrawList before = TimeDistanceBuilder::buildDynamic(in);
    CHECK(hasText(before, "\xE2\x80\xA6"));
    REQUIRE(TimeDistanceBuilder::buildStatic(in, CancelToken{}).ok());
    const DrawList dyn = TimeDistanceBuilder::buildDynamic(in);
    const TdLayout L = TimeDistanceBuilder::layoutOf(in);
    REQUIRE(countKind(dyn, CmdKind::Lines) == 1);
    const DrawCmd& cursor = dyn.cmds[0];
    const float x = dyn.verts[cursor.vertexOffset * 2];
    CHECK(x == std::round(L.traceArea.x + static_cast<float>(L.traceWidthPx) / 3.0F) + 0.5F);
    // The value at 20 s of lap 1 (phase 1/3 of the speed profile), in km/h.
    const double ms = test::lapSpeed(20.0 / 60.0, 0.0);
    CHECK(hasText(dyn, std::to_string(static_cast<int>(std::floor(ms * 3.6)))));

    // A Pending channel shows the ellipsis and the Chrome pending stripe.
    in.slots[0].channels[0].status = model::ChannelStatus::Pending;
    CHECK(hasText(TimeDistanceBuilder::buildDynamic(in), "\xE2\x80\xA6"));
    const DrawList chrome = TimeDistanceBuilder::buildChrome(in, false);
    CHECK(countKind(chrome, CmdKind::FilledRect) >= 2); // caption + stripe (+ active square)

    // An overlay whose source lacks the channel says so in its value line (04 section 8.4).
    ComponentBuildInputs overlay = test::inputsOf(fx, cases[4], 1.0F); // distance_overlay: B lap 2 overlay
    overlay.slots[1].channels[1].descriptor.reset();
    overlay.slots[1].channels[1].series.reset();
    CHECK(hasText(TimeDistanceBuilder::buildDynamic(overlay), "(not in Lap 2)"));

    // A channel missing in the Main source is labelled "(missing)" and draws nothing.
    ComponentBuildInputs missing = test::inputsOf(fx, cases[0], 1.0F);
    missing.slots[0].channels[0].descriptor.reset();
    missing.slots[0].channels[0].series.reset();
    const Result<ComponentBuild> b = TimeDistanceBuilder::buildStatic(missing, CancelToken{});
    REQUIRE(b.ok());
    CHECK(hasText(b->list, "Corr Speed (missing)"));
    CHECK(countKind(b->list, CmdKind::Polyline) == 1); // the frame only
}

TEST_CASE("TimeDistanceBuilder: distance mode banner and cancellation", "[unit]") {
    const test::Fixture fx;
    const auto cases = test::goldenCases(fx);
    const ComponentBuildInputs in = test::inputsOf(fx, cases[7], 1.0F); // no_distance_banner
    const DrawList chrome = TimeDistanceBuilder::buildChrome(in, false);
    CHECK(hasText(chrome, "No distance data for synthetic_c.ld"));
    CHECK_FALSE(hasText(chrome, "synthetic_a.ld"));
    const DrawList time = TimeDistanceBuilder::buildChrome(test::inputsOf(fx, cases[0], 1.0F), false);
    CHECK_FALSE(hasText(time, "No distance data"));

    CancelSource cancel;
    cancel.cancel();
    const Result<ComponentBuild> r = TimeDistanceBuilder::buildStatic(in, cancel.token());
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Cancelled);
}

TEST_CASE("TimeDistanceBuilder: DrawList JSON goldens of the eight M1 worksheet cases (AC-M1-04)", "[unit]") {
    const test::Fixture fx;
    const bool update = getEnv(envName("UPDATE_GOLDENS")).value_or("") == "1";
    for (const test::GoldenCase& gc : test::goldenCases(fx)) {
        for (const float dpr : {1.0F, 2.0F}) {
            const ComponentBuildInputs in = test::inputsOf(fx, gc, dpr);
            const Result<ComponentBuild> build = TimeDistanceBuilder::buildStatic(in, CancelToken{});
            REQUIRE(build.ok());
            const DrawList dynamic = TimeDistanceBuilder::buildDynamic(in);
            const DrawList chrome = TimeDistanceBuilder::buildChrome(in, gc.pending);
            const std::string actual = goldenText(gc.name, dpr, *build, dynamic, chrome);
            const std::string file = gc.name + "@" + std::to_string(static_cast<int>(dpr)) + "x.json";
            const std::filesystem::path path = goldenDir() / file;
            if (update) {
                writeText(path, actual);
                continue;
            }
            const std::string expected = readText(path);
            if (expected != actual) {
                const std::filesystem::path out =
                    std::filesystem::path(ESSES_TEST_BINARY_DIR) /
                    (gc.name + "@" + std::to_string(static_cast<int>(dpr)) + "x.actual.json");
                writeText(out, actual);
                FAIL_CHECK("DrawList golden " << file << " differs; actual written to " << out.string()
                                              << (expected.empty() ? " (golden missing)" : ""));
            }
        }
    }
}
