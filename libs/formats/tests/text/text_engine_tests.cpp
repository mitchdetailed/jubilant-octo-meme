// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The TextEngine through the registry and the open pipeline (05 sections 2, 6 and 7): the legacy CSV dialect
// and the auto-detecting generic profile on small self-authored inputs: preamble details, header and units,
// the rate from the details and from the time column, sentinels, hold and sparse cells, decimal comma,
// timestamped files (last wins, out-of-order rows dropped), text columns as enumerations, encodings, lap
// counter beacons, the dictionary and column hints of step 4a, units of step 4b, sniffing.
#include "text_test_support.hpp"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::texttest;
using Catch::Approx;

namespace {

// A legacy-dialect file written for this test (values invented): eleven quoted metadata lines, two blank
// lines, names, units, two blank lines, data; CRLF; 20.48 Hz printed with three decimals.
constexpr std::string_view kLegacy = "\"Format\",\"MoTeC CSV File\",,,\"Workbook\",\"Bench\"\r\n"
                                     "\"Venue\",\"Testville\",,,\"Worksheet\",\"Graph\"\r\n"
                                     "\"Vehicle\",\"T-1\",,,\"Vehicle Desc\",\"Test car\"\r\n"
                                     "\"Driver\",\"A. Tester\",,,\"Engine ID\",\"E1\"\r\n"
                                     "\"Device\",\"LOGGER\"\r\n"
                                     "\"Comment\",\"synthetic\",,,\"Session\",\"Practice\"\r\n"
                                     "\"Log Date\",\"02/01/2026\",,,\"Origin Time\",\"0.000\",\"s\"\r\n"
                                     "\"Log Time\",\"10:00:00\",,,\"Start Time\",\"0.000\",\"s\"\r\n"
                                     "\"Sample Rate\",\"20.480\",\"Hz\",,\"End Time\",\"0.244\",\"s\"\r\n"
                                     "\"Duration\",\"0.244\",\"s\",,\"Start Distance\",\"0\",\"m\"\r\n"
                                     "\"Range\",\"whole log\",,,\"End Distance\",\"2\",\"m\"\r\n"
                                     "\r\n\r\n"
                                     "\"Time\",\"Corr Dist\",\"Engine RPM\",\"Ground Speed\",\"Gear\"\r\n"
                                     "\"s\",\"m\",\"rpm\",\"km/h\",\"\"\r\n"
                                     "\r\n\r\n"
                                     "\"0.000\",\"0\",\"1500\",\"36.0\",\"1\"\r\n"
                                     "\"0.049\",\"0.5\",\"1510\",\"\",\"1\"\r\n"
                                     "\"0.098\",\"1.0\",\"-###\",\"36.5\",\"2\"\r\n"
                                     "\"0.146\",\"1.5\",\"1530\",\"37\",\"2\"\r\n"
                                     "\"0.195\",\"2.1\",\"1540\",\"37.5\",\"\"\r\n";

std::string detailText(const ProbeResult& p, std::string_view id) {
    for (const model::Detail& d : p.details) {
        if (d.id == id) {
            return model::detailValueText(d.value);
        }
    }
    return "<absent>";
}

} // namespace

TEST_CASE("text engine: the legacy CSV dialect", "[unit]") {
    Env env;
    auto o = env.open(kLegacy, "session.csv");
    REQUIRE(o.ok());
    const ProbeResult& p = o->probe;
    CHECK(p.importerId == kLegacyCsvImporterId);
    CHECK(p.dialect.empty());
    // Details from the key/value columns; @-captures are engine inputs, not details; empty values are none.
    CHECK(detailText(p, "Venue") == "Testville");
    CHECK(detailText(p, "Vehicle Id") == "T-1");
    CHECK(detailText(p, "Vehicle Desc") == "Test car");
    CHECK(detailText(p, "Engine Id") == "E1");
    CHECK(detailText(p, "Device Type") == "LOGGER");
    CHECK(detailText(p, "Export Workbook") == "Bench");
    CHECK(detailText(p, "Export Range") == "whole log");
    CHECK(detailText(p, "Sample Rate") == "<absent>");
    CHECK(detailText(p, "Format") == "<absent>");
    REQUIRE(p.layers.size() == 1);
    const LayerInfo& layer = p.layers[0];
    REQUIRE(layer.channels.size() == 4); // Time is the time base, not a channel
    CHECK(layer.channels[0].loggedName == "Corr Dist");
    CHECK(layer.channels[0].dtype == store::Dtype::F64); // the profile's column override
    CHECK(layer.channels[1].vendorUnit == "rpm");
    CHECK(layer.channels[1].kind == store::SampleKind::Integer);
    CHECK(layer.channels[3].vendorUnit.empty());
    for (const ChannelInfo& c : layer.channels) {
        CHECK(std::get<Rate>(c.timing) == Rate{512, 25}); // the details' 20.480 Hz, exactly
        CHECK(c.sampleCount == 5);
        CHECK(c.t0Us == 0);
        CHECK(c.vendorDtype == "text");
    }
    CHECK(layer.durationUs == Rate{512, 25}.timeAt(5, 0));
    CHECK(hasCode(p.warnings, "text.rate_from_detail"));
    CHECK_FALSE(hasCode(p.warnings, "text.time_rate_mismatch"));

    // Step 4: units to base (D-010), the profile's column hint (Corr Dist: Distance), the dictionary (Gear).
    const model::Channel& rpm = channel(*o, "Engine RPM");
    CHECK(env.units->unit(rpm.desc.unit).symbol == "rad/s");
    CHECK(rpm.desc.kind == store::SampleKind::Real);
    const Dump r = dumpSeries(rpm.series);
    REQUIRE(r.values.size() == 5);
    CHECK(r.values[0] == Approx(1500.0 * 2.0 * std::numbers::pi / 60.0).epsilon(1e-6));
    CHECK_FALSE(r.valid[2]); // -### is a sentinel: invalid, never an error
    CHECK(r.valid[3]);
    const Dump speed = dumpSeries(channel(*o, "Ground Speed").series);
    CHECK(speed.values[0] == Approx(10.0)); // 36 km/h
    CHECK(speed.values[1] == Approx(10.0)); // sparse "hold": the empty cell repeats the previous value
    CHECK(speed.values[2] == Approx(36.5 / 3.6));
    const model::Channel& gear = channel(*o, "Gear");
    CHECK(gear.desc.kind == store::SampleKind::Integer);
    CHECK(gear.desc.interp == store::Interp::Previous);
    CHECK(gear.desc.unit == units::kNoUnit);
    const Dump g = dumpSeries(gear.series);
    CHECK(g.values == std::vector<double>{1, 1, 2, 2, 2});
    const model::Channel& dist = channel(*o, "Corr Dist");
    CHECK(env.units->quantity(dist.desc.quantity).name == "Distance");
    CHECK(dumpSeries(dist.series).values[4] == 2.1);
    // The whole outing (no lap column in this file) and the synthesised SOL/EOL.
    CHECK(hasCode(o->log, "text.lap_source_absent"));
    CHECK(o->source->layers[0].markers.size() == 2);
}

TEST_CASE("text engine: semicolons, decimal comma, units in names, sparse and text columns", "[unit]") {
    constexpr std::string_view kText = "Logger export v2\n"
                                       "Driver: Someone\n"
                                       "\n"
                                       "Time;Speed (km/h);Oil Temp [deg C];Gear;Status\n"
                                       "0,000;10,5;80,25;1;OK\n"
                                       "0,100;11,0;80,5;1;OK\n"
                                       "0,200;11,5;;2;WARN\n"
                                       "0,300;12,0;81,0;2;OK\n";
    Env env;
    auto o = env.open(kText, "logger.txt");
    REQUIRE(o.ok());
    const ProbeResult& p = o->probe;
    CHECK(p.importerId == kGenericCsvImporterId);
    CHECK(p.dialect == "semicolon,decimal-comma");
    CHECK(detailOf(p.warnings, "text.auto_preamble") == "2");
    CHECK(detailOf(p.warnings, "text.auto_units_row") == "no");
    CHECK(detailOf(p.warnings, "text.auto_time_column") == "Time");
    CHECK(detailOf(p.warnings, "text.rate_inferred") == "10 Hz");
    const LayerInfo& layer = p.layers[0];
    REQUIRE(layer.channels.size() == 4);
    CHECK(layer.channels[0].loggedName == "Speed");
    CHECK(layer.channels[0].vendorUnit == "km/h");
    CHECK(layer.channels[1].loggedName == "Oil Temp");
    CHECK(layer.channels[1].vendorUnit == "deg C");
    // Oil Temp has an empty cell: under sparse "timestamped" it keeps only its own samples, with times.
    CHECK(std::holds_alternative<Timestamped>(layer.channels[1].timing));
    CHECK(layer.channels[1].sampleCount == 3);
    CHECK(std::get<Rate>(layer.channels[0].timing) == Rate{10, 1});
    CHECK(layer.channels[3].kind == store::SampleKind::Enum);
    REQUIRE(p.enumTables.size() == 1);
    CHECK(p.enumTables[0].entries.size() == 2);
    CHECK(p.enumTables[0].entries[1].label == "WARN");
    CHECK(hasCode(p.warnings, "text.sparse_column"));

    const Dump speed = dumpSeries(channel(*o, "Speed").series);
    CHECK(speed.values[0] == Approx(10.5 / 3.6));
    CHECK(speed.values[3] == Approx(12.0 / 3.6));
    const model::Channel& oil = channel(*o, "Oil Temp");
    REQUIRE(std::holds_alternative<store::TimestampedSeries>(oil.series));
    const Dump t = dumpSeries(oil.series);
    CHECK(t.times == std::vector<Micros>{0, 100000, 300000});
    CHECK(t.values[1] == Approx(80.5 + 273.15));
    const auto& ts = std::get<store::TimestampedSeries>(oil.series);
    CHECK(ts.tMin == 0);
    CHECK(ts.tMax == 300000);
    const Dump status = dumpSeries(channel(*o, "Status").series);
    CHECK(status.values == std::vector<double>{0, 0, 1, 0});
    // The generic dictionary makes Gear an Integer channel drawn as steps.
    CHECK(channel(*o, "Gear").desc.interp == store::Interp::Previous);
}

TEST_CASE("text engine: a non-uniform time column gives timestamped channels (last wins)", "[unit]") {
    constexpr std::string_view kText = "time,a,b\n"
                                       "0.0,1,10\n"
                                       "0.1,2,20\n"
                                       "0.25,3,30\n"
                                       "0.25,4,40\n"
                                       "0.2,5,50\n"
                                       "0.4,6,60\n"
                                       "x,7,70\n";
    Env env;
    auto o = env.open(kText);
    REQUIRE(o.ok());
    const ProbeResult& p = o->probe;
    CHECK(p.dialect == "timestamped");
    CHECK(hasCode(p.warnings, "text.timestamped"));
    CHECK(detailOf(p.warnings, "text.time_not_monotonic") == "1");
    CHECK(detailOf(p.warnings, "text.time_duplicate") == "1");
    CHECK(detailOf(p.warnings, "text.time_invalid") == "1");
    const Dump a = dumpSeries(channel(*o, "a").series);
    CHECK(a.times == std::vector<Micros>{0, 100000, 250000, 400000});
    CHECK(a.values == std::vector<double>{1, 2, 4, 6});
    // Both channels share one time column.
    const auto& sa = std::get<store::TimestampedSeries>(channel(*o, "a").series);
    const auto& sb = std::get<store::TimestampedSeries>(channel(*o, "b").series);
    CHECK(sa.timesUs == sb.timesUs);
    CHECK(o->source->layers[0].duration == 400000);
}

TEST_CASE("text engine: epoch times, origin, utc anchor and the ms unit", "[unit]") {
    Env env;
    auto e = env.open("timestamp,v\n1700000000000,1\n1700000000050,2\n1700000000100,3\n");
    REQUIRE(e.ok());
    CHECK(detailOf(e->probe.warnings, "text.auto_time_unit") == "epoch_ms");
    const LayerInfo& l = e->probe.layers[0];
    REQUIRE(l.utc.has_value());
    CHECK(l.utc->unixUs == 1'700'000'000'000'000);
    CHECK(std::get<Rate>(l.channels[0].timing) == Rate{20, 1});
    CHECK(l.channels[0].t0Us == 0);

    auto ms = env.open("t,v\n0,1\n20000,2\n40000,3\n");
    REQUIRE(ms.ok());
    CHECK(detailOf(ms->probe.warnings, "text.auto_time_unit") == "ms");
    CHECK(std::get<Rate>(ms->probe.layers[0].channels[0].timing) == Rate{1, 20});

    // No time column at all: rows one second apart, with a warning.
    auto none = env.open("a,b\n1,2\n3,4\n", "x.csv", [] {
        ImportOptions opt;
        opt.importerId = std::string(kGenericCsvImporterId);
        return opt;
    }());
    REQUIRE(none.ok());
    CHECK(hasCode(none->probe.warnings, "text.auto_time_column"));
}

TEST_CASE("text engine: encodings, BOMs, CR-only lines, quotes and a trailing delimiter", "[unit]") {
    Env env;
    // UTF-8 BOM, CRLF, quoted text with the delimiter in it, a trailing delimiter on every row.
    const std::string bom =
        "\xEF\xBB\xBFTime,Name,Value,\r\n0,\"a, b\",1,\r\n1,\"c\",2,\r\n2,\"a, b\",3,\r\n";
    auto o = env.open(bom);
    REQUIRE(o.ok());
    REQUIRE(o->probe.layers[0].channels.size() == 2);
    CHECK(o->probe.enumTables.at(0).entries.at(0).label == "a, b");
    CHECK(dumpSeries(channel(*o, "Value").series).values == std::vector<double>{1, 2, 3});
    CHECK(hasCode(o->probe.warnings, "text.encoding"));

    // cp1252 names (not valid UTF-8) are transcoded.
    auto cp = env.open(std::string("Time,Temp\xE9rature\n0,1\n1,2\n"));
    REQUIRE(cp.ok());
    CHECK(cp->probe.layers[0].channels[0].loggedName == "Temp\xC3\xA9rature");

    // UTF-16LE with a BOM.
    const std::string ascii = "Time,V\r\n0,5\r\n1,6\r\n";
    std::string u16 = "\xFF\xFE";
    for (const char c : ascii) {
        u16 += c;
        u16 += '\0';
    }
    auto w = env.open(u16);
    REQUIRE(w.ok());
    CHECK(detailOf(w->probe.warnings, "text.encoding") == "utf-16le");
    CHECK(dumpSeries(channel(*w, "V").series).values == std::vector<double>{5, 6});

    // Old Mac line ends.
    auto cr = env.open("Time,V\r0,1\r1,2\r2,3\r");
    REQUIRE(cr.ok());
    CHECK(dumpSeries(channel(*cr, "V").series).values == std::vector<double>{1, 2, 3});

    // Duplicate names get a suffix; columns without a name are dropped.
    auto dup = env.open("Time,A,A,,B\n0,1,2,3,4\n1,5,6,7,8\n");
    REQUIRE(dup.ok());
    const LayerInfo& l = dup->probe.layers[0];
    REQUIRE(l.channels.size() == 3);
    CHECK(l.channels[1].loggedName == "A (2)");
    CHECK(hasCode(dup->probe.warnings, "text.duplicate_name"));
    CHECK(hasCode(dup->probe.warnings, "text.empty_name"));
}

TEST_CASE("text engine: a lap counter column gives beacons at its changes", "[unit]") {
    Env env;
    auto o = env.open("Time,Lap,Speed\n0,1,10\n0.5,1,11\n1.0,2,12\n1.5,2,13\n2.0,3,14\n2.5,3,15\n");
    REQUIRE(o.ok());
    const LayerInfo& l = o->probe.layers[0];
    REQUIRE(l.markers.size() == 2);
    CHECK(l.markers[0].cls == model::MarkerClass::BCN);
    CHECK(l.markers[0].tUs == 1'000'000);
    CHECK(l.markers[1].tUs == 2'000'000);
    CHECK(detailOf(o->probe.warnings, "text.lap_source") == "lap_counter_edge:Lap");
    // SOL and EOL are synthesised around the beacons.
    CHECK(o->source->layers[0].markers.size() == 4);
}

TEST_CASE("text engine: ragged rows and unparsable cells are invalid samples, logged at decode", "[unit]") {
    Env env;
    auto o = env.open("Time,A,B\n0,1,2\n1,3\n2,x,5\n3,7,8\n");
    REQUIRE(o.ok());
    const Dump a = dumpSeries(channel(*o, "A").series);
    const Dump b = dumpSeries(channel(*o, "B").series);
    CHECK(a.valid == std::vector<bool>{true, true, false, true});
    CHECK(b.valid == std::vector<bool>{true, false, true, true}); // a missing cell is invalid, not sparse
    const auto decoded = o->decodeLog->snapshot();
    CHECK(hasCode(decoded, "text.ragged_row"));
    CHECK(hasCode(decoded, "text.parse_error"));
    // Decode-time entries never enter the probe (05 section 1.1).
    CHECK_FALSE(hasCode(o->probe.warnings, "text.ragged_row"));
}

TEST_CASE("text engine: sniff confidence of the text importers", "[unit]") {
    Env env;
    const auto rank = [&](std::string_view text, std::string name) {
        const std::vector<std::byte> b = bytesOf(text);
        SniffInput in;
        in.path = pathFromUtf8(name);
        in.head = b;
        in.size = b.size();
        return env.registry->sniff(in);
    };
    const auto legacy = rank(kLegacy, "run.csv");
    REQUIRE(legacy.size() == 2);
    CHECK(legacy[0].first->id() == kLegacyCsvImporterId);
    CHECK(legacy[0].second.confidence == 95);
    CHECK(legacy[1].first->id() == kGenericCsvImporterId);
    CHECK(legacy[1].second.confidence == 30);
    // The legacy preamble wins even under another extension; a plain .csv goes to the generic profile.
    CHECK(rank(kLegacy, "run.txt")[0].first->id() == kLegacyCsvImporterId);
    const auto plain = rank("Time,A\n0,1\n1,2\n", "plain.csv");
    REQUIRE(plain.size() == 2);
    CHECK(plain[0].first->id() == kGenericCsvImporterId);
    CHECK(plain[1].second.confidence == 20); // the legacy profile by extension only
    // Delimited text under an unknown extension: the generic profile at 10; binary: nobody.
    const auto dat = rank("a;b\n1;2\n3;4\n5;6\n", "log.dat");
    REQUIRE(dat.size() == 1);
    CHECK(dat[0].second.confidence == 10);
    for (const auto& ranked : rank(std::string("\x01\x02\0\0binary", 10), "x.csv")) {
        CHECK(ranked.first->id() == kLdImporterId); // never a text importer
    }
}

TEST_CASE("text engine: a profile with the blocks layout is refused for now", "[unit]") {
    std::vector<std::string> log;
    auto p = TextProfile::parse(
        R"({"$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "sectioned",
        "name": "S", "version": "1", "layout": "blocks", "blocks": {"sectionRegex": "^\\{(\\w+)\\}$"}})",
        log);
    REQUIRE(p.ok());
    auto imp = TextEngine::makeImporter(*p);
    auto probe = imp->probe(*memory("{A}\nx\n"));
    REQUIRE_FALSE(probe.ok());
    CHECK(probe.error().code == ErrorCode::Unsupported);
}

TEST_CASE("text engine: addProfile refuses colliding ids and foreign importer ids", "[unit]") {
    Env env;
    std::vector<std::string> log;
    const std::string head =
        R"({"$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "name": "N", "version": "1", )";
    auto dup = TextProfile::parse(head + R"("id": "generic.csv"})", log);
    REQUIRE(dup.ok());
    CHECK(env.registry->addProfile(*dup).error().code == ErrorCode::Invalid);
    auto foreign = TextProfile::parse(head + R"("id": "mine", "importerId": "motec.ld"})", log);
    REQUIRE(foreign.ok());
    CHECK(env.registry->addProfile(*foreign).error().code == ErrorCode::Invalid);
    auto user = TextProfile::parse(
        head + R"("id": "mine", "delimiter": "|", "sniff": {"extensions": [".pip"], "confidence": 40}})",
        log);
    REQUIRE(user.ok());
    REQUIRE(env.registry->addProfile(*user).ok());
    REQUIRE(env.registry->find("text.profile:mine") != nullptr);
    auto o = env.open("Time|X\n0|1\n1|2\n", "a.pip");
    REQUIRE(o.ok());
    CHECK(o->probe.importerId == "text.profile:mine");
}

TEST_CASE("text engine: a UTF-16 file of odd length drops its last byte, not a row more", "[unit]") {
    Env env;
    std::string ascii = "Time,V\r\n";
    for (int r = 0; r < 20; ++r) {
        ascii += std::to_string(r) + "," + std::to_string(r * 2) + "\r\n";
    }
    std::string u16 = "\xFF\xFE";
    for (const char c : ascii) {
        u16 += c;
        u16 += '\0';
    }
    u16 += 'x'; // a truncated export: one byte of the next character
    auto o = env.open(u16);
    REQUIRE(o.ok());
    CHECK(o->probe.layers[0].channels[0].sampleCount == 20);
    const Dump v = dumpSeries(channel(*o, "V").series);
    REQUIRE(v.values.size() == 20);
    CHECK(std::all_of(v.valid.begin(), v.valid.end(), [](bool b) { return b; }));
    CHECK(detailOf(o->probe.warnings, "text.encoding") == "utf-16le");
    const auto odd =
        std::find_if(o->probe.warnings.begin(), o->probe.warnings.end(), [](const ImportLogEntry& e) {
            return e.code == "text.encoding" && e.level == ImportLogEntry::Level::Warning;
        });
    REQUIRE(odd != o->probe.warnings.end());
    CHECK(odd->ctx.detail == std::optional<std::string>("odd byte count"));
}

TEST_CASE("text engine: a header without data rows opens with no samples", "[unit]") {
    Env env(false);
    ImportOptions opt;
    opt.importerId = std::string(kGenericCsvImporterId);
    {
        auto o = env.open("Time,A,B\n", "empty.csv", opt);
        REQUIRE(o.ok());
        const LayerInfo& l = o->probe.layers[0];
        REQUIRE(l.channels.size() == 2);
        CHECK(l.channels[0].loggedName == "A");
        CHECK(l.channels[1].loggedName == "B");
        CHECK(l.channels[0].sampleCount == 0);
        CHECK(hasCode(o->probe.warnings, "text.no_data_rows"));
        CHECK(store::seriesValues(channel(*o, "A").series).size() == 0);
    }
    {
        // A preamble, blank lines, names and a units row.
        auto o = env.open("Venue,Somewhere\n\nTime,A,B\ns,m,km/h\n\n", "empty.csv", opt);
        REQUIRE(o.ok());
        const LayerInfo& l = o->probe.layers[0];
        REQUIRE(l.channels.size() == 2);
        CHECK(l.channels[1].loggedName == "B");
        CHECK(l.channels[1].vendorUnit == "km/h");
        CHECK(detailOf(o->probe.warnings, "text.auto_units_row") == "yes");
        CHECK(hasCode(o->probe.warnings, "text.no_data_rows"));
    }
    // Prose is no header (no time column), and a file that ends in numbers is not a header-only file: the
    // @auto rule's error stands.
    auto prose = env.open("hello, world\n", "notes.txt", opt);
    REQUIRE_FALSE(prose.ok());
    CHECK(prose.error().code == ErrorCode::Format);
    auto numeric = env.open("Time,A\n1,2,3,4,5\n", "numeric.csv", opt);
    REQUIRE_FALSE(numeric.ok());
    CHECK(numeric.error().code == ErrorCode::Format);
}

TEST_CASE("text engine: a cancelled open stops pass 1", "[unit]") {
    Env env(false);
    std::string text = "Time,A\n";
    for (int r = 0; r < 1000; ++r) {
        text += std::to_string(r) + "," + std::to_string(r % 7) + "\n";
    }
    const IImporter* importer = env.registry->find(kGenericCsvImporterId);
    REQUIRE(importer != nullptr);
    CancelSource cancel;
    cancel.cancel();
    ImportOptions opt;
    opt.cancel = cancel.token();
    auto stopped = importer->open(memory(text), opt);
    REQUIRE_FALSE(stopped.ok());
    CHECK(stopped.error().code == ErrorCode::Cancelled);
    // The registry hands the open's token to the importer.
    ImportOptions viaRegistry;
    viaRegistry.importerId = std::string(kGenericCsvImporterId);
    CHECK(env.registry->open(memory(text), viaRegistry, env.context(), cancel.token()).error().code ==
          ErrorCode::Cancelled);
    opt.cancel = {};
    CHECK(importer->open(memory(text), opt).ok());
}

TEST_CASE("text engine: profiles and dictionaries refuse nested quantifiers; the sniff sees 16 lines",
          "[unit]") {
    std::vector<std::string> log;
    auto nested = TextProfile::parse(
        R"({"$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "slow", "name": "S",
            "version": "1", "sniff": {"preambleRegex": "^(\\w+\\s?)+$", "confidence": 60}})",
        log);
    REQUIRE_FALSE(nested.ok());
    CHECK(nested.error().context.detail.value_or("") == "/sniff/preambleRegex");
    auto dict = Dictionary::parse(
        R"({"$schema": "urn:esses:dictionary-1", "schemaVersion": 1, "importer": "x", "version": "1",
            "entries": [{"match": {"nameRegex": "^(a+)+$"}, "name": "A"}]})",
        log);
    REQUIRE_FALSE(dict.ok());
    CHECK(dict.error().context.detail.value_or("").starts_with("/entries/0/match/nameRegex"));

    // The sniff regex is tried on the first 16 lines only.
    auto marker = TextProfile::parse(
        R"({"$schema": "urn:esses:importer-profile-1", "schemaVersion": 1, "id": "marker", "name": "M",
            "version": "1", "sniff": {"preambleRegex": "^MARKER", "extensions": [".mrk"], "confidence": 60}})",
        log);
    REQUIRE(marker.ok());
    auto imp = TextEngine::makeImporter(*marker);
    const auto confidence = [&](int line) {
        std::string text;
        for (int i = 1; i < line; ++i) {
            text += "x\n";
        }
        text += "MARKER\n";
        const std::vector<std::byte> b = bytesOf(text);
        SniffInput in;
        in.path = pathFromUtf8("a.txt");
        in.head = b;
        in.size = b.size();
        return imp->sniff(in).confidence;
    };
    CHECK(confidence(1) == 60);
    CHECK(confidence(16) == 60);
    CHECK(confidence(17) == 0);
}

TEST_CASE("text engine: a quoted field with a line break is reported", "[unit]") {
    Env env(false);
    ImportOptions opt;
    opt.importerId = std::string(kGenericCsvImporterId);
    // RFC 4180 allows it; this engine reads one line per row and says where the row was split.
    auto o = env.open("Time,\"Speed\nkm/h\",RPM\n0,1,2\n1,2,3\n", "multiline.csv", opt);
    REQUIRE(o.ok());
    const ImportLogEntry* split = entry(o->probe.warnings, "text.quoted_newline");
    REQUIRE(split != nullptr);
    CHECK(split->level == ImportLogEntry::Level::Warning);
    CHECK(split->ctx.line == std::optional<std::uint64_t>(1));
    CHECK(split->ctx.repeats == 1);
    auto plain = env.open("Time,\"Speed\",RPM\n0,1,2\n1,2,3\n", "plain.csv", opt);
    REQUIRE(plain.ok());
    CHECK_FALSE(hasCode(plain->probe.warnings, "text.quoted_newline"));
}
