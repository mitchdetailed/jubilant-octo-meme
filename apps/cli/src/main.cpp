// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli (05 section 9.2; 07 section 3.1, M0 command set): info, channels, laps, validate, fixture add,
// fixture verify; M2 (03 section 13): eval, maths check, maths functions, export, fixture diff. Every --json
// output is canonical JSON (02 section 6.5 rule 1). Exit status: 0 success, 1 a check failed (invalid
// document, fixture mismatch, unreadable log, an expression or maths item with an error), 2 usage or
// environment error.
#include "app_context.hpp"
#include "cli_common.hpp"
#include "export.hpp"
#include "fixture.hpp"
#include "inspect.hpp"
#include "maths_cli.hpp"
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
        app.add_subcommand("laps", "markers and ranges (outings, beacon-pair laps); --json prints laps-1");
    addInspect(laps, true);

    // validate
    ValidateOptions validate;
    std::vector<std::string> validatePaths;
    std::string validateSchema;
    CLI::App* val =
        app.add_subcommand("validate", "validate JSON documents against the schema their $schema names");
    std::string validateRoot;
    std::vector<std::string> validateExclude;
    val->add_option("paths", validatePaths, "files or directories (walked for *.json)");
    val->add_option("--schema", validateSchema, "validate against this schema file instead");
    val->add_flag("--json", validate.json, "canonical JSON report");
    val->add_flag("--verbose", validate.verbose, "list valid files too");
    val->add_flag("--all", validate.all,
                  "data/, fixtures/, tests/maths/ and schemas/ under --root, without the private checkouts");
    val->add_option("--root", validateRoot, "the repository root for --all (default: the current directory)");
    val->add_option("--exclude", validateExclude, "a directory to leave out of the walks (repeatable)");

    // eval (03 section 13, AC-M2-14)
    EvalOptions eval;
    std::vector<std::string> evalFiles;
    std::string evalOutput;
    CLI::App* evalCmd = app.add_subcommand("eval", "evaluate a maths expression over logs (03 section 13)");
    evalCmd->add_option("expression", eval.expression, "the expression, e.g. \"'Ground Speed'[km/h] * 2\"")
        ->required();
    evalCmd->add_option("files", evalFiles, "the logs")->required();
    evalCmd->add_option("--layer", eval.layer, "the layer (default Normal)");
    evalCmd->add_option("--rate", eval.rate,
                        "auto (the highest input rate), a number of Hz or num/den, exact (20.5 = 41/2 Hz)");
    evalCmd->add_option("--unit", eval.unit, "the result unit the values are reported in");
    evalCmd->add_option("--range", eval.range, "report only the samples of a range, e.g. \"Outings:Laps/3\"");
    evalCmd->add_option("--compat", eval.compat, "unaryMinusAbovePower=1, alias:<spelling>=<function>");
    evalCmd->add_option("--precision", eval.precision, "single or double");
    evalCmd->add_flag("--json", eval.json, "canonical JSON: rate, unit, diagnostics and values");
    evalCmd->add_flag("--csv", eval.csv, "t_us,value,valid rows");
    evalCmd->add_option("-o,--output", evalOutput, "write to this file");
    evalCmd->add_option("--importer", eval.importerId, "importer id; skips sniffing");
    evalCmd->add_option("--options", eval.optionsJson, "importer options as JSON");
    evalCmd->add_flag("--no-sidecar", eval.noSidecar, "ignore <log>.sidecar.json");
    evalCmd->add_flag("--timing", eval.timing, "compile and evaluation times on stderr");

    // export (05 section 9.2; 07 section 5.1: project-aware CSV with maths)
    ExportOptions exportOptions;
    std::vector<std::string> exportInputs;
    std::string exportChannels, exportOutput, exportUnits;
    CLI::App* exportCmd = app.add_subcommand(
        "export", "write logged and maths channels as CSV, with a project's maths, mappings and unit maps");
    exportCmd->add_option("inputs", exportInputs, "[<project folder or project.json>] <log>")->required();
    exportCmd->add_option("--channels", exportChannels, "comma-separated channel names (default: all)");
    exportCmd->add_option("--range", exportOptions.range, "write one range, e.g. \"Outings:Laps/3\"");
    exportCmd->add_option("--format", exportOptions.format, "csv (default) or motec.csv");
    exportCmd->add_option("--rate", exportOptions.rate,
                          "the CSV grid, Hz or num/den, at most 1 MHz (default: the highest rate, <= 1 kHz)");
    exportCmd->add_option("--units", exportUnits, "display units, e.g. \"Corr Speed=km/h,Oil Temp=deg C\"");
    exportCmd->add_option("--layer", exportOptions.layer, "the layer (default Normal)");
    exportCmd->add_option("-o,--output", exportOutput, "the CSV file")->required();
    exportCmd->add_option("--importer", exportOptions.importerId, "importer id; skips sniffing");
    exportCmd->add_option("--options", exportOptions.optionsJson, "importer options as JSON");
    exportCmd->add_flag("--no-sidecar", exportOptions.noSidecar, "ignore <log>.sidecar.json");
    exportCmd->add_flag("--json", exportOptions.json, "a canonical JSON report");

    // maths check / functions (AC-M2-01, 03 section 13)
    CLI::App* mathsCmd = app.add_subcommand("maths", "maths modules and functions");
    mathsCmd->require_subcommand(1);
    MathsCheckOptions check;
    std::vector<std::string> checkModules;
    std::string checkLog;
    CLI::App* checkCmd =
        mathsCmd->add_subcommand("check", "parse and check every item of maths-1 modules (per-item status)");
    checkCmd->add_option("modules", checkModules, "maths-1 files or directories of *.maths.json")->required();
    checkCmd->add_option("--log", checkLog, "resolve names against this log (else without data)");
    checkCmd->add_option("--layer", check.layer, "the layer of --log (default Normal)");
    checkCmd->add_option("--compat", check.compat, "unaryMinusAbovePower=1, alias:<spelling>=<function>");
    checkCmd->add_option("--importer", check.importerId, "importer id of --log; skips sniffing");
    checkCmd->add_flag("--no-sidecar", check.noSidecar, "ignore the sidecar of --log");
    checkCmd->add_flag("--json", check.json, "canonical JSON report with summary counts");
    bool functionsJson = false;
    CLI::App* functionsCmd = mathsCmd->add_subcommand("functions", "the function registry's manifests");
    functionsCmd->add_flag("--json", functionsJson, "canonical JSON");

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

    FixtureDiffOptions diff;
    std::vector<std::string> diffDirs;
    CLI::App* diffCmd =
        fixture->add_subcommand("diff", "explain where a fixture differs from its expectations");
    diffCmd->add_option("dirs", diffDirs, "fixture directories or trees (default fixtures)");
    diffCmd->add_option("--channel", diff.channel, "only this channel");
    diffCmd->add_flag("--json", diff.json, "canonical JSON report");

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
        if (validatePaths.empty() && !validate.all) {
            std::cerr << "esses-cli: validate needs paths or --all\n";
            return 2;
        }
        validate.root = p8(validateRoot);
        for (const std::string& e : validateExclude) {
            validate.exclude.push_back(p8(e));
        }
        return runValidate(ctx, validate, std::cout, std::cerr);
    }
    if (evalCmd->parsed()) {
        for (const std::string& f : evalFiles) {
            eval.files.push_back(p8(f));
        }
        if (!evalOutput.empty()) {
            eval.output = p8(evalOutput);
        }
        return runEval(ctx, eval, std::cout, std::cerr);
    }
    if (exportCmd->parsed()) {
        for (const std::string& f : exportInputs) {
            exportOptions.inputs.push_back(p8(f));
        }
        exportOptions.channels = splitList(exportChannels);
        exportOptions.units = splitList(exportUnits);
        exportOptions.output = p8(exportOutput);
        return runExport(ctx, exportOptions, std::cout, std::cerr);
    }
    if (checkCmd->parsed()) {
        for (const std::string& f : checkModules) {
            check.modules.push_back(p8(f));
        }
        if (!checkLog.empty()) {
            check.log = p8(checkLog);
        }
        return runMathsCheck(ctx, check, std::cout, std::cerr);
    }
    if (functionsCmd->parsed()) {
        return runMathsFunctions(ctx, functionsJson, std::cout, std::cerr);
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
    if (diffCmd->parsed()) {
        for (const std::string& d : diffDirs) {
            diff.dirs.push_back(p8(d));
        }
        if (diff.dirs.empty()) {
            diff.dirs.push_back(p8("fixtures"));
        }
        return runFixtureDiff(ctx, diff, std::cout, std::cerr);
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
