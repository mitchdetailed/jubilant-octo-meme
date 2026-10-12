// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Scheme (scheme-1; 06 section 16, D-017) and the shipped light and dark schemes.
#include "test_support.hpp"

#include <esses/project/scheme.hpp>
#include <esses/project/shipped_data.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

namespace {

// Relative luminance (sRGB, 0..1) for the contrast checks.
double luminance(Rgba c) {
    const auto lin = [](std::uint8_t v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(red(c)) + 0.7152 * lin(green(c)) + 0.0722 * lin(blue(c));
}

double contrast(Rgba a, Rgba b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace

TEST_CASE("scheme: the shipped light and dark schemes round-trip byte for byte and are complete", "[unit]") {
    for (const std::string_view rel : {kLightScheme, kDarkScheme}) {
        INFO(rel);
        const auto path = pt::dataFile(rel);
        std::vector<std::string> log;
        const auto s = Scheme::load(path, log);
        REQUIRE(s.ok());
        CHECK(log.empty());
        CHECK(s->toJson() == pt::readAll(path));
        CHECK(pt::schemaErrors(s->toJson(), "scheme-1").empty());
        for (const std::string_view role : kSchemeRoles) {
            INFO(role);
            CHECK(s->roles.count(role) == 1);
        }
        CHECK(s->roles.size() == kSchemeRoles.size());
        for (const std::string_view font : kSchemeFontRoles) {
            INFO(font);
            CHECK(s->fonts.count(font) == 1);
            CHECK(s->font(font).family ==
                  "Noto Sans"); // golden images load exactly this family (04 section 6.7)
        }
        CHECK(s->font("keyStrikeout").strikeOut);
        CHECK_FALSE(s->font("key").strikeOut);
        CHECK(s->channelPalette.size() >= 10);
        CHECK(s->overlayPalette.size() == 9);
        CHECK(std::set<Rgba>(s->channelPalette.begin(), s->channelPalette.end()).size() ==
              s->channelPalette.size());
        CHECK(s->overlayStyle == OverlayStyle{}); // D-100: plain channel colours by default
        CHECK_FALSE(s->print.isNull());
        // Every channel colour, the Main cursor and Overlay 0 stand out from the component background.
        const Rgba bg = s->role("cptBackground");
        for (const Rgba c : s->channelPalette) {
            CHECK(contrast(c, bg) >= 3.0);
        }
        for (const Rgba c : s->overlayPalette) {
            CHECK(contrast(c, bg) >= 3.0);
        }
        CHECK(contrast(s->role("cursorMain"), bg) >= 3.0);
        CHECK(contrast(s->role("overlay0"), bg) >= 3.0);
        CHECK(contrast(s->role("mainSingleColour"), bg) >= 7.0);
        CHECK(contrast(s->role("cptForeground"), bg) >= 7.0);
    }
    std::vector<std::string> log;
    const auto light = Scheme::load(pt::dataFile(kLightScheme), log);
    const auto dark = Scheme::load(pt::dataFile(kDarkScheme), log);
    REQUIRE(light.ok());
    REQUIRE(dark.ok());
    CHECK(light->background == "light");
    CHECK(dark->background == "dark");
    CHECK(light->name != dark->name);
    CHECK(luminance(light->role("cptBackground")) > luminance(dark->role("cptBackground")));
    CHECK(light->channelPalette != dark->channelPalette); // a palette per background (06 section 16 rule 1)
}

TEST_CASE("scheme: lookups, overlay colours and fallbacks", "[unit]") {
    Scheme s;
    CHECK(s.role("cursorMain", 0x11223344U) == 0x11223344U);
    CHECK(s.channelColour(3) == 0x808080FFU);
    CHECK(s.overlayColour(1) == 0x808080FFU);
    s.channelPalette = {rgba(1, 2, 3), rgba(4, 5, 6)};
    CHECK(s.channelColour(3) == rgba(4, 5, 6));
    CHECK(s.channelColour(-1, 7U) == 7U);
    s.overlayPalette = {rgba(1, 1, 1), rgba(2, 2, 2), rgba(3, 3, 3), rgba(4, 4, 4), rgba(5, 5, 5),
                        rgba(6, 6, 6), rgba(7, 7, 7), rgba(8, 8, 8), rgba(9, 9, 9)};
    CHECK(s.overlayColour(1) == rgba(1, 1, 1));
    CHECK(s.overlayColour(9) == rgba(9, 9, 9));
    CHECK(s.overlayColour(10) == rgba(1, 1, 1)); // cycling
    CHECK(s.overlayColour(0, 5U) == 5U);
    CHECK(s.font("axis") == FontSpec{});
    s.fonts["caption"] = FontSpec{"Noto Sans", 9.0, 600, false, false};
    CHECK(s.font("axis").weight == 600);

    std::vector<std::string> log;
    const auto shipped = Scheme::load(pt::dataFile(kDarkScheme), log);
    REQUIRE(shipped.ok());
    auto user = Scheme::parse(pt::readAll(pt::repoRoot() / "schemas/examples/scheme-1/valid/user.json"), log);
    REQUIRE(user.ok());
    CHECK(user->role("cursorMain") == rgba(0xFF, 0x20, 0x20));
    CHECK(user->role("overlay0") == rgba(0xBD, 0xBD, 0xBD, 0x80));
    const std::size_t had = user->roles.size();
    const auto filled = user->fillMissingFrom(*shipped);
    CHECK(user->roles.size() == kSchemeRoles.size());
    CHECK(user->role("cursorMain") == rgba(0xFF, 0x20, 0x20)); // its own roles win
    CHECK(user->channelPalette == shipped->channelPalette);
    CHECK(user->overlayPalette == shipped->overlayPalette);
    CHECK(user->fonts.size() == kSchemeFontRoles.size());
    CHECK(user->font("caption").weight == 700);
    // One line per filled role and font, plus the two palettes.
    CHECK(filled.size() == (kSchemeRoles.size() - had) + (kSchemeFontRoles.size() - 2) + 2);
    CHECK(user->fillMissingFrom(*shipped).empty());
}

TEST_CASE("scheme: colours are written lower case, unknown members survive", "[unit]") {
    std::vector<std::string> log;
    const auto user =
        Scheme::parse(pt::readAll(pt::repoRoot() / "schemas/examples/scheme-1/valid/user.json"), log);
    REQUIRE(user.ok());
    const std::string out = user->toJson();
    CHECK(out.find("\"cursorMain\": \"#ff2020\"") != std::string::npos);
    CHECK(out.find("\"overlay0\": \"#bdbdbd80\"") != std::string::npos);
    CHECK(out.find("\"strikeOut\": true") != std::string::npos);
    CHECK(out.find("\"strikeOut\": false") == std::string::npos);
    CHECK(pt::schemaErrors(out, "scheme-1").empty());
    const auto again = Scheme::parse(out, log);
    REQUIRE(again.ok());
    CHECK(*again == *user);
    CHECK(again->toJson() == out);

    const auto palettes =
        Scheme::parse(pt::readAll(pt::repoRoot() / "schemas/examples/scheme-1/valid/palettes.json"), log);
    REQUIRE(palettes.ok());
    CHECK(palettes->unknown.compact() == R"({"futureMember":"kept"})");
    CHECK(palettes->toJson().find("\"futureMember\": \"kept\"") != std::string::npos);
}

TEST_CASE("scheme: malformed schemes are Invalid at a JSON pointer", "[unit]") {
    struct Case {
        std::string json;
        std::string pointer;
    };
    const std::string head = R"({"$schema": "urn:esses:scheme-1", "schemaVersion": 1, "name": "S", )";
    const std::string ten = R"(["#000001", "#000002", "#000003", "#000004", "#000005", "#000006", "#000007",
                                "#000008", "#000009", "#00000a"])";
    const std::vector<Case> cases = {
        {head + R"("background": "grey"})", "/background"},
        {R"({"$schema": "urn:esses:scheme-1", "schemaVersion": 1, "background": "dark"})", ""},
        {head + R"("background": "dark", "roles": {"cursorMain": "red"}})", "/roles/cursorMain"},
        {head + R"("background": "dark", "roles": {"cursor main": "#ff0000"}})", "/roles/cursor main"},
        {head + R"("background": "dark", "channelPalette": ["#ff0000"]})", "/channelPalette"},
        {head + R"("background": "dark", "overlayPalette": )" + ten + "}", "/overlayPalette"},
        {head + R"("background": "dark", "overlayStyle": {"dash": "zigzag"}})", "/overlayStyle/dash"},
        {head + R"("background": "dark", "overlayStyle": {"lightness": 2}})", "/overlayStyle/lightness"},
        {head + R"("background": "dark", "overlayStyle": {"glow": 1}})", "/overlayStyle/glow"},
        {head + R"("background": "dark", "fonts": {"axis": {"family": "Noto Sans", "pointSize": 8}}})",
         "/fonts/axis"},
        {head + R"("background": "dark", "fonts": {"axis": {"family": "", "pointSize": 8, "weight": 400}}})",
         "/fonts/axis/family"},
        {head + R"("background": "dark", "print": [1]})", "/print"},
    };
    for (const Case& c : cases) {
        INFO(c.json);
        std::vector<std::string> log;
        const auto r = Scheme::parse(c.json, log);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        CHECK(r.error().context.detail == c.pointer);
    }
    pt::TempDir dir("scheme");
    std::vector<std::string> log;
    auto s = Scheme::load(pt::dataFile(kLightScheme), log);
    REQUIRE(s.ok());
    s->name = "Mine";
    REQUIRE(s->save(dir.path() / "mine.scheme.json").ok());
    const auto back = Scheme::load(dir.path() / "mine.scheme.json", log);
    REQUIRE(back.ok());
    CHECK(*back == *s);
}
