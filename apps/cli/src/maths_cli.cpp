// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "maths_cli.hpp"

#include "cli_common.hpp"
#include "inspect.hpp"

#include <esses/analysis/range_tables.hpp>
#include <esses/base/casefold.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/maths/eval_context.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/gps_corrections.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/math_module_io.hpp>
#include <esses/maths/parser.hpp>
#include <esses/maths/plan.hpp>
#include <esses/maths/type_checker.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/layer.hpp>

#include <fast_float/fast_float.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace esses::cli {

namespace {

using nlohmann::json;
namespace m = esses::maths;

// Built-in module outputs registered for every source (03 section 9.1): they resolve in `maths check`.
struct BuiltinOutput {
    std::string_view name;
    std::string_view unit;
};
constexpr std::array<BuiltinOutput, 2> kBuiltinOutputs{{{"Corr Speed", "m/s"}, {"Corr Dist", "m"}}};

std::string_view levelName(m::Diagnostic::Level l) {
    switch (l) {
    case m::Diagnostic::Error:
        return "error";
    case m::Diagnostic::Warning:
        return "warning";
    case m::Diagnostic::Info:
        return "info";
    case m::Diagnostic::Style:
        return "style";
    }
    return "error";
}

std::string numberText(double v) {
    std::array<char, 40> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    return {buf.data(), r.ptr};
}

// The shortest decimal that reads back to the same F32 value (an F32 output prints as it is stored).
double shortestF32(double v) {
    std::array<char, 40> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), static_cast<float>(v));
    double out = v;
    (void)fast_float::from_chars(buf.data(), r.ptr, out);
    return out;
}

// Nominal rate of a channel (03 section 6.4 rule 2, as MathGraph reads it, D-185): the descriptor's, else for
// a timestamped series one over its mean interval rounded to whole Hz (a period below 1 Hz).
std::optional<Rate> nominalRate(const model::Channel& c) {
    if (c.desc.rate) {
        return c.desc.rate;
    }
    if (const auto* ts = std::get_if<store::TimestampedSeries>(&c.series)) {
        if (ts->size() > 1 && ts->tMax > ts->tMin) {
            const double interval =
                static_cast<double>(ts->tMax - ts->tMin) / static_cast<double>(ts->size() - 1);
            const double hz = 1e6 / interval;
            if (hz >= 1.0) {
                return Rate{static_cast<std::uint32_t>(std::llround(std::min(hz, 4e9))), 1};
            }
            return Rate::fromPeriodUs(static_cast<std::uint64_t>(std::llround(interval)));
        }
    }
    return Rate{};
}

// The layer's highest rate over every channel, timestamped ones included (03 section 6.4 rule 3).
Rate highestRate(const model::DataLayer& layer) {
    Rate best{};
    for (const model::Channel& c : layer.channels) {
        const std::optional<Rate> r = nominalRate(c);
        if (r && r->valid() && (!best.valid() || best < *r)) {
            best = *r;
        }
    }
    return best;
}

units::UnitId baseOf(units::UnitId u, const units::UnitRegistry& units) {
    if (u == units::kNoUnit || !units.contains(u)) {
        return units::kNoUnit;
    }
    return units.quantity(units.quantityOf(u)).base;
}

void printDiagnostics(const m::Diagnostics& diags, std::string_view where, std::ostream& err) {
    for (const m::Diagnostic& d : diags) {
        err << where << diagnosticText(d) << "\n";
    }
}

std::size_t countLevel(const m::Diagnostics& diags, m::Diagnostic::Level level) {
    return static_cast<std::size_t>(
        std::count_if(diags.begin(), diags.end(), [&](const m::Diagnostic& d) { return d.level == level; }));
}

Result<void> emit(const std::string& text, const std::optional<std::filesystem::path>& output,
                  std::ostream& out) {
    if (output) {
        return writeFile(*output, text);
    }
    out << text;
    return ok();
}

// "Outings:Laps/3" -> [start, end) of lap 3 (by number, else the third range of the group).
Result<std::pair<Micros, Micros>> findRange(const model::DataLayer& layer, const std::string& spec) {
    const std::size_t slash = spec.rfind('/');
    if (slash == std::string::npos) {
        return fail(ErrorCode::Invalid, "--range is <group path>/<number>, e.g. \"Outings:Laps/3\"");
    }
    const std::string path = spec.substr(0, slash);
    int number = 0;
    const std::string n = spec.substr(slash + 1);
    const auto r = std::from_chars(n.data(), n.data() + n.size(), number);
    if (r.ec != std::errc() || r.ptr != n.data() + n.size()) {
        return fail(ErrorCode::Invalid, "--range: '" + n + "' is not a number");
    }
    const model::RangeGroup* group = layer.ranges.find(path);
    if (group == nullptr) {
        return fail(ErrorCode::NotFound, "--range: no range group '" + path + "' in layer " + layer.name);
    }
    for (const model::Range& range : group->ranges) {
        if (range.enabled && range.number == number) {
            return std::make_pair(range.start, range.end);
        }
    }
    if (number >= 1 && static_cast<std::size_t>(number) <= group->ranges.size()) {
        const model::Range& range = group->ranges[static_cast<std::size_t>(number - 1)];
        return std::make_pair(range.start, range.end);
    }
    return fail(ErrorCode::NotFound, "--range: '" + path + "' has no range " + n);
}

// ---- maths check
// ---------------------------------------------------------------------------------------------

struct ItemCheck {
    std::string status; // Ok | Warning | Error | Disabled
    m::Diagnostics diags;
    bool parseError = false;
};

std::string statusOf(const m::Diagnostics& diags) {
    if (m::hasError(diags)) {
        return "Error";
    }
    if (countLevel(diags, m::Diagnostic::Warning) + countLevel(diags, m::Diagnostic::Style) > 0) {
        return "Warning";
    }
    return "Ok";
}

// A module's condition, type-checked against the scope (the graph evaluates it; this reports its
// diagnostics).
ItemCheck checkSource(const std::string& source, const m::Scope& scope, const m::MathsCompat& compat) {
    ItemCheck c;
    Result<m::Ast> ast = m::Parser::parse(source, compat, c.diags);
    if (!ast) {
        c.parseError = true;
        c.status = "Error";
        return c;
    }
    (void)m::TypeChecker::check(*ast, scope, c.diags);
    c.status = statusOf(c.diags);
    return c;
}

// Whether the text the user wrote for an item (an expression, a table axis) parses; synthesised filter and
// scale_offset expressions are not the user's text.
bool parsesWithError(const m::MathItem& item, const m::MathsCompat& compat) {
    const std::string* text = item.kind == m::ItemKind::Expression ? &item.expr
                              : item.kind == m::ItemKind::Table2D  ? &item.table.x.expr
                                                                   : nullptr;
    if (text == nullptr) {
        return false;
    }
    m::Diagnostics ignored;
    return !m::Parser::parse(*text, compat, ignored).ok();
}

// One item's row from its MathGraph node (03 section 7.3 status, the definition's diagnostics).
ItemCheck checkNode(const m::MathNodeInfo& n) {
    ItemCheck c;
    c.diags = n.status.diags;
    switch (n.status.state) {
    case m::MathState::Disabled:
        c.status = "Disabled";
        break;
    case m::MathState::Error:
        c.status = "Error";
        break;
    default:
        c.status = statusOf(c.diags); // Pending/Ok/Warning: warnings and style findings make it Warning
        break;
    }
    return c;
}

std::string_view itemKindText(m::ItemKind k) {
    switch (k) {
    case m::ItemKind::Expression:
        return "expression";
    case m::ItemKind::Filter:
        return "filter";
    case m::ItemKind::ScaleOffset:
        return "scale_offset";
    case m::ItemKind::Table2D:
        return "table2d";
    case m::ItemKind::Table3D:
        return "table3d";
    case m::ItemKind::Builtin:
        return "builtin";
    case m::ItemKind::Plugin:
        return "plugin";
    case m::ItemKind::Regression:
        return "regression";
    }
    return "expression";
}

std::vector<std::filesystem::path> moduleFiles(const std::vector<std::filesystem::path>& inputs) {
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::path& p : inputs) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            std::vector<std::filesystem::path> found;
            for (const auto& e : std::filesystem::recursive_directory_iterator(p, ec)) {
                const std::string name = u8(e.path().filename());
                if (e.is_regular_file() && name.size() > 11 && name.ends_with(".maths.json")) {
                    found.push_back(e.path());
                }
            }
            std::sort(found.begin(), found.end());
            files.insert(files.end(), found.begin(), found.end());
        } else {
            files.push_back(p);
        }
    }
    return files;
}

} // namespace

// ---- shared helpers
// ----------------------------------------------------------------------------------------

Result<m::MathsCompat> parseCompat(const std::vector<std::string>& switches) {
    m::MathsCompat compat;
    for (const std::string& s : switches) {
        const std::size_t eq = s.find('=');
        if (eq == std::string::npos) {
            return fail(ErrorCode::Invalid, "--compat takes name=value, not '" + s + "'");
        }
        const std::string key = s.substr(0, eq);
        const std::string value = s.substr(eq + 1);
        if (key == "unaryMinusAbovePower") {
            if (value == "1" || value == "true") {
                compat.unaryMinusAbovePower = true;
            } else if (value == "0" || value == "false") {
                compat.unaryMinusAbovePower = false;
            } else {
                return fail(ErrorCode::Invalid, "unaryMinusAbovePower is 0 or 1");
            }
        } else if (key.starts_with("alias:") && key.size() > 6 && !value.empty()) {
            compat.functionAliases[caseFold(key.substr(6))] = value;
        } else {
            return fail(ErrorCode::Invalid, "unknown --compat switch '" + key + "'");
        }
    }
    return compat;
}

SourceScope::SourceScope(std::shared_ptr<const model::DataSource> source, std::string layer,
                         const m::MathModule* module, const m::FunctionRegistry& functions,
                         const units::UnitRegistry& units)
    : Scope(functions, units), source_(std::move(source)), layer_(std::move(layer)), module_(module) {}

std::optional<m::ScopeEntry> SourceScope::lookup(std::string_view nameCf,
                                                 const m::ChannelKey& /*requester*/) const {
    const units::UnitRegistry& reg = units();
    const m::MathConstant* constant = nullptr;
    if (module_ != nullptr) {
        for (const m::MathConstant& c : module_->constants) {
            if (c.enabled && caseFold(c.name) == nameCf) {
                constant = &c;
                break;
            }
        }
    }
    const model::DataLayer* layer = source_ != nullptr ? source_->layer(layer_) : nullptr;
    const model::Channel* ch = nullptr;
    if (layer != nullptr) {
        ch = layer->find(nameCf);
        if (ch == nullptr) {
            ch = layer->findLogged(nameCf);
        }
    }
    if (ch != nullptr) {
        m::ScopeEntry e;
        e.what = m::ScopeEntry::Channel;
        e.type.kind = m::Kind::Series;
        e.type.unit = ch->desc.unit;
        e.type.rate = nominalRate(*ch);
        e.type.integer = ch->desc.dtype == store::Dtype::I32 || ch->desc.kind == model::SampleKind::Integer ||
                         ch->desc.kind == model::SampleKind::Enum ||
                         ch->desc.kind == model::SampleKind::Bool ||
                         ch->desc.kind == model::SampleKind::Bitfield;
        e.key = m::ChannelKey::make(source_->id, layer_, ch->desc.name);
        e.resolvedName = ch->desc.name;
        e.interp = ch->desc.interp;
        e.dtype = ch->desc.dtype;
        if (constant != nullptr) {
            e.diags.push_back(
                m::Diagnostic{"W-CONST-SHADOWED",
                              m::Diagnostic::Warning,
                              {},
                              "'" + ch->desc.name + "' is a channel and a constant: the channel is read",
                              {}});
        }
        return e;
    }
    // Outputs of the module's items and of the built-in modules: maths channels (not evaluated here).
    const auto output = [&](const std::string& name, units::UnitId unit, std::optional<Rate> rate) {
        m::ScopeEntry e;
        e.what = m::ScopeEntry::Channel;
        e.type.kind = m::Kind::Series;
        e.type.unit = baseOf(unit, reg);
        e.type.rate = rate.value_or(Rate{});
        e.key = m::ChannelKey::make(source_ != nullptr ? source_->id : model::SourceId{}, layer_, name);
        e.resolvedName = name;
        return e;
    };
    if (module_ != nullptr) {
        for (const m::MathItem& item : module_->items) {
            if (item.enabled && !item.name.empty() && caseFold(item.name) == nameCf &&
                (item.kind == m::ItemKind::Expression || item.kind == m::ItemKind::Table2D)) {
                return output(item.name, item.resultUnit,
                              item.rate.automatic ? std::nullopt : std::optional<Rate>(item.rate.rate));
            }
        }
    }
    for (const BuiltinOutput& b : kBuiltinOutputs) {
        if (caseFold(b.name) == nameCf) {
            return output(std::string(b.name), reg.find(b.unit).value_or(units::kNoUnit), std::nullopt);
        }
    }
    if (source_ != nullptr) {
        if (const model::Detail* d = model::findDetail(source_->details, nameCf)) {
            m::ScopeEntry e;
            e.resolvedName = d->id;
            if (const auto* text = std::get_if<std::string>(&d->value)) {
                e.what = m::ScopeEntry::DetailText;
                e.type.kind = m::Kind::Text;
                e.text = *text;
            } else if (const auto* dt = std::get_if<model::DateTime>(&d->value)) {
                e.what = m::ScopeEntry::DetailText;
                e.type.kind = m::Kind::Text;
                e.text = dt->iso();
            } else if (const auto* q = std::get_if<model::Quantityd>(&d->value)) {
                e.what = m::ScopeEntry::DetailNumber;
                e.type.kind = m::Kind::Scalar;
                e.type.unit = baseOf(q->unit, reg);
                e.number = model::toBase(*q, reg).value_or(q->v);
            }
            return e;
        }
    }
    if (constant != nullptr) {
        m::ScopeEntry e;
        e.what = m::ScopeEntry::Constant;
        e.type.kind = m::Kind::Scalar;
        e.type.unit = baseOf(constant->value.unit, reg);
        e.number = reg.toBase(constant->value.unit)(constant->value.v);
        e.resolvedName = constant->name;
        return e;
    }
    return std::nullopt;
}

json diagnosticsJson(const m::Diagnostics& diags) {
    json out = json::array();
    for (const m::Diagnostic& d : diags) {
        json j{{"code", d.code},
               {"level", levelName(d.level)},
               {"begin", d.span.begin},
               {"end", d.span.end},
               {"message", d.message}};
        if (!d.hint.empty()) {
            j["hint"] = d.hint;
        }
        out.push_back(std::move(j));
    }
    return out;
}

std::string diagnosticText(const m::Diagnostic& d) {
    std::string s = d.code + " " + std::string(levelName(d.level)) + " " + std::to_string(d.span.begin) +
                    "-" + std::to_string(d.span.end) + ": " + d.message;
    if (!d.hint.empty()) {
        s += " (" + d.hint + ")";
    }
    return s;
}

// ---- eval
// ------------------------------------------------------------------------------------------------------

int runEval(AppContext& ctx, const EvalOptions& o, std::ostream& out, std::ostream& err) {
    if (o.json && o.csv) {
        err << "esses-cli eval: --json and --csv exclude each other\n";
        return 2;
    }
    Result<const units::UnitRegistry*> unitsResult = ctx.units();
    if (!unitsResult || *unitsResult == nullptr) {
        err << "esses-cli eval: the unit table is needed ("
            << (unitsResult ? "--no-units" : describe(unitsResult.error())) << ")\n";
        return 2;
    }
    const units::UnitRegistry& units = **unitsResult;
    const std::shared_ptr<const units::UnitRegistry> sharedUnits(&units, [](const units::UnitRegistry*) {});
    Result<m::MathsCompat> compat = parseCompat(o.compat);
    if (!compat) {
        err << "esses-cli eval: " << compat.error().message << "\n";
        return 2;
    }
    m::MathItem item;
    item.name = "eval";
    if (!o.rate.empty() && o.rate != "auto") {
        const Result<Rate> rate = parseRate(o.rate); // exact (D-008): "20.5/1" is 41/2 Hz
        if (!rate) {
            err << "esses-cli eval: --rate '" << o.rate << "': " << rate.error().message
                << " (auto, a number of Hz or num/den)\n";
            return 2;
        }
        item.rate.automatic = false;
        item.rate.rate = *rate;
    }
    std::optional<units::UnitId> outUnit;
    if (!o.unit.empty()) {
        outUnit = units.find(o.unit);
        if (!outUnit) {
            err << "esses-cli eval: unknown unit '" << o.unit << "'\n";
            return 2;
        }
        item.resultUnit = *outUnit;
    }
    if (!o.precision.empty()) {
        if (o.precision != "single" && o.precision != "double") {
            err << "esses-cli eval: --precision is single or double\n";
            return 2;
        }
        item.precision = o.precision;
    }

    m::Diagnostics parseDiags;
    Result<m::Ast> ast = m::Parser::parse(o.expression, *compat, parseDiags);
    json doc{{"expression", o.expression}};
    if (!ast) {
        printDiagnostics(parseDiags, "", err);
        if (o.json) {
            doc["diagnostics"] = diagnosticsJson(parseDiags);
            doc["results"] = json::array();
            if (Result<std::string> text = canonicalText(doc); text) {
                (void)emit(*text, o.output, out);
            }
        }
        return 1;
    }
    doc["canonical"] = m::print(*ast);
    doc["diagnostics"] = diagnosticsJson(parseDiags);
    if (!o.json) {
        printDiagnostics(parseDiags, "", err);
    }

    Result<formats::ImportOptions> importOptions = makeImportOptions(o.importerId, o.optionsJson);
    if (!importOptions) {
        err << "esses-cli eval: " << describe(importOptions.error()) << "\n";
        return 2;
    }
    int status = 0;
    json results = json::array();
    std::ostringstream text;
    for (const std::filesystem::path& file : o.files) {
        json r{{"file", u8(file)}, {"layer", o.layer}};
        const auto failed = [&](const std::string& message, const m::Diagnostics& diags) {
            err << u8(file) << ": " << message << "\n";
            r["error"] = message;
            r["diagnostics"] = diagnosticsJson(diags);
            results.push_back(r);
            status = 1;
        };
        Result<formats::OpenedSource> opened = openFile(ctx, file, *importOptions, true, !o.noSidecar, false);
        if (!opened) {
            failed(describe(opened.error()), {});
            continue;
        }
        const model::DataLayer* layer = opened->source->layer(o.layer);
        if (layer == nullptr) {
            failed("no layer '" + o.layer + "'", {});
            continue;
        }
        const SourceScope scope(opened->source, layer->name, nullptr, m::FunctionRegistry::builtin(), units);
        m::Diagnostics diags;
        const auto t0 = std::chrono::steady_clock::now();
        Result<m::Plan> plan = m::compile(*ast, scope, item, *compat, diags);
        const auto t1 = std::chrono::steady_clock::now();
        if (!o.json) {
            printDiagnostics(diags, u8(file) + ": ", err);
        }
        if (!plan) {
            failed(plan.error().message, diags);
            continue;
        }
        const std::optional<Rate>& planRate = plan->out.rate;
        Rate resultRate = highestRate(*layer);
        if (planRate.has_value() && planRate->valid()) {
            resultRate = *planRate;
        }
        if (!resultRate.valid()) {
            failed("the layer has no channel with a rate to evaluate at", diags);
            continue;
        }
        const std::uint64_t count =
            layer->duration > 0 ? static_cast<std::uint64_t>(resultRate.indexAt(layer->duration - 1, 0)) + 1
                                : 0;
        m::EvalContext ectx;
        ectx.source = opened->source;
        ectx.layer = layer->name;
        ectx.resultRate = resultRate;
        ectx.resultFirst = 0;
        ectx.resultCount = count;
        ectx.mathStartUs = 0;
        ectx.mathEndUs = layer->duration;
        ectx.ranges = analysis::RangeTables::build(*layer);
        ectx.units = sharedUnits;
        const std::shared_ptr<const model::DataSource> source = opened->source;
        const std::string layerName = layer->name;
        ectx.getChannel = [source, layerName, &units](const m::ChannelKey& key, units::UnitId unit,
                                                      Rate gridRate) -> Result<m::MathInputGatherer> {
            const model::DataLayer* l = source->layer(layerName);
            const model::Channel* ch = l != nullptr ? l->find(key.nameCf) : nullptr;
            if (ch == nullptr && l != nullptr) {
                ch = l->findLogged(key.nameCf);
            }
            if (ch == nullptr) {
                return fail(ErrorCode::NotFound, "no channel '" + key.nameCf + "'");
            }
            units::Converter conv;
            if (unit != ch->desc.unit && unit != units::kNoUnit && ch->desc.unit != units::kNoUnit) {
                Result<units::Converter> c = units.converter(ch->desc.unit, unit);
                if (!c) {
                    return Failure{c.error()};
                }
                conv = *c;
            }
            return m::MathInputGatherer(ch->series, gridRate, 0, ch->desc.interp, conv, CancelToken{},
                                        Priority::Interactive);
        };
        Result<analysis::UniformSignal> signal = m::evaluateWhole(*plan, ectx);
        const auto t2 = std::chrono::steady_clock::now();
        if (!signal) {
            failed(describe(signal.error()), diags);
            continue;
        }
        if (o.timing) {
            err << u8(file) << ": compile " << std::chrono::duration<double, std::milli>(t1 - t0).count()
                << " ms, evaluate " << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms, "
                << count << " samples at " << resultRate.toString() << "\n";
        }

        // The rows to report (--range) and the unit they are in.
        std::size_t first = 0;
        std::size_t end = signal->values.size();
        if (!o.range.empty()) {
            Result<std::pair<Micros, Micros>> window = findRange(*layer, o.range);
            if (!window) {
                failed(window.error().message, diags);
                continue;
            }
            const auto at = [&](Micros t) {
                return t <= signal->t0
                           ? std::size_t{0}
                           : std::min(signal->values.size(),
                                      static_cast<std::size_t>(resultRate.indexAt(t - 1, signal->t0)) + 1);
            };
            first = at(window->first);
            end = std::max(first, at(window->second));
        }
        units::Converter display;
        std::string unitText;
        if (outUnit) {
            display = units.fromBase(*outUnit);
            unitText = units.unit(*outUnit).symbol;
        } else if (plan->out.unit != units::kNoUnit && units.contains(plan->out.unit)) {
            unitText = units.unit(plan->out.unit).symbol;
        }
        const bool f32 = plan->outDtype == store::Dtype::F32;
        std::size_t valid = 0;
        json values = json::array();
        if (!o.json) {
            if (!o.csv) {
                text << "file " << u8(file) << "\n"
                     << "layer " << layer->name << ", rate " << resultRate.toString() << ", unit "
                     << (unitText.empty() ? std::string("(none)") : unitText) << ", dtype "
                     << store::dtypeName(plan->outDtype) << ", " << (end - first) << " samples\n";
            }
            text << (o.csv ? "t_us,value,valid\n" : "t_us value valid\n");
        }
        for (std::size_t i = first; i < end; ++i) {
            const bool isValid = signal->validAt(i);
            double v = signal->values[i];
            if (isValid) {
                ++valid;
                v = display(v);
                if (f32) {
                    v = shortestF32(v);
                }
            }
            if (o.json) {
                values.push_back(isValid ? json(v) : json(nullptr));
            } else {
                const Micros t = resultRate.timeAt(static_cast<SampleIdx>(i), 0);
                text << t << (o.csv ? "," : " ") << (isValid ? numberText(v) : std::string())
                     << (o.csv ? "," : " ") << (isValid ? 1 : 0) << "\n";
            }
        }
        r["rate"] = json{{"num", resultRate.num}, {"den", resultRate.den}};
        r["hz"] = resultRate.hz();
        r["t0Us"] = resultRate.timeAt(static_cast<SampleIdx>(first), 0);
        r["firstIndex"] = first;
        r["count"] = end - first;
        r["validCount"] = valid;
        r["unit"] = unitText;
        r["dtype"] = std::string(store::dtypeName(plan->outDtype));
        r["diagnostics"] = diagnosticsJson(diags);
        r["values"] = std::move(values);
        results.push_back(std::move(r));
    }
    if (o.json) {
        doc["results"] = std::move(results);
        Result<std::string> textDoc = canonicalText(doc);
        if (!textDoc) {
            err << "esses-cli eval: " << describe(textDoc.error()) << "\n";
            return 2;
        }
        if (Result<void> w = emit(*textDoc, o.output, out); !w) {
            err << "esses-cli eval: " << describe(w.error()) << "\n";
            return 2;
        }
        return status;
    }
    if (Result<void> w = emit(text.str(), o.output, out); !w) {
        err << "esses-cli eval: " << describe(w.error()) << "\n";
        return 2;
    }
    return status;
}

// ---- maths check
// -----------------------------------------------------------------------------------------------

bool loggedGpsInputs(const m::GpsCorrectionsModule& gps, const model::DataLayer& layer) {
    const json s = json::parse(gps.defaultSettings().compact(), nullptr, false);
    const auto logged = [&](const char* key) {
        const auto it = s.find(key);
        if (it == s.end() || !it->is_string()) {
            return false;
        }
        const std::string name = it->get<std::string>();
        return !name.empty() && (layer.find(name) != nullptr || layer.findLogged(name) != nullptr);
    };
    return s.is_object() && logged("latitude") && logged("longitude");
}

int runMathsCheck(AppContext& ctx, const MathsCheckOptions& o, std::ostream& out, std::ostream& err) {
    Result<const units::UnitRegistry*> unitsResult = ctx.units();
    if (!unitsResult || *unitsResult == nullptr) {
        err << "esses-cli maths check: the unit table is needed\n";
        return 2;
    }
    const units::UnitRegistry& units = **unitsResult;
    const std::shared_ptr<const units::UnitRegistry> sharedUnits(&units, [](const units::UnitRegistry*) {});
    Result<m::MathsCompat> compat = parseCompat(o.compat);
    if (!compat) {
        err << "esses-cli maths check: " << compat.error().message << "\n";
        return 2;
    }
    std::shared_ptr<const model::DataSource> source;
    std::optional<formats::OpenedSource> opened;
    if (o.log) {
        Result<formats::ImportOptions> importOptions = makeImportOptions(o.importerId, "");
        if (!importOptions) {
            err << "esses-cli maths check: " << describe(importOptions.error()) << "\n";
            return 2;
        }
        Result<formats::OpenedSource> r = openFile(ctx, *o.log, *importOptions, true, !o.noSidecar, false);
        if (!r) {
            err << "esses-cli maths check: " << describe(r.error()) << "\n";
            return 1;
        }
        opened = std::move(r).value();
        source = opened->source;
        if (source->layer(o.layer) == nullptr) {
            err << "esses-cli maths check: no layer '" << o.layer << "'\n";
            return 2;
        }
    }

    std::map<std::string, std::size_t> byStatus{{"Ok", 0}, {"Warning", 0}, {"Error", 0}, {"Disabled", 0}};
    std::map<std::string, std::size_t> byCode;
    std::size_t items = 0;
    std::size_t parseErrors = 0;
    std::size_t moduleErrors = 0;
    json modules = json::array();
    std::ostringstream text;

    // Every module goes into one MathGraph in the order given (the project order of 03 section 4.3), so the
    // check resolves, shadows, detects cycles, synthesises filter and scale_offset items and compiles exactly
    // as the application does (D-188). Without --log the graph has no source: names are I-UNRESOLVED and an
    // item that reads one is checked, not compiled. A module id given twice is renamed inside the graph only.
    struct Loaded {
        std::filesystem::path file;
        std::optional<m::MathModule> module;
        std::string graphId;
        std::string error;
    };
    std::vector<Loaded> loaded;
    std::vector<m::MathModule> globals;
    std::set<std::string> ids;
    for (const std::filesystem::path& file : moduleFiles(o.modules)) {
        Loaded l;
        l.file = file;
        std::vector<std::string> log;
        Result<m::MathModule> module = m::MathModuleIo::load(file, units, log);
        if (!module) {
            l.error = describe(module.error());
            loaded.push_back(std::move(l));
            continue;
        }
        l.graphId = module->id;
        for (int k = 2; !ids.insert(caseFold(l.graphId)).second; ++k) {
            l.graphId = module->id + "#" + std::to_string(k);
        }
        m::MathModule copy = *module;
        copy.id = l.graphId;
        copy.origin = m::MathModule::Origin::Global;
        globals.push_back(std::move(copy));
        l.module = std::move(module).value();
        loaded.push_back(std::move(l));
    }
    const model::DataLayer* dataLayer = source != nullptr ? source->layer(o.layer) : nullptr;
    m::MathGraph graph(
        source, dataLayer != nullptr ? std::string(model::canonicalLayerName(dataLayer->name)) : o.layer,
        sharedUnits, *compat);
    m::CorrectedSpeedDistanceModule corrected;
    m::GpsCorrectionsModule gps;
    if (dataLayer != nullptr) {
        // The built-in modules the application registers for this source (03 section 9, D-173).
        if (Result<std::filesystem::path> schemas = ctx.schemaDir(); schemas) {
            static_cast<void>(corrected.loadSchema(*schemas));
            static_cast<void>(gps.loadSchema(*schemas));
        }
        std::vector<const m::IBuiltinModule*> builtins{&corrected};
        if (loggedGpsInputs(gps, *dataLayer)) {
            builtins.push_back(&gps);
        }
        for (const m::IBuiltinModule* module : builtins) {
            m::Diagnostics diags;
            // Its diagnostics belong to no item row: counted here, and a module that does not register is a
            // module error (the reason on stderr).
            Result<std::vector<m::ChannelKey>> keys = graph.defineBuiltin(*module, {}, diags);
            for (const m::Diagnostic& d : diags) {
                ++byCode[d.code];
            }
            if (!keys) {
                err << "esses-cli maths check: built-in module " << module->id() << ": "
                    << describe(keys.error()) << "\n";
                ++moduleErrors;
            }
        }
    }
    static_cast<void>(graph.setModules(globals, nullptr, nullptr));
    const std::vector<m::MathNodeInfo> nodes = graph.nodes();

    for (const Loaded& l : loaded) {
        json mj{{"file", u8(l.file)}};
        if (!l.module) {
            mj["error"] = l.error;
            modules.push_back(std::move(mj));
            text << u8(l.file) << ": " << l.error << "\n";
            ++moduleErrors;
            continue;
        }
        const m::MathModule& module = *l.module;
        mj["id"] = module.id;
        text << "module " << module.id << " (" << u8(l.file) << ")\n";
        // The module's nodes in item order (the graph makes one per item with a name, built-in items aside).
        std::vector<const m::MathNodeInfo*> mine;
        for (const m::MathNodeInfo& n : nodes) {
            if (n.moduleId == l.graphId && n.origin == m::MathModule::Origin::Global) {
                mine.push_back(&n);
            }
        }
        if (module.condition) {
            // The condition is a row of its own, counted like an item (codes, parse errors) and, in error,
            // as a module error: a module with no named item has no node that could carry it.
            const SourceScope scope(source, o.layer, &module, m::FunctionRegistry::builtin(), units);
            ItemCheck cond = checkSource(*module.condition, scope, *compat);
            if (!m::hasError(cond.diags)) {
                // The application's verdict (03 section 4.3 rule 8): a condition the graph cannot fold over
                // the details disables the module's items with E-CONDITION, which the type check above may
                // pass (with --log, a condition that reads a channel).
                for (const m::MathNodeInfo* n : mine) {
                    for (const m::Diagnostic& d : n->status.diags) {
                        if (d.code == "E-CONDITION" && d.level == m::Diagnostic::Error) {
                            cond.diags.push_back(d);
                        }
                    }
                    if (m::hasError(cond.diags)) {
                        cond.status = "Error";
                        break;
                    }
                }
            }
            if (!module.enabled) {
                cond.status = "Disabled"; // a disabled module's condition is not evaluated
            }
            for (const m::Diagnostic& d : cond.diags) {
                ++byCode[d.code];
            }
            if (cond.parseError && module.enabled) {
                ++parseErrors;
            }
            if (cond.status == "Error") {
                ++moduleErrors;
            }
            mj["condition"] = json{{"status", cond.status}, {"diagnostics", diagnosticsJson(cond.diags)}};
            text << "  " << cond.status << "  (condition)\n";
            for (const m::Diagnostic& d : cond.diags) {
                text << "      " << diagnosticText(d) << "\n";
            }
        }
        std::size_t next = 0;
        json itemsJson = json::array();
        for (std::size_t i = 0; i < module.items.size(); ++i) {
            const m::MathItem& item = module.items[i];
            ItemCheck c;
            if (item.kind == m::ItemKind::Builtin) {
                c.status =
                    item.enabled ? "Ok" : "Disabled"; // settings for a registered module (03 section 9)
            } else if (item.name.empty()) {
                c.diags.push_back(m::Diagnostic{
                    "E-NAME-EMPTY", m::Diagnostic::Error, {}, "the item has no output name", {}});
                c.status = item.enabled ? "Error" : "Disabled";
            } else if (next < mine.size()) {
                c = checkNode(*mine[next++]);
            } else {
                c.status = "Error"; // not reached: every named item is a node
            }
            c.parseError = parsesWithError(item, *compat);
            ++items;
            ++byStatus[c.status];
            if (c.parseError && item.enabled) {
                ++parseErrors;
            }
            for (const m::Diagnostic& d : c.diags) {
                ++byCode[d.code];
            }
            itemsJson.push_back(json{{"index", i},
                                     {"name", item.name},
                                     {"kind", std::string(itemKindText(item.kind))},
                                     {"status", c.status},
                                     {"diagnostics", diagnosticsJson(c.diags)}});
            text << "  " << c.status << "  " << item.name << "\n";
            for (const m::Diagnostic& d : c.diags) {
                text << "      " << diagnosticText(d) << "\n";
            }
        }
        mj["items"] = std::move(itemsJson);
        modules.push_back(std::move(mj));
    }
    json summary{{"modules", modules.size()},
                 {"moduleErrors", moduleErrors},
                 {"items", items},
                 {"ok", byStatus["Ok"]},
                 {"warning", byStatus["Warning"]},
                 {"error", byStatus["Error"]},
                 {"disabled", byStatus["Disabled"]},
                 {"parseErrors", parseErrors},
                 {"codes", byCode}};
    if (o.json) {
        Result<std::string> doc = canonicalText(json{{"modules", std::move(modules)}, {"summary", summary}});
        if (!doc) {
            err << "esses-cli maths check: " << describe(doc.error()) << "\n";
            return 2;
        }
        out << *doc;
    } else {
        out << text.str() << items << " items: " << byStatus["Ok"] << " ok, " << byStatus["Warning"]
            << " warning, " << byStatus["Error"] << " error, " << byStatus["Disabled"] << " disabled; "
            << parseErrors << " parse errors; " << moduleErrors << " module errors\n";
    }
    return (moduleErrors > 0 || byStatus["Error"] > 0) ? 1 : 0;
}

int runMathsFunctions(AppContext& /*ctx*/, bool asJson, std::ostream& out, std::ostream& err) {
    json list = json::array();
    for (const m::FunctionManifest* f : m::FunctionRegistry::builtin().all()) {
        json args = json::array();
        for (const m::ArgSpec& a : f->args) {
            static constexpr std::array<std::string_view, 5> kKinds{"Scalar", "Series", "Text", "RangeRef",
                                                                    "Keyword"};
            args.push_back(json{{"name", a.name},
                                {"kind", std::string(kKinds[static_cast<std::size_t>(a.kind)])},
                                {"dim", a.dim},
                                {"optional", a.optional}});
        }
        list.push_back(json{{"name", f->name},
                            {"description", f->description},
                            {"arities", f->arities},
                            {"args", std::move(args)},
                            {"unitRule", f->returns.unitRule},
                            {"provenance", f->provenance}});
    }
    if (asJson) {
        Result<std::string> doc = canonicalText(json{{"functions", std::move(list)}});
        if (!doc) {
            err << "esses-cli maths functions: " << describe(doc.error()) << "\n";
            return 2;
        }
        out << *doc;
        return 0;
    }
    for (const json& f : list) {
        out << f.at("name").get<std::string>() << "\n";
    }
    return 0;
}

} // namespace esses::cli
