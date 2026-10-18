// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TextProfile (05 section 7.2, D-029; schema importer-profile-1): a declarative description of a delimited
// text format that the TextEngine reads. Shipped profiles live in data/profiles-text/<id>.json
// (motec.csv.json, generic.csv.json), user profiles in <config>/profiles-text/<id>.json. Only shipped
// profiles may set importerId (and only to a built-in id such as motec.csv); every other profile registers as
// text.profile:<id>. The importer id's stem (without "text.profile:") names the unit map, dictionary and
// fixture directory. `version` is the profile's content version and enters the column cache key (05 section
// 2).
//
// The members follow the key table of 05 section 7.2; strings keep the schema's spellings where a key accepts
// a value or an @auto directive ("auto", "@auto", "fromDetail:@rate", "infer").
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::formats {

struct TextProfile {
    static constexpr std::string_view kSchemaId = "urn:esses:importer-profile-1";
    static constexpr std::string_view kImporterIdPrefix = "text.profile:";

    enum class Encoding : std::uint8_t { Auto, Utf8, Utf16Le, Utf16Be, Cp1252 };
    enum class Layout : std::uint8_t { Table, Blocks };
    enum class Sparse : std::uint8_t { Timestamped, Hold };
    enum class DuplicateNames : std::uint8_t { Suffix, Error };

    struct Sniff {
        std::string preambleRegex; // multiline regex on the first 64 KiB; empty: none
        std::vector<std::string> extensions;
        int confidence = 50;
    };
    struct Preamble {
        int lines = 0;          // {lines: n}
        std::string untilRegex; // {untilRegex} ("@auto" for the generic rule)
        bool untilBlank = false;
        std::vector<std::pair<std::string, std::string>> capture; // detailId | @input -> regex | key name
        std::vector<std::array<int, 3>> keyValueColumns;          // [[key, value, unit], ...]
        std::string unmatchedTo;                                  // detail receiving unmatched lines
    };
    struct Header {
        bool skipBlank = true;
        int namesRow = 0;
        std::string unitsRow = "null"; // an integer, "null" or "@auto"
        std::string dataRow = "auto";  // an integer or "auto"
        std::string unitInName;        // regex with one group; empty: none
        bool ignoreEmptyNames = true;
    };
    struct Time {
        std::string column = "@auto";
        std::string unit = "@auto"; // s | ms | us | hhmmss | epoch_s | epoch_ms | @auto
        std::string rate = "infer"; // "infer" | "fromDetail:@x" (fixedRate when {num, den})
        std::optional<Rate> fixedRate;
        std::string origin; // "" | "first" | "zero"
    };
    struct Column { // per-column overrides applied before the dictionary
        std::string name, quantity, unit, dtype, kind, interp;
        std::optional<int> dps;
        std::vector<std::string> sentinels;
    };
    struct LapSource {
        std::string kind = "none"; // none | lap_counter_edge | beacon_event | lap_table
        std::string column, section, detailsKey;
        std::map<std::string, std::string> match;
        bool optional = false;
        std::shared_ptr<const LapSource> fallback;
    };

    std::string id;
    std::string importerId; // empty: text.profile:<id>
    std::string name;
    std::string version;
    Sniff sniff;
    Encoding encoding = Encoding::Auto;
    std::string delimiter = "auto"; // , ; \t | space auto
    std::string decimal = "auto";   // . , auto
    std::optional<char> quote = '"';
    bool trim = true;
    std::optional<bool> trailingDelimiter; // nullopt: auto
    Layout layout = Layout::Table;
    Preamble preamble;
    Header header;
    Time time;
    std::vector<std::string> sentinels{"", "NaN", "-###", "+###"};
    Sparse sparse = Sparse::Timestamped;
    DuplicateNames duplicateNames = DuplicateNames::Suffix;
    std::map<std::string, Column> columns;
    LapSource lapSource;
    model::OpaqueJson blocks;  // layout "blocks" only (05 section 7.3)
    model::OpaqueJson unknown; // unrecognised top-level members, preserved

    // Errors: Format (syntax, context.offset), Invalid (an importer-profile-1 rule, context.detail = JSON
    // pointer), Unsupported (another schema major).
    static Result<TextProfile> parse(std::string_view json, std::vector<std::string>& log);
    static Result<TextProfile> load(const std::filesystem::path& path, std::vector<std::string>& log);
    Result<std::string> toJson() const; // canonical text (02 section 6.5 rule 1)

    // importerId when set, else "text.profile:" + id.
    std::string effectiveImporterId() const;
    // The profile's share of the column cache key (05 section 2: xxh3 of the profile JSON).
    Hash128 contentHash() const;
};

} // namespace esses::formats
