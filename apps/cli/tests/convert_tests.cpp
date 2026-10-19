// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli convert (D-214): the output never replaces the input, its .ldx companion or its sidecar, whatever
// the spelling of the path, and the usage errors are refused before the input is read. The conversions
// themselves run end to end in cli.golden.convert (the .ld -> motec.ld -> read back round trip against
// tools/ld_ref_decode.py).
#include "app_context.hpp"
#include "cli_common.hpp"
#include "convert.hpp"

#include <esses/model/detail.hpp>
#include <esses/model/range.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

using namespace esses;
using namespace esses::cli;

namespace {

// A fresh directory with in.ld, in.ldx and in.ld.sidecar.json (unique per run: two build trees may run the
// test at the same time).
std::filesystem::path scratch(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("esses-convert-" + name + "-" + std::to_string(stamp));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "sub");
    for (const char* f : {"in.ld", "in.ldx", "in.ld.sidecar.json"}) {
        std::ofstream(dir / f, std::ios::binary) << "x";
    }
    return dir;
}

} // namespace

TEST_CASE("convert: the output never replaces the input, its companion or its sidecar", "[unit][cli]") {
    const std::filesystem::path dir = scratch("overwrite");
    const std::filesystem::path in = dir / "in.ld";
    // The input itself, for every format, and spelled another way.
    CHECK(convertWouldOverwriteInput(in, in, true));
    CHECK(convertWouldOverwriteInput(in, in, false));
    CHECK(convertWouldOverwriteInput(in, dir / "sub" / ".." / "in.ld", false));
    // Its companion and its sidecar.
    CHECK(convertWouldOverwriteInput(in, dir / "in.ldx", false));
    CHECK(convertWouldOverwriteInput(in, dir / "in.ld.sidecar.json", false));
    // A .ld output writes its own .ldx beside it: in.csv as motec.ld would write in.ldx.
    CHECK(convertWouldOverwriteInput(in, dir / "in.csv", true));
    CHECK_FALSE(convertWouldOverwriteInput(in, dir / "in.csv", false));
    // Other files are fine.
    CHECK_FALSE(convertWouldOverwriteInput(in, dir / "out.ld", true));
    CHECK_FALSE(convertWouldOverwriteInput(in, dir / "sub" / "in.ld", true));
    // Regression (PR #5 review E4, D-217): a CSV has no companion, so run.csv -> run.ld (and its run.ldx) is
    // fine; its sidecar stays protected. A .LD input's companion is protected whatever the case.
    const std::filesystem::path csv = dir / "run.csv";
    CHECK_FALSE(convertWouldOverwriteInput(csv, dir / "run.ld", true));
    CHECK(convertWouldOverwriteInput(csv, dir / "run.csv.sidecar.json", true));
    CHECK(convertWouldOverwriteInput(dir / "RUN.LD", dir / "RUN.csv", true));
    // Another case is the same file exactly when the file system says so (Windows, macOS by default).
    std::error_code ec;
    const bool caseInsensitive = std::filesystem::exists(dir / "IN.LD", ec);
    CHECK(convertWouldOverwriteInput(in, dir / "IN.LD", false) == caseInsensitive);
    // A hard link to the input is the input (where the file system has them).
    std::filesystem::create_hard_link(in, dir / "link.ld", ec);
    if (!ec) {
        CHECK(convertWouldOverwriteInput(in, dir / "link.ld", false));
    }
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("convert: usage errors are refused before the input is read", "[unit][cli]") {
    const std::filesystem::path dir = scratch("usage");
    AppContext ctx;
    ConvertOptions o;
    o.input = dir / "missing.ld"; // never opened: each case fails first
    o.output = dir / "out.ld";
    std::ostringstream out;
    std::ostringstream err;

    o.format = "parquet";
    CHECK(runConvert(ctx, o, out, err) == 2);
    CHECK(err.str().find("--format is csv, motec.csv or motec.ld") != std::string::npos);

    o.format = "motec.ld";
    o.output = dir / "missing.ldx"; // the input's companion
    err.str({});
    CHECK(runConvert(ctx, o, out, err) == 2);
    CHECK(err.str().find("refusing to overwrite the input") != std::string::npos);

    o.output = dir / "out.ld";
    o.rate = "10"; // the .ld writer does not resample in 0.1
    err.str({});
    CHECK(runConvert(ctx, o, out, err) == 2);
    CHECK(err.str().find("does not resample") != std::string::npos);

    o.format = "csv";
    o.rate = "0";
    err.str({});
    CHECK(runConvert(ctx, o, out, err) == 2);

    o.rate.clear();
    o.output.reset();
    err.str({});
    CHECK(runConvert(ctx, o, out, err) == 2);
    CHECK(err.str().find("-o <file> is required") != std::string::npos);

    // A well-formed command over a missing input is an unreadable input (exit 1), and writes nothing.
    o.output = dir / "out.csv";
    err.str({});
    CHECK(runConvert(ctx, o, out, err) == 1);
    CHECK_FALSE(std::filesystem::exists(dir / "out.csv"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

namespace {

model::Range lap(model::LapClass cls, int number, double from, double to) {
    model::Range r;
    r.kind = model::RangeKind::Lap;
    r.lapClass = cls;
    r.number = number;
    r.start = static_cast<Micros>(from * 1e6);
    r.end = static_cast<Micros>(to * 1e6);
    return r;
}

std::string textOf(const model::DataSource& s, std::string_view id) {
    const model::Detail* d = model::findDetail(s.details, id);
    const auto* text = d != nullptr ? std::get_if<std::string>(&d->value) : nullptr;
    return text != nullptr ? *text : std::string("<none>");
}

} // namespace

// Regression (PR #5 review E3, D-217): a cut carries its own Log Time, Log Date and Start Lap.
TEST_CASE("convert: a cut's details name its own start and first lap", "[unit][cli]") {
    model::DataLayer layer;
    model::RangeGroup laps = model::RangeGroup::make("Outings:Laps");
    laps.ranges = {lap(model::LapClass::Out, 0, 0, 10), lap(model::LapClass::Flying, 7, 10, 30),
                   lap(model::LapClass::Flying, 8, 30, 50.5), lap(model::LapClass::In, 9, 50.5, 60)};
    layer.ranges.roots.push_back(laps);
    const auto source = [](std::string time, std::string date) {
        model::DataSource s;
        s.details = {{"Log Time", std::move(time), model::Provenance::Logged, std::nullopt},
                     {"Log Date", std::move(date), model::Provenance::Logged, std::nullopt},
                     {"Start Lap", std::string("7"), model::Provenance::Companion, std::nullopt}};
        s.baseDetails = s.details;
        return s;
    };
    // Lap 8 cut at 30 s: 10:00:00 + 30 s, the same day; the first flying lap of the cut is 8.
    model::DataSource a = source("10:00:00", "09/10/2026");
    cutDetails(a, layer, 30'000'000);
    CHECK(textOf(a, "Log Time") == "10:00:30");
    CHECK(textOf(a, "Log Date") == "09/10/2026");
    CHECK(textOf(a, "Start Lap") == "8");
    CHECK(model::findDetail(a.baseDetails, "Start Lap") != nullptr);
    // Past midnight at the year's end, without seconds in the input.
    model::DataSource b = source("23:59", "31/12/2026");
    cutDetails(b, layer, 90'500'000); // 90.5 s rounds to 91 s
    CHECK(textOf(b, "Log Time") == "00:00:31");
    CHECK(textOf(b, "Log Date") == "01/01/2027");
    CHECK(textOf(b, "Start Lap") == "7"); // no flying lap starts in a cut from 90.5 s: Start Lap stays
    // A time that does not parse goes (it would name the input's start); a date that would move but does not
    // parse goes too; a cut from 0 keeps the first flying lap's number.
    model::DataSource c = source("noon", "31/12/2026");
    cutDetails(c, layer, 0);
    CHECK(textOf(c, "Log Time") == "<none>");
    CHECK(textOf(c, "Log Date") == "31/12/2026");
    CHECK(textOf(c, "Start Lap") == "7");
    model::DataSource d = source("23:00:00", "Tuesday");
    cutDetails(d, layer, 7'200'000'000);
    CHECK(textOf(d, "Log Time") == "01:00:00");
    CHECK(textOf(d, "Log Date") == "<none>");
}
