// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Scheme: a colour and font scheme (schema scheme-1; 06 section 16 is canonical, 02 section 6.9 an excerpt;
// D-017 colours). Shipped: data/schemes/light.scheme.json and data/schemes/dark.scheme.json (07 section 4.1;
// the golden images fix the scheme to these, 04 section 6.7). Per user: <config>/schemes/*.scheme.json.
// Roles are looked up by name ("cptBackground", "cursorMain", "overlay0", "mainSingleColour", ...); font
// roles map the plot text styles (04 section 6.1 item 4: axis, axisRotated, caption, key, small, gauge,
// reportTable, equation, colourBands, trackLabel, valuesHeader, valuesItem).
//
// JSON written: name, background, roles and fonts (when not empty), channelPalette and overlayPalette (when
// not empty), overlayStyle, print (when not null), then the unknown members; a font's "strikeOut" only when
// true. Colours are "#rrggbb" (alpha ff) or "#rrggbbaa", lower case. Checked on load: channelPalette has at
// least 10 entries and overlayPalette exactly 9 when present (06 section 16 rules 2-3).
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc
#include <esses/project/rgba.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

struct FontSpec {
    std::string family = "Noto Sans";
    double pointSize = 9.0;
    int weight = 400;
    bool italic = false;
    bool strikeOut = false; // JSON "strikeOut", written only when true (keyStrikeout)
    friend bool operator==(const FontSpec&, const FontSpec&) = default;
};

struct OverlayStyle {                     // D-017 / D-100 hook; default = plain channel colours
    std::string colourSource = "channel"; // "channel" | "overlayPalette"
    std::string dash = "solid";           // "solid" | "dash" | "dot" | "dashDot"
    double lightness = 0.0;               // -1..1
    double widthScale = 1.0;
    friend bool operator==(const OverlayStyle&, const OverlayStyle&) = default;
};

// Every colour role of 06 section 16, in its order; the shipped schemes define all of them.
inline constexpr std::array<std::string_view, 36> kSchemeRoles{"windowBackground",
                                                               "cptFrame",
                                                               "cptBackground",
                                                               "cptForeground",
                                                               "cptForeground2",
                                                               "cptFocusBorder",
                                                               "cptCaptionBackground",
                                                               "cptCaptionForeground",
                                                               "dataBackground",
                                                               "dataForeground",
                                                               "dataForeground2",
                                                               "dataGrid",
                                                               "dataHeadingBackground",
                                                               "dataHeadingForeground",
                                                               "dataHighlightBackground",
                                                               "dataHighlightForeground",
                                                               "dataError",
                                                               "dataUnknown",
                                                               "keyBackground",
                                                               "cursorMain",
                                                               "cursorDatum",
                                                               "cursorZoom",
                                                               "rangeMark",
                                                               "sectionCorner",
                                                               "sectionStraight",
                                                               "markerBand",
                                                               "annotationMark",
                                                               "deviceEvent",
                                                               "varianceZero",
                                                               "statusOk",
                                                               "statusWarn",
                                                               "statusError",
                                                               "pendingHatch",
                                                               "mainSingleColour",
                                                               "overlay0",
                                                               "trackLine"};
// Every font role of 06 section 16.
inline constexpr std::array<std::string_view, 16> kSchemeFontRoles{
    "axis",        "axisRotated",  "caption",    "key",         "keyStrikeout",  "small",
    "colourBands", "trackLabel",   "equation",   "reportTable", "reportHeading", "dataHeader",
    "dataRange",   "valuesHeader", "valuesItem", "gauge"};

struct Scheme {
    std::string name;
    std::string background = "light"; // "light" | "dark"
    std::map<std::string, Rgba, std::less<>> roles;
    std::vector<Rgba> channelPalette;
    std::vector<Rgba> overlayPalette; // nine entries
    OverlayStyle overlayStyle;
    std::map<std::string, FontSpec, std::less<>> fonts;
    JsonDoc print;   // print overrides of roles/palettes
    JsonDoc unknown; // unrecognised top-level members, preserved
    // The minor written on save: the newest this build writes, or the newer minor the document was read with
    // (R-38: never downgraded).
    int schemaVersion = 1;

    // The colour of a role; `fallback` when the scheme does not define it.
    Rgba role(std::string_view roleName, Rgba fallback = 0xFF00FFFF) const;
    // The font of a role; the "caption" font, else FontSpec{}, when the role is missing.
    FontSpec font(std::string_view roleName) const;
    // channelPalette[index mod size]; `fallback` for an empty palette or a negative index.
    Rgba channelColour(int index, Rgba fallback = 0x808080FF) const;
    // overlayPalette[(overlayNumber - 1) mod 9] for Overlays 1..n (06 section 16 rule 3); `fallback` for an
    // empty palette or a number below 1.
    Rgba overlayColour(int overlayNumber, Rgba fallback = 0x808080FF) const;

    // 06 section 16 rule 8: fills the roles, fonts and palettes this scheme lacks from `shipped` (the shipped
    // scheme of the same background) and returns one Messages line per filled item.
    std::vector<std::string> fillMissingFrom(const Scheme& shipped);

    static Result<Scheme> parse(std::string_view json, std::vector<std::string>& log);
    static Result<Scheme> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text; "" on a non-finite number
    Result<void> save(const std::filesystem::path& path) const;

    friend bool operator==(const Scheme&, const Scheme&) = default;
};

} // namespace esses::project
