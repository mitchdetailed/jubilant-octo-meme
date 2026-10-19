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

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

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
