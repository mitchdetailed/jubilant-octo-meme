// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Keymap (keymap-1; 06 sections 14.2-14.4, D-033) and the shipped data/keymaps/default.json: the complete
// default table, hotkey batch 1 implemented in M1 (07 section 4.1), every other row planned with its
// milestone.
#include "test_support.hpp"

#include <esses/project/keymap.hpp>
#include <esses/project/shipped_data.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

namespace {

// Every (command, context) of the 06 section 14.4 table ("GR, S" rows count once per context; the dialog
// navigation row is not in the keymap).
constexpr const char* kTable =
    "view.data_window global, view.channels_window worksheet, view.values_window worksheet,"
    "view.return_focus global, view.select_worksheet global, layout.editor global,"
    "view.worksheet_n worksheet, view.workbook_n global, view.prev_worksheet global,"
    "view.next_worksheet global, view.prev_workbook global, view.next_workbook global,"
    "view.channel_status global, view.messages_window global, view.track_map_window global,"
    "view.command_palette global, view.reset_docks global, view.fullscreen global,"
    "file.open_log global, file.close_all global, file.reload_source data_window,"
    "file.save_project global, file.print global, help.contents global, file.unload_device global,"
    "file.import_usb global, edit.undo global, edit.redo global, file.new_project global,"
    "file.open_project global, file.close_project global, file.import_data global,"
    "file.export_data global, component.cut worksheet, component.copy worksheet,"
    "component.paste worksheet, component.delete worksheet, component.select_all worksheet,"
    "data.toggle_domain global, data.overlays_toggle global, data.overlays_clear global,"
    "data.single_colour_main worksheet, data.next_range worksheet, data.prev_range worksheet,"
    "data.select_fastest global, data.select_all_fast global, data.swap_main_overlay0 worksheet,"
    "data.animate worksheet, data.lap_stretch_toggle global, data.filter_all_sources global,"
    "data.select_main data_window, data.toggle_overlay0 data_window,"
    "data.toggle_overlay data_window, data.close_file data_window, tools.details global,"
    "tools.channel_editor global, tools.channel_status global, tools.maths_editor global,"
    "tools.setup_sheets global, tools.lap_editor global, tools.track_editor global,"
    "tools.corrected_speed global, tools.aliases global, tools.mappings global,"
    "tools.units_list global, tools.options global, channel.next_active component,"
    "channel.prev_active component, channel.hide component, channel.remove component,"
    "channel.edit_maths component, channel.status component, channel.filter component,"
    "channel.scale_offset component, channel.zero_at_cursor component,"
    "channel.properties component, component.properties component, component.focus_next worksheet,"
    "component.focus_prev worksheet, component.legend component,"
    "component.colour_legend component, component.cycle_layout component,"
    "component.cycle_style component, component.colour_band component,"
    "component.maximise component, component.send_to_background component,"
    "component.bring_to_front component, component.export_data component,"
    "component.print_clipboard component, graph.status_errors graph, graph.device_events graph,"
    "graph.variance graph, graph.measurements graph, graph.copy_cursor_values graph,"
    "cursor.add_annotation graph, zoom.in graph, zoom.in summary, zoom.out graph,"
    "zoom.out summary, zoom.full global, zoom.default_range graph, zoom.default_range summary,"
    "zoom.datum_main graph, zoom.v_in graph, zoom.v_out graph, zoom.v_full graph,"
    "zoom.rubber_band graph, zoom.full_axis graph, zoom.wheel graph, zoom.wheel summary,"
    "zoom.wheel track, zoom.extend global, zoom.outing_rect graph, pan.left graph,"
    "pan.right graph, pan.up graph, pan.down graph, pan.back graph, pan.forward graph,"
    "pan.centre_cursor graph, pan.drag_axis graph, cursor.left graph, cursor.left summary,"
    "cursor.right graph, cursor.right summary, cursor.left_fast graph, cursor.left_fast summary,"
    "cursor.right_fast graph, cursor.right_fast summary, cursor.home graph, cursor.end graph,"
    "cursor.style graph, cursor.style summary, cursor.insert_marker graph, cursor.place graph,"
    "datum.toggle graph, datum.place graph, datum.swap graph, offset.axis_toggle graph,"
    "offset.main_left graph, offset.main_right graph, offset.main_down graph,"
    "offset.main_up graph, offset.overlay_left graph, offset.overlay_right graph,"
    "offset.overlay_down graph, offset.overlay_up graph, offset.drag graph, track.zoom_in track,"
    "track.zoom_out track, track.fit track,";

std::set<std::pair<std::string, std::string>> tablePairs() {
    std::set<std::pair<std::string, std::string>> out;
    std::istringstream in(kTable);
    std::string item;
    while (std::getline(in, item, ',')) {
        item.erase(0, item.find_first_not_of(' '));
        const auto space = item.find(' ');
        out.emplace(item.substr(0, space), item.substr(space + 1));
    }
    return out;
}

Keymap shipped() {
    std::vector<std::string> log;
    auto k = Keymap::load(pt::dataFile(kDefaultKeymap), log);
    REQUIRE(k.ok());
    CHECK(log.empty()); // no conflicts
    return std::move(k).value();
}

KeyBinding row(std::string command, std::vector<std::string> keys, std::string context) {
    KeyBinding b;
    b.command = std::move(command);
    b.keys = std::move(keys);
    b.context = std::move(context);
    return b;
}

} // namespace

TEST_CASE("keymap: the shipped default round-trips byte for byte and validates", "[unit]") {
    const Keymap k = shipped();
    CHECK(k.name == "default");
    CHECK_FALSE(k.inherits.has_value());
    CHECK(k.unbind.empty());
    CHECK(k.toJson() == pt::readAll(pt::dataFile(kDefaultKeymap)));
    CHECK(pt::schemaErrors(k.toJson(), "keymap-1").empty());
    CHECK(k.conflicts().empty());
}

TEST_CASE("keymap: the shipped default holds every row of the 06 section 14.4 table", "[unit]") {
    const Keymap k = shipped();
    std::set<std::pair<std::string, std::string>> rows;
    for (const KeyBinding& b : k.bindings) {
        rows.emplace(b.command, b.context);
    }
    const auto expected = tablePairs();
    CHECK(expected.size() == 151);
    CHECK(rows == expected);
    // Rows with arguments: one per worksheet/workbook number and per colour band.
    CHECK(k.rowsFor("view.worksheet_n").size() == 10);
    CHECK(k.rowsFor("view.workbook_n").size() == 10);
    CHECK(k.rowsFor("component.colour_band").size() == 10);
    CHECK(k.rowsFor("view.worksheet_n")[9]->keys == std::vector<std::string>{"0"});
    CHECK(k.rowsFor("view.worksheet_n")[9]->args.compact() == "[10]");
    CHECK(k.rowsFor("component.colour_band")[0]->keys == std::vector<std::string>{"Alt+0"});
    CHECK(k.rowsFor("component.colour_band")[0]->args.compact() == "[0]");
    // Spot checks of keys against the table.
    const auto keysOf = [&k](std::string_view command, std::string_view context) {
        for (const KeyBinding* b : k.rowsFor(command)) {
            if (b->context == context) {
                return b->keys;
            }
        }
        return std::vector<std::string>{"<missing>"};
    };
    CHECK(keysOf("data.toggle_domain", "global") == std::vector<std::string>{"F9"});
    CHECK(keysOf("data.select_main", "data_window") == std::vector<std::string>{"Return", "Enter"});
    CHECK(keysOf("data.toggle_overlay", "data_window") ==
          std::vector<std::string>{"Alt+Return", "Alt+Enter"});
    CHECK(keysOf("data.select_all_fast", "global") == std::vector<std::string>{"Ctrl+Alt+F"});
    CHECK(keysOf("zoom.full", "global") == std::vector<std::string>{"F2"});
    CHECK(keysOf("channel.zero_at_cursor", "component") == std::vector<std::string>{"Ctrl+Shift+Z"});
    CHECK(keysOf("edit.redo", "global") == std::vector<std::string>{"Ctrl+Y"});
    CHECK(keysOf("offset.overlay_up", "graph") == std::vector<std::string>{"Alt+Shift+Up"});
    CHECK(keysOf("track.zoom_in", "track") == std::vector<std::string>{"+", "="});
    // Ctrl+U belongs to tools.channel_status; its view alias carries no key (no conflict in global).
    CHECK(keysOf("tools.channel_status", "global") == std::vector<std::string>{"Ctrl+U"});
    CHECK(keysOf("view.channel_status", "global").empty());
}

TEST_CASE("keymap: hotkey batch 1 is implemented in M1, M2 and M3 rows as they land, every other row is "
          "planned or reserved",
          "[unit]") {
    const Keymap k = shipped();
    const std::set<std::string> batch1Areas{"view", "data", "zoom", "pan", "cursor", "datum"};
    std::set<std::string> implemented;
    for (const KeyBinding& b : k.bindings) {
        INFO(b.command << " " << b.context);
        CHECK_FALSE(b.doc.empty());
        switch (b.status) {
        case KeymapStatus::Implemented:
            // A closed milestone (M1-M3) or the one in progress (M4), whose rows turn implemented as they
            // land (tools/check_keymap_coverage.py); a menu-only or mouse row of M2/M3 may have no key (06
            // section 14.3).
            CHECK((b.milestone == "M1" || b.milestone == "M2" || b.milestone == "M3" || b.milestone == "M4"));
            if (b.milestone == "M1") {
                implemented.insert(b.command);
                CHECK(batch1Areas.count(b.command.substr(0, b.command.find('.'))) == 1);
                CHECK_FALSE(b.keys.empty()); // AC-M1-06 sends the key
            }
            break;
        case KeymapStatus::Planned:
            // At or before the current milestone (M3, AC-M3-01) means implemented (06 section 14.5).
            CHECK((b.milestone == "M4" || b.milestone == "v1.0" || b.milestone == "later"));
            break;
        case KeymapStatus::Reserved:
            CHECK(b.milestone.empty());
            CHECK_FALSE(b.keys.empty());
            break;
        }
    }
    const std::set<std::string> m1{"view.data_window",
                                   "view.channels_window",
                                   "view.values_window",
                                   "view.return_focus",
                                   "view.worksheet_n",
                                   "view.prev_worksheet",
                                   "view.next_worksheet",
                                   "data.toggle_domain",
                                   "data.overlays_toggle",
                                   "data.overlays_clear",
                                   "data.single_colour_main",
                                   "data.next_range",
                                   "data.prev_range",
                                   "data.select_fastest",
                                   "data.select_all_fast",
                                   "data.swap_main_overlay0",
                                   "data.select_main",
                                   "data.toggle_overlay0",
                                   "data.toggle_overlay",
                                   "data.close_file",
                                   "zoom.in",
                                   "zoom.out",
                                   "zoom.full",
                                   "zoom.default_range",
                                   "zoom.datum_main",
                                   "pan.left",
                                   "pan.right",
                                   "pan.back",
                                   "pan.forward",
                                   "pan.centre_cursor",
                                   "cursor.left",
                                   "cursor.right",
                                   "cursor.left_fast",
                                   "cursor.right_fast",
                                   "cursor.home",
                                   "cursor.end",
                                   "datum.toggle",
                                   "datum.place",
                                   "datum.swap"};
    CHECK(implemented == m1);
    // The AC-M1-01 walkthrough keys are implemented: F8, CTRL+F, ALT+ENTER, F9.
    for (const char* id :
         {"view.data_window", "data.select_fastest", "data.toggle_overlay", "data.toggle_domain"}) {
        CHECK(implemented.count(id) == 1);
    }
    // Summary-context rows of batch-1 commands landed with the summary components (M3).
    for (const KeyBinding* b : k.rowsFor("zoom.in")) {
        CHECK(b->status == KeymapStatus::Implemented);
    }
}

TEST_CASE("keymap: a user file inherits, replaces, unbinds and appends", "[unit]") {
    const Keymap base = shipped();
    std::vector<std::string> log;
    const auto user =
        Keymap::parse(pt::readAll(pt::repoRoot() / "schemas/examples/keymap-1/valid/user.json"), log);
    REQUIRE(user.ok());
    CHECK(user->inherits == "default");
    CHECK(user->bindings.size() == 4);
    CHECK(user->bindings[0].status == KeymapStatus::Implemented);
    CHECK(user->bindings[0].milestone.empty());
    // A user file writes no status (inherits is set) and keeps unbind rows.
    const std::string out = user->toJson();
    CHECK(out.find("\"status\"") == std::string::npos);
    CHECK(out.find("\"unbind\": [\n    {\n      \"command\": \"file.print\"") != std::string::npos);
    CHECK(pt::schemaErrors(out, "keymap-1").empty());

    Keymap custom = *user;
    custom.bindings.push_back(row("data.toggle_domain", {"Ctrl+9"}, "global")); // duplicate of an earlier row
    custom.bindings[0].keys = {"F10"};                                          // data.toggle_domain global
    custom.bindings.push_back(row("my.plugin_action", {"Ctrl+Shift+Y"}, "graph"));
    const Keymap merged = Keymap::merged(base, custom);
    CHECK(merged.name == "user");
    CHECK_FALSE(merged.inherits.has_value());
    CHECK(merged.unbind.empty());
    CHECK(merged.rowsFor("file.print").empty());
    const auto toggle = merged.rowsFor("data.toggle_domain");
    REQUIRE(toggle.size() == 1);
    CHECK(toggle[0]->keys == std::vector<std::string>{"Ctrl+9"}); // the last user row wins
    CHECK(toggle[0]->status == KeymapStatus::Implemented);
    CHECK(toggle[0]->milestone == "M1"); // status, milestone and doc stay the base row's
    CHECK_FALSE(toggle[0]->doc.empty());
    CHECK(merged.rowsFor("cursor.insert_marker")[0]->keys == std::vector<std::string>{"Shift+F7"});
    // The status stays the base row's (implemented since the shell's M2 wiring).
    CHECK(merged.rowsFor("cursor.insert_marker")[0]->status ==
          base.rowsFor("cursor.insert_marker")[0]->status);
    REQUIRE(merged.rowsFor("my.plugin_action").size() == 1);
    CHECK(merged.bindings.back().command == "my.plugin_action");
    CHECK(merged.bindings.size() == base.bindings.size() - 1 + 1);
    // Rows with arguments are replaced per argument.
    Keymap byArg;
    byArg.name = "user";
    byArg.inherits = "default";
    KeyBinding tab3 = row("view.worksheet_n", {"Shift+3"}, "worksheet");
    tab3.args = JsonDoc::parse("[3]").value();
    byArg.bindings.push_back(tab3);
    const Keymap m2 = Keymap::merged(base, byArg);
    const auto tabs = m2.rowsFor("view.worksheet_n");
    REQUIRE(tabs.size() == 10);
    CHECK(tabs[2]->keys == std::vector<std::string>{"Shift+3"});
    CHECK(tabs[3]->keys == std::vector<std::string>{"4"});
}

TEST_CASE("keymap: conflicts are the same normalised key twice in one context", "[unit]") {
    Keymap k;
    k.name = "t";
    k.bindings = {
        row("a.one", {"Ctrl+Shift+C"}, "graph"),    row("a.two", {"shift+ctrl+c"}, "graph"),
        row("a.three", {"Ctrl+Shift+C"}, "global"), row("a.four", {"Delete"}, "worksheet"),
        row("a.five", {"Delete"}, "data_window"),   row("a.six", {"Return", "Return"}, "data_window"),
        row("a.seven", {"F9"}, "global"),           row("a.eight", {"f9"}, "global")};
    const auto c = k.conflicts();
    REQUIRE(c.size() == 2);
    CHECK(c[0] == "keymap 't': Ctrl+Shift+C is bound twice in context graph (a.one and a.two)");
    CHECK(c[1] == "keymap 't': f9 is bound twice in context global (a.seven and a.eight)");
    // parse() logs them.
    std::vector<std::string> log;
    const auto parsed = Keymap::parse(k.toJson(), log);
    REQUIRE(parsed.ok());
    CHECK(log == c);

    CHECK(normaliseKeySequence("shift+ctrl+c") == "Ctrl+Shift+C");
    CHECK(normaliseKeySequence("Meta+Alt+Shift+Control+x") == "Ctrl+Alt+Shift+Meta+X");
    CHECK(normaliseKeySequence("F9") == "F9");
    CHECK(normaliseKeySequence("+") == "+");
    CHECK(normaliseKeySequence("Ctrl++") == "Ctrl++");
    CHECK(normaliseKeySequence("alt+return") == "Alt+return");
    CHECK(normaliseKeySequence("PgDown") == "PgDown");
}

TEST_CASE("keymap: malformed rows are Invalid at a JSON pointer", "[unit]") {
    struct Case {
        std::string json;
        std::string pointer;
    };
    const std::string head =
        R"({"$schema": "urn:esses:keymap-1", "schemaVersion": 1, "name": "k", "bindings": [)";
    const std::vector<Case> cases = {
        {head + R"({"command": "ToggleDomain", "keys": ["F9"], "context": "global"}]})",
         "/bindings/0/command"},
        {head + R"({"command": "data", "keys": ["F9"], "context": "global"}]})", "/bindings/0/command"},
        {head + R"({"command": "data..x", "keys": ["F9"], "context": "global"}]})", "/bindings/0/command"},
        {head + R"({"command": "data.x", "keys": ["F9"], "context": "dialog"}]})", "/bindings/0/context"},
        {head + R"({"command": "data.x", "keys": [""], "context": "global"}]})", "/bindings/0/keys/0"},
        {head + R"({"command": "data.x", "context": "global"}]})", "/bindings/0"},
        {head + R"({"command": "data.x", "keys": ["F9"], "context": "global", "args": 3}]})",
         "/bindings/0/args"},
        {head + R"({"command": "data.x", "keys": ["F9"], "context": "global", "status": "done"}]})",
         "/bindings/0/status"},
        {head + R"({"command": "data.x", "keys": ["F9"], "context": "global", "status": "planned"}]})",
         "/bindings/0"},
        {head + R"({"command": "data.x", "keys": ["F9"], "context": "global", "milestone": "M9"}]})",
         "/bindings/0/milestone"},
        {head + R"({"command": "data.x", "keys": ["F9"], "context": "global", "when": "x"}]})",
         "/bindings/0/when"},
        {head + R"(], "unbind": [{"command": "file.print"}]})", "/unbind/0"},
        {R"({"$schema": "urn:esses:keymap-1", "schemaVersion": 1, "name": "k"})", ""},
    };
    for (const Case& c : cases) {
        INFO(c.json);
        std::vector<std::string> log;
        const auto r = Keymap::parse(c.json, log);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
        CHECK(r.error().context.detail == c.pointer);
    }
    // A reserved row needs no milestone.
    std::vector<std::string> log;
    CHECK(Keymap::parse(head + R"({"command": "file.x", "keys": ["Ctrl+I"], "context": "global",
                                   "status": "reserved"}]})",
                        log)
              .ok());
}

TEST_CASE("keymap: save and load through a file", "[unit]") {
    pt::TempDir dir("keymap");
    Keymap k = shipped();
    k.name = "copy";
    REQUIRE(k.save(dir.path() / "keymap.json").ok());
    std::vector<std::string> log;
    const auto back = Keymap::load(dir.path() / "keymap.json", log);
    REQUIRE(back.ok());
    CHECK(*back == k);
    CHECK(keymapStatusName(KeymapStatus::Planned) == "planned");
    CHECK(keymapStatusName(KeymapStatus::Reserved) == "reserved");
    CHECK(keymapStatusName(KeymapStatus::Implemented) == "implemented");
}
