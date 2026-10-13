// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli eval | maths check | maths functions (03 section 13, 07 section 5.1, AC-M2-01, AC-M2-14):
//   eval            parses, type-checks and compiles an expression against each log (diagnostics on stderr,
//                   or in the JSON), evaluates it over the layer extent (03 section 6.0, D-139) and prints
//                   {t_us, value, valid} rows (--csv), canonical JSON with the rate, unit and values
//                   (--json), or a summary and the rows (text). The evaluation runs on the calling thread:
//                   the CLI may wait (03 section 7.4).
//   maths check     loads maths-1 modules into one MathGraph in the order given (the project order) and
//                   reports each item's node: resolution, shadowing, cycles, synthesised filter and
//                   scale_offset items and compile errors as the application finds them (D-188), against a
//                   log's channels, details and built-in modules with --log, else without a source (names are
//                   I-UNRESOLVED and an item reading one is checked, not compiled); per-item status Ok /
//                   Warning / Error / Disabled and summary counts (AC-M2-01: tools/maths_corpus_check.py
//                   aggregates them without names or expressions).
//   maths functions the manifests of the function registry (03 section 10.1).
#pragma once

#include "app_context.hpp"

#include <esses/base/result.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/gps_corrections.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/maths/maths_compat.hpp>
#include <esses/maths/scope.hpp>
#include <esses/model/source.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esses::cli {

struct EvalOptions {
    std::string expression;
    std::vector<std::filesystem::path> files;
    std::string layer = "Normal";
    std::string rate = "auto";       // auto | <Hz> | <num>/<den>
    std::string unit;                // the declared result unit; the values are reported in it
    std::string range;               // "Outings:Laps/3": report only the samples of that range
    std::vector<std::string> compat; // "unaryMinusAbovePower=1"
    std::string precision;           // "" | single | double
    bool json = false;
    bool csv = false;
    std::optional<std::filesystem::path> output; // -o
    std::string importerId;
    std::string optionsJson;
    bool noSidecar = false;
    bool timing = false; // compile and evaluation times on stderr
};

struct MathsCheckOptions {
    std::vector<std::filesystem::path> modules; // maths-1 files or directories (walked for *.maths.json)
    std::optional<std::filesystem::path> log;   // resolve against this log's channels and details
    std::string layer = "Normal";
    std::vector<std::string> compat;
    bool json = false;
    std::string importerId;
    bool noSidecar = false;
};

int runEval(AppContext& ctx, const EvalOptions& options, std::ostream& out, std::ostream& err);
// True when the GPS corrections module's default latitude and longitude channels are logged in `layer` (the
// application registers esses.gps_corrections only then, D-173).
bool loggedGpsInputs(const maths::GpsCorrectionsModule& gps, const model::DataLayer& layer);
int runMathsCheck(AppContext& ctx, const MathsCheckOptions& options, std::ostream& out, std::ostream& err);
int runMathsFunctions(AppContext& ctx, bool json, std::ostream& out, std::ostream& err);

// ---- shared with the tests
// ---------------------------------------------------------------------------------

// "name=value" switches of MathsCompat: unaryMinusAbovePower=0|1, alias:<spelling>=<canonical name>.
Result<maths::MathsCompat> parseCompat(const std::vector<std::string>& switches);

// A data-backed scope over one layer of a source (03 section 3.3, the subset the CLI needs): channels by
// name, then by logged name, then numeric and text details of the source, then the constants of `module`
// (own module first; a name that is also a channel resolves to the channel with W-CONST-SHADOWED). Values of
// details and constants are given in their quantity's base unit. With no source, only constants resolve
// and a miss is I-UNRESOLVED (the data-less scope of the Maths Editor).
class SourceScope final : public maths::Scope {
public:
    SourceScope(std::shared_ptr<const model::DataSource> source, std::string layer,
                const maths::MathModule* module, const maths::FunctionRegistry& functions,
                const units::UnitRegistry& units);

    std::optional<maths::ScopeEntry> lookup(std::string_view nameCf,
                                            const maths::ChannelKey& requester) const override;
    bool dataBacked() const noexcept override { return source_ != nullptr; }

private:
    std::shared_ptr<const model::DataSource> source_;
    std::string layer_;
    const maths::MathModule* module_;
};

// [{code, level, begin, end, message, hint}] (hint omitted when empty).
nlohmann::json diagnosticsJson(const maths::Diagnostics& diags);
// "E-UNIT-UNKNOWN error 4-9: unknown unit [kmh] (did you mean [km/h]?)"
std::string diagnosticText(const maths::Diagnostic& d);

} // namespace esses::cli
