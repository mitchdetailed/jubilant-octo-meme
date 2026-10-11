// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli (05 section 9.2; 07 section 3.1, M0 command set): info, channels, laps, validate, fixture add,
// fixture verify. Every --json output is canonical JSON (02 section 6.5 rule 1). Exit status: 0 success,
// 1 a check failed (invalid document, fixture mismatch, unreadable log), 2 usage or environment error.
#include "app_context.hpp"
#include "cli_common.hpp"
#include "fixture.hpp"
#include "inspect.hpp"
#include "validate.hpp"

#include <esses/base/config.hpp>
#include <esses/base/utf8.hpp>
#include <esses/base/version.hpp>

#include <CLI/CLI11.hpp>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#if defined(ESSES_OS_WINDOWS)
#include <io.h>

#include <fcntl.h>
#endif

namespace {

using namespace esses;
using namespace esses::cli;

int run(const std::vector<std::string>& args) {
    CLI::App app{"Esses command line: inspect logs, validate documents, maintain the fixture corpus"};
    app.set_version_flag("--version", std::string(versionBanner()));
    app.require_subcommand(1);
    app.fallthrough();

    std::string dataDir, unitsFile, schemaDir;
    GlobalOptions global;
    app.add_option("--data-dir", dataDir, "directory holding units.json and schemas/ (default: <exe>/data)");
    app.add_option("--units", unitsFile, "the unit table (default: <data-dir>/units.json)");
    app.add_option("--schemas", schemaDir, "directory of <name>-<major>.schema.json files");
    app.add_flag("--no-units", global.noUnits, "do not load a unit table (vendor units, no unit lookups)");

    // info / channels / laps
    InspectOptions inspect;
    std::string inspectFile;
    const auto addInspect = [&](CLI::App* cmd, bool sidecar) {
        cmd->add_option("file", inspectFile, "the log file")->required();
        cmd->add_flag("--json", inspect.json, "canonical JSON output");
        cmd->add_option("--importer", inspect.importerId, "importer id; skips sniffing (e.g. motec.ld)");
        cmd->add_option("--options", inspect.optionsJson,
                        R"(importer options as JSON, e.g. {"ld":{"shiftOrder":"beforeMul"}})");
        if (sidecar) {
            cmd->add_flag("--no-sidecar", inspect.noSidecar, "ignore <log>.sidecar.json");
        }
    };
    CLI::App* info = app.add_subcommand("info", "sniff and probe a log; --json prints probe_json (probe-1)");
    addInspect(info, false);
    info->add_flag("--timing", inspect.timing, "print the sniff + probe time on stderr");
    CLI::App* channels = app.add_subcommand("channels", "the channel table after the open pipeline");
    addInspect(channels, true);
    CLI::App* laps =
        app.add_subcommand("laps", "markers and ranges (whole-outing in M0); --json prints laps-1");
    addInspect(laps, true);

    // validate
    ValidateOptions validate;
    std::vector<std::string> validatePaths;
    std::string validateSchema;
    CLI::App* val =
        app.add_subcommand("validate", "validate JSON documents against the schema their $schema names");
    val->add_option("paths", validatePaths, "files or directories (walked for *.json)")->required();
    val->add_option("--schema", validateSchema, "validate against this schema file instead");
    val->add_flag("--json", validate.json, "canonical JSON report");
    val->add_flag("--verbose", validate.verbose, "list valid files too");

    // fixture add / verify
    CLI::App* fixture = app.add_subcommand("fixture", "maintain the fixture corpus (05 section 9.1)");
    fixture->require_subcommand(1);
    FixtureAddOptions add;
    std::string addInput, addOut, addRefDecode, addChannels, addOptions;
    CLI::App* addCmd = fixture->add_subcommand("add", "write a fixture directory with oracle expectations");
    addCmd->add_option("file", addInput, "the input log")->required();
    addCmd->add_option("--format", add.format, "importer id (motec.ld)")->required();
    addCmd->add_option("--oracle", add.oracle, "ref-decode, or a directory of <NNN>_<name>.csv files")
        ->required();
    addCmd->add_option("--out", addOut, "the fixture directory")->required();
    addCmd->add_option("--name", add.name, "fixture name (default: the directory name)");
    addCmd->add_option("--source", add.source, "provenance prose for fixture.json");
    addCmd->add_option("--provenance", add.provenance,
                       "synthetic | user-device-log | user-tool-output | third-party | vendor-sample");
    addCmd->add_option("--licence", add.licence,
                       "SPDX licence of the input (required for a public fixture; --private defaults to "
                       "LicenseRef-unknown)");
    addCmd->add_flag("--private", add.isPrivate, "a private fixture (fixtures/private)");
    addCmd->add_option("--permission", add.permission, "why the input may be used (private fixtures)");
    addCmd->add_option("--notes", add.notes, "free text for fixture.json");
    addCmd->add_option("--channels", addChannels, "comma-separated channel names to compare (default all)");
    addCmd->add_option("--options", addOptions,
                       R"(import options as JSON (default {"ld":{"shiftOrder":"afterMul"}}))");
    addCmd->add_option("--validity", add.validity,
                       "strict | finite-only (oracle.validity; default finite-only for --oracle ref-decode, "
                       "which does not decode gap blocks, else strict)");
    addCmd->add_option("--oracle-kind", add.oracleKind, "oracle kind for a directory oracle");
    addCmd->add_option("--oracle-ref", add.oracleRef, "oracle reference for a directory oracle");
    addCmd->add_option("--python", add.python,
                       "Python interpreter for ref-decode (default python / python3)");
    addCmd->add_option("--ref-decode", addRefDecode, "path of tools/ld_ref_decode.py");
    addCmd->add_flag("--quiet", add.quiet, "no summary line");
    addCmd->add_flag("--json", add.json, "the summary as canonical JSON");

    FixtureVerifyOptions verify;
    std::vector<std::string> verifyRoots, verifyManifests;
    CLI::App* verifyCmd =
        fixture->add_subcommand("verify", "re-probe, re-decode and compare with the expectations");
    verifyCmd->add_option("dirs", verifyRoots, "fixture directories or trees (searched for fixture.json)");
    verifyCmd->add_option("--manifest", verifyManifests,
                          "a manifest of fixtures whose inputs live elsewhere");
    verifyCmd->add_flag("--if-present", verify.ifPresent,
                        "a missing manifest is skipped (private), not an error");
    verifyCmd->add_flag("--update", verify.update,
                        "rewrite probe/markers/laps expectations, hashes and dialect");
    verifyCmd->add_flag("--json", verify.json, "canonical JSON report");
    verifyCmd->add_flag("--verbose", verify.verbose, "print notes");

    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const std::string& a : args) {
        argv.push_back(a.c_str());
    }
    try {
        app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError& e) {
        const int rc = app.exit(e);
        return rc == 0 ? 0 : 2;
    }

    if (!dataDir.empty()) {
        global.dataDir = p8(dataDir);
    }
    if (!unitsFile.empty()) {
        global.unitsFile = p8(unitsFile);
    }
    if (!schemaDir.empty()) {
        global.schemaDir = p8(schemaDir);
    }
    AppContext ctx(global);
    inspect.file = p8(inspectFile);
    if (info->parsed()) {
        return runInfo(ctx, inspect, std::cout, std::cerr);
    }
    if (channels->parsed()) {
        return runChannels(ctx, inspect, std::cout, std::cerr);
    }
    if (laps->parsed()) {
        return runLaps(ctx, inspect, std::cout, std::cerr);
    }
    if (val->parsed()) {
        for (const std::string& p : validatePaths) {
            validate.paths.push_back(p8(p));
        }
        if (!validateSchema.empty()) {
            validate.schemaFile = p8(validateSchema);
        }
        return runValidate(ctx, validate, std::cout, std::cerr);
    }
    if (addCmd->parsed()) {
        add.input = p8(addInput);
        add.out = p8(addOut);
        add.channels = splitList(addChannels);
        if (!addOptions.empty()) {
            add.optionsJson = addOptions;
        }
        if (!addRefDecode.empty()) {
            add.refDecode = p8(addRefDecode);
        }
        return runFixtureAdd(ctx, add, std::cout, std::cerr);
    }
    if (verifyCmd->parsed()) {
        for (const std::string& r : verifyRoots) {
            verify.roots.push_back(p8(r));
        }
        for (const std::string& m : verifyManifests) {
            verify.manifests.push_back(p8(m));
        }
        if (verify.roots.empty() && verify.manifests.empty()) {
            verify.roots.push_back(p8("fixtures"));
        }
        return runFixtureVerify(ctx, verify, std::cout, std::cerr);
    }
    std::cerr << app.help();
    return 2;
}

void binaryStdio() {
#if defined(ESSES_OS_WINDOWS)
    // Canonical JSON is LF-only: no CR LF translation on the standard streams.
    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)_setmode(_fileno(stderr), _O_BINARY);
#endif
}

int guarded(const std::vector<std::string>& args) {
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::cerr << "esses-cli: internal error: " << e.what() << "\n";
        return 2;
    }
}

} // namespace

#if defined(ESSES_OS_WINDOWS)
int wmain(int argc, wchar_t** argv) {
    binaryStdio();
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(esses::utf8::fromWide(argv[i]));
    }
    return guarded(args);
}
#else
int main(int argc, char** argv) {
    binaryStdio();
    return guarded(std::vector<std::string>(argv, argv + argc));
}
#endif
