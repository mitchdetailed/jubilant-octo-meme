// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Keymap: a key binding document (schema keymap-1; 06 section 14.3 is canonical, the complete default table
// is 06 section 14.4). Shipped: data/keymaps/default.json, carrying status/milestone/doc on every row (M1:
// the full table, unimplemented rows `planned`, 07 section 4.1); user file <config>/keymap.json with inherits
// "default" may add, replace or unbind rows. AC-M1-06: one Qt Test per row with status "implemented",
// enforced by tools/check_keymap_coverage.py. Keys are Qt portable-text sequences kept as strings
// ("Ctrl+Shift+C", "F9", "Return"); contexts are the snake_case CommandContext names (global, worksheet,
// component, graph, summary, track, data_window, channels_window, values_window). esses_qtmodel's
// KeymapBinder turns them into Qt types.
//
// JSON written: name, inherits (null when absent), bindings, unbind, then the unknown members. A row writes
// command, keys and context, "args" when not null, "status" when it is not implemented or the row has a
// milestone (every shipped-default row; a user row has neither), "milestone" and "doc" when not empty. Rows
// are identified by (command, context, args): view.worksheet_n, view.workbook_n and component.colour_band
// have one row per argument.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

enum class KeymapStatus : std::uint8_t { Implemented, Planned, Reserved };
std::string_view keymapStatusName(KeymapStatus s) noexcept; // "implemented", "planned", "reserved"

// The snake_case CommandContext names (06 section 14.1).
inline constexpr std::array<std::string_view, 9> kKeymapContexts{
    "global", "worksheet",   "component",       "graph",        "summary",
    "track",  "data_window", "channels_window", "values_window"};
// The milestone vocabulary of keymap-1 rows (06 section 14.3).
inline constexpr std::array<std::string_view, 6> kKeymapMilestones{"M1", "M2", "M3", "M4", "v1.0", "later"};

struct KeyBinding {
    std::string command;           // "data.toggle_domain"
    std::vector<std::string> keys; // may be empty for menu-only commands
    std::string context;           // "global", "graph", "data_window", ...
    JsonDoc args;                  // optional array
    // Shipped-default fields (ignored in user files):
    KeymapStatus status = KeymapStatus::Implemented;
    std::string milestone; // "M1".."M4", "v1.0", "later"; empty for reserved rows
    std::string doc;
    friend bool operator==(const KeyBinding&, const KeyBinding&) = default;
};

struct KeyUnbind {
    std::string command;
    std::string context;
    friend bool operator==(const KeyUnbind&, const KeyUnbind&) = default;
};

struct Keymap {
    std::string name;
    std::optional<std::string> inherits; // "default" for a user file
    std::vector<KeyBinding> bindings;
    std::vector<KeyUnbind> unbind;
    JsonDoc unknown; // unrecognised top-level members, preserved
    // The minor written on save: the newest this build writes, or the newer minor the document was read with
    // (R-38: never downgraded).
    int schemaVersion = 1;

    // Invalid (context.detail = JSON pointer) on a malformed row: a command that is not <area>.<verb_noun>
    // snake_case, an unknown context, status or milestone, an empty key, args that are not an array, or a
    // planned/implemented row without milestone.
    static Result<Keymap> parse(std::string_view json, std::vector<std::string>& log);
    static Result<Keymap> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text
    Result<void> save(const std::filesystem::path& path) const;

    // `user` applied on top of `base`: user rows replace base rows with the same (command, context, args)
    // (keys and args from the user row; status, milestone and doc stay the base row's), unbind rows remove
    // every base row of (command, context), new rows are appended. The result has user's name (base's when
    // empty), no `inherits` and no unbind rows.
    static Keymap merged(const Keymap& base, const Keymap& user);
    // Rows that bind the same key sequence twice in one context (06 section 14.2 rule 7), as messages, in row
    // order. Key sequences compare after normaliseKeySequence, key names case-insensitively.
    std::vector<std::string> conflicts() const;
    // The rows of `command` (every context), in order.
    std::vector<const KeyBinding*> rowsFor(std::string_view command) const;

    friend bool operator==(const Keymap&, const Keymap&) = default;
};

// Portable-text normal form for comparisons: modifiers in the order Ctrl, Alt, Shift, Meta with that
// spelling (case-insensitive on input; "Control" is Ctrl), single letters upper case, other key names as
// written: "shift+ctrl+c" -> "Ctrl+Shift+C"; "+" and "Ctrl++" keep the plus key.
std::string normaliseKeySequence(std::string_view keys);

} // namespace esses::project
