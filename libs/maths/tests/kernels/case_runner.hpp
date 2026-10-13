// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The maths case runner (03 section 14.1, AC-M2-02): loads one maths-case-1 document (tests/maths/cases),
// builds an in-memory source with its input channels, details, range groups and selection, parses, compiles
// and evaluates the expression (or the table2d item) the way MathGraph will (whole-node, 03 section 6.0), and
// compares the result with the document's expectations: the result rate, every value within the tolerance
// (null = an invalid sample), the expected error code, and the count of each listed diagnostic code. The
// expectations come from tools/maths_ref.py, an independent reference (D-041). Test-only.
#pragma once

#include "../maths_test_support.hpp"
#include "kernels/kernels_internal.hpp"

#include <esses/analysis/distance_index.hpp>
#include <esses/analysis/range_tables.hpp>
#include <esses/base/casefold.hpp>
#include <esses/maths/eval_context.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/math_input_gatherer.hpp>
#include <esses/maths/parser.hpp>
#include <esses/maths/plan.hpp>
#include <esses/maths/scope.hpp>
#include <esses/model/range.hpp>
#include <esses/store/memory_provider.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace esses::maths::test {

inline std::filesystem::path caseDirectory() {
    return repoRoot() / "tests" / "maths" / "cases";
}

// Every *.case.json under the corpus directory, sorted.
inline std::vector<std::filesystem::path> caseFiles() {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(caseDirectory(), ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() > 10 && name.ends_with(".case.json")) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct CaseOutcome {
    std::string id;
    bool passed = false;
    std::vector<std::string> problems; // why it failed, one line each
    std::size_t samples = 0;           // values compared
};

namespace detail {

using nlohmann::json;

inline Rate rateOf(const json& j) {
    return Rate{j.at("num").get<std::uint32_t>(), j.at("den").get<std::uint32_t>()};
}

inline units::UnitId unitOrNone(const std::string& symbol) {
    return symbol.empty() ? units::kNoUnit : unitOf(symbol);
}

inline std::string text(double v) {
    std::ostringstream os;
    os.precision(12);
    os << v;
    return os.str();
}

// The case's names over one source: input channels, constants, numeric and text details.
class CaseScope final : public Scope {
public:
    CaseScope(std::shared_ptr<const model::DataSource> src, const json& doc)
        : Scope(FunctionRegistry::builtin(), *shippedUnits()), src_(std::move(src)) {
        if (doc.contains("constants")) {
            for (const auto& [name, v] : doc.at("constants").items()) {
                ScopeEntry e;
                e.what = ScopeEntry::Constant;
                e.type.kind = Kind::Scalar;
                e.type.unit = unitOrNone(v.at("unit").get<std::string>());
                e.number = v.at("v").get<double>();
                e.resolvedName = name;
                named_[caseFold(name)] = e;
            }
        }
        if (doc.contains("details")) {
            for (const auto& [name, v] : doc.at("details").items()) {
                ScopeEntry e;
                e.resolvedName = name;
                if (v.contains("text")) {
                    e.what = ScopeEntry::DetailText;
                    e.type.kind = Kind::Text;
                    e.text = v.at("text").get<std::string>();
                } else {
                    e.what = ScopeEntry::DetailNumber;
                    e.type.kind = Kind::Scalar;
                    e.type.unit = unitOrNone(v.at("unit").get<std::string>());
                    e.number = v.at("v").get<double>();
                }
                named_[caseFold(name)] = e;
            }
        }
    }

    std::optional<ScopeEntry> lookup(std::string_view nameCf,
                                     const ChannelKey& /*requester*/) const override {
        if (const model::Channel* ch = src_->layers.front().find(nameCf)) {
            ScopeEntry e;
            e.what = ScopeEntry::Channel;
            e.type.kind = Kind::Series;
            e.type.unit = ch->desc.unit;
            e.type.rate = ch->desc.rate;
            e.type.integer = ch->desc.dtype == store::Dtype::I32;
            e.key = ChannelKey::make(src_->id, "Normal", ch->desc.name);
            e.resolvedName = ch->desc.name;
            e.interp = ch->desc.interp;
            e.dtype = ch->desc.dtype;
            return e;
        }
        const auto it = named_.find(std::string(nameCf));
        if (it == named_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    std::shared_ptr<const model::DataSource> src_;
    std::map<std::string, ScopeEntry> named_;
};

inline store::Interp interpOf(const json& in) {
    const std::string s = in.value("interp", std::string("linear"));
    return s == "previous"  ? store::Interp::Previous
           : s == "next"    ? store::Interp::Next
           : s == "closest" ? store::Interp::Closest
                            : store::Interp::Linear;
}

inline model::Channel channelOf(store::ChunkCache& cache, const std::string& key, const std::string& name,
                                const json& in) {
    model::Channel ch;
    ch.desc.name = name;
    ch.desc.loggedName = name;
    ch.desc.unit = unitOrNone(in.at("unit").get<std::string>());
    ch.desc.quantity = shippedUnits()->quantityOf(ch.desc.unit);
    ch.desc.rate = rateOf(in.at("rate"));
    ch.desc.interp = interpOf(in);
    const std::vector<double> values = in.at("values").get<std::vector<double>>();
    std::vector<bool> valid;
    if (in.contains("valid")) {
        for (const int v : in.at("valid").get<std::vector<int>>()) {
            valid.push_back(v != 0);
        }
    }
    const std::string dtype = in.value("dtype", std::string("f32"));
    std::shared_ptr<store::InMemoryProvider> provider;
    if (dtype == "f64") {
        ch.desc.dtype = store::Dtype::F64;
        provider = store::InMemoryProvider::fromF64(values, valid, "case:" + key);
    } else if (dtype == "i32") {
        ch.desc.dtype = store::Dtype::I32;
        std::vector<std::int32_t> ints;
        for (const double v : values) {
            ints.push_back(static_cast<std::int32_t>(v));
        }
        provider = store::InMemoryProvider::fromI32(ints, valid, "case:" + key);
    } else {
        ch.desc.dtype = store::Dtype::F32;
        std::vector<float> floats;
        for (const double v : values) {
            floats.push_back(static_cast<float>(v));
        }
        provider = store::InMemoryProvider::fromF32(floats, valid, "case:" + key);
    }
    store::ColumnOptions options;
    options.cache = &cache;
    ch.series = store::UniformSeries{*ch.desc.rate, 0, store::Column(provider, options)};
    return ch;
}

// "<group path>/<n>" (or {range, offsetUs, offsetM, stretch}) against the case's ranges.
inline std::optional<MathRangeGeom> geomOf(const json& doc, const char* which, const model::SourceId& src) {
    if (!doc.contains("selection") || !doc.at("selection").contains(which)) {
        return std::nullopt;
    }
    const json& s = doc.at("selection").at(which);
    const std::string spec = s.is_string() ? s.get<std::string>() : s.at("range").get<std::string>();
    const std::size_t slash = spec.rfind('/');
    const std::string path = spec.substr(0, slash);
    const auto index = static_cast<std::size_t>(std::stoul(spec.substr(slash + 1)) - 1);
    std::vector<std::pair<Micros, Micros>> ranges;
    for (const json& r : doc.at("ranges").at(path)) {
        ranges.emplace_back(r.at("startUs").get<Micros>(), r.at("endUs").get<Micros>());
    }
    std::sort(ranges.begin(), ranges.end());
    MathRangeGeom g;
    g.ref.src = src;
    g.ref.layer = "Normal";
    g.ref.groupPath = path;
    g.ref.range = model::rangeId(path, model::MarkerId{}, "case-" + std::to_string(index));
    g.start = ranges.at(index).first;
    g.end = ranges.at(index).second;
    if (s.is_object()) {
        g.offsetUs = s.value("offsetUs", Micros{0});
        g.offsetM = s.value("offsetM", 0.0);
        g.stretch = s.value("stretch", 1.0);
    }
    return g;
}

inline void addRanges(model::DataLayer& layer, const json& doc) {
    if (!doc.contains("ranges")) {
        return;
    }
    std::vector<std::string> paths;
    for (const auto& [path, list] : doc.at("ranges").items()) {
        paths.push_back(path);
    }
    // Parents first: fewer ':' separators first.
    std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
        const auto da = std::count(a.begin(), a.end(), ':');
        const auto db = std::count(b.begin(), b.end(), ':');
        return da != db ? da < db : a < b;
    });
    for (const std::string& path : paths) {
        model::RangeGroup g = model::RangeGroup::make(path);
        g.version = 1;
        std::size_t k = 0;
        for (const json& r : doc.at("ranges").at(path)) {
            model::Range range;
            range.id = model::rangeId(path, model::MarkerId{}, "case-" + std::to_string(k));
            range.kind = model::RangeKind::Lap;
            range.start = r.at("startUs").get<Micros>();
            range.end = r.at("endUs").get<Micros>();
            range.number = static_cast<int>(k + 1);
            g.ranges.push_back(range);
            ++k;
        }
        layer.ranges.setGroup(std::move(g));
    }
}

inline std::shared_ptr<const analysis::DistanceIndex> distanceOf(const json& doc, const MathRangeGeom& g) {
    if (!doc.at("inputs").contains("Corr Dist")) {
        return nullptr;
    }
    const json& in = doc.at("inputs").at("Corr Dist");
    const Rate rate = rateOf(in.at("rate"));
    const std::vector<double> d = in.at("values").get<std::vector<double>>();
    std::vector<Micros> times;
    for (std::size_t i = 0; i < d.size(); ++i) {
        times.push_back(rate.timeAt(static_cast<SampleIdx>(i), 0));
    }
    analysis::DistanceIndexKey key;
    key.start = g.start;
    key.end = g.end;
    key.stretch = g.stretch;
    Result<analysis::DistanceIndex> idx = analysis::DistanceIndex::fromSamples(times, d, key);
    if (!idx) {
        return nullptr;
    }
    return std::make_shared<const analysis::DistanceIndex>(std::move(idx).value());
}

inline std::size_t countCode(const Diagnostics& diags, const std::string& code) {
    return static_cast<std::size_t>(
        std::count_if(diags.begin(), diags.end(), [&](const Diagnostic& d) { return d.code == code; }));
}

} // namespace detail

inline CaseOutcome runCase(const std::filesystem::path& file) {
    using detail::json;
    CaseOutcome out;
    out.id = file.filename().string();
    std::ifstream f(file, std::ios::binary);
    json doc;
    try {
        doc = json::parse(f);
    } catch (const json::exception& e) {
        out.problems.push_back(std::string("not JSON: ") + e.what());
        return out;
    }
    out.id = doc.value("id", out.id);
    const std::string expr = doc.at("expression").get<std::string>();

    // The source: one Normal layer with the inputs and range groups.
    auto rt = std::make_shared<Runtime>(1);
    std::vector<model::Channel> channels;
    for (const auto& [name, in] : doc.at("inputs").items()) {
        channels.push_back(detail::channelOf(rt->cache, out.id + ":" + name, name, in));
    }
    const Micros duration = doc.at("durationUs").get<Micros>();
    std::shared_ptr<model::DataSource> src =
        makeSource(std::move(channels), duration, "maths-case:" + out.id);
    detail::addRanges(src->layers.front(), doc);

    MathsCompat compat;
    if (doc.contains("compat")) {
        compat.unaryMinusAbovePower = doc.at("compat").value("unaryMinusAbovePower", false);
    }
    MathItem item;
    if (doc.contains("item")) {
        const json& t = doc.at("item");
        item.kind = ItemKind::Table2D;
        item.table.x.expr = expr;
        item.table.x.unit = detail::unitOrNone(t.at("axis").at("unit").get<std::string>());
        item.table.x.breakpoints = t.at("axis").at("breakpoints").get<std::vector<double>>();
        item.table.valuesUnit = detail::unitOrNone(t.at("values").at("unit").get<std::string>());
        item.table.values = t.at("values").at("data").get<std::vector<double>>();
        const std::string interp = t.value("interpolation", std::string("linear"));
        item.table.interpolation = interp == "next"       ? TableSpec::Interp::Next
                                   : interp == "previous" ? TableSpec::Interp::Previous
                                                          : TableSpec::Interp::Linear;
        item.table.outOfRange = t.value("outOfRange", std::string("clamp")) == "invalid"
                                    ? TableSpec::OutOfRange::Invalid
                                    : TableSpec::OutOfRange::Clamp;
    }
    if (doc.contains("rate") && doc.at("rate").is_object()) {
        item.rate.automatic = false;
        item.rate.rate = detail::rateOf(doc.at("rate"));
    }
    if (doc.contains("resultUnit")) {
        item.resultUnit = detail::unitOrNone(doc.at("resultUnit").get<std::string>());
        item.quantity = shippedUnits()->quantityOf(item.resultUnit);
    }
    if (doc.value("precision", std::string("single")) == "double") {
        item.precision = "double";
    }

    const json& expected = doc.at("expected");
    const bool wantError = expected.contains("error");
    const std::string wantCode =
        wantError ? expected.at("error").at("code").get<std::string>() : std::string();

    Diagnostics diags;
    std::string failCode;
    std::string failMessage;
    std::optional<analysis::UniformSignal> got;
    Rate resultRate;
    Result<Ast> ast = Parser::parse(expr, compat, diags);
    if (!ast) {
        failMessage = ast.error().message;
    } else {
        const detail::CaseScope scope(src, doc);
        Result<Plan> plan = compile(*ast, scope, item, compat, diags);
        if (plan && item.kind == ItemKind::Table2D) {
            // As the host does for a table2d item (MathGraph's plan builder): the Table stage after the axis.
            Result<std::unique_ptr<Kernel>> table = kern::makeTableKernel(item.table, *shippedUnits());
            if (!table) {
                plan = Failure{table.error()};
            } else {
                Stage stage;
                stage.kernel = std::move(table).value();
                stage.inputs.push_back(
                    Stage::Ref{Stage::Ref::From::Stage, static_cast<std::uint32_t>(plan->stages.size() - 1)});
                stage.outTemp = static_cast<std::uint32_t>(plan->stages.size());
                plan->stages.push_back(std::move(stage));
                plan->out.integer = false;
                plan->outDtype = item.precision == "double" ? store::Dtype::F64 : store::Dtype::F32;
            }
        }
        if (!plan) {
            failCode = plan.error().context.detail.value_or("");
            failMessage = plan.error().message;
        } else {
            resultRate = plan->out.rate && plan->out.rate->valid() ? *plan->out.rate : Rate{};
            if (!resultRate.valid()) {
                for (const model::Channel& c : src->layers.front().channels) {
                    if (c.desc.rate && (!resultRate.valid() || resultRate < *c.desc.rate)) {
                        resultRate = *c.desc.rate;
                    }
                }
            }
            EvalContext ctx;
            ctx.source = src;
            ctx.layer = "Normal";
            ctx.resultRate = resultRate;
            ctx.resultFirst = 0;
            ctx.resultCount = static_cast<std::uint64_t>(resultRate.indexAt(duration - 1, 0) + 1);
            ctx.units = shippedUnits();
            ctx.ranges = analysis::RangeTables::build(src->layers.front());
            ctx.mathStartUs = 0;
            ctx.mathEndUs = duration;
            if (const std::optional<MathRangeGeom> main = detail::geomOf(doc, "main", src->id)) {
                ctx.selection.main = *main;
                ctx.mathStartUs = main->start;
                ctx.mathEndUs = main->end;
                ctx.mainDist = detail::distanceOf(doc, *main);
            }
            if (const std::optional<MathRangeGeom> ref = detail::geomOf(doc, "reference", src->id)) {
                ctx.selection.reference = *ref;
                ctx.refDist = detail::distanceOf(doc, *ref);
            }
            const std::shared_ptr<const model::DataSource> source = src;
            ctx.getChannel = [source](const ChannelKey& key, units::UnitId unit,
                                      Rate r) -> Result<MathInputGatherer> {
                const model::Channel* ch = source->layers.front().find(key.nameCf);
                if (ch == nullptr) {
                    return fail(ErrorCode::NotFound, "no channel " + key.nameCf);
                }
                units::Converter conv;
                if (unit != ch->desc.unit) {
                    Result<units::Converter> c = shippedUnits()->converter(ch->desc.unit, unit);
                    if (!c) {
                        return Failure{c.error()};
                    }
                    conv = *c;
                }
                return MathInputGatherer(ch->series, r, 0, ch->desc.interp, conv, CancelToken{});
            };
            Result<analysis::UniformSignal> r = evaluateWhole(*plan, ctx);
            if (!r) {
                failCode = r.error().context.detail.value_or("");
                failMessage = r.error().message;
            } else {
                got = std::move(r).value();
            }
        }
    }

    // Diagnostics.
    if (expected.contains("diagnostics")) {
        for (const json& d : expected.at("diagnostics")) {
            const std::string code = d.at("code").get<std::string>();
            const auto want = d.at("count").get<std::size_t>();
            const std::size_t n = detail::countCode(diags, code);
            if (n != want) {
                out.problems.push_back(code + " reported " + std::to_string(n) + " times, expected " +
                                       std::to_string(want));
            }
        }
    }

    if (wantError) {
        const bool failed = !got.has_value();
        const bool codeSeen =
            failCode == wantCode || std::any_of(diags.begin(), diags.end(), [&](const Diagnostic& d) {
                return d.level == Diagnostic::Error && d.code == wantCode;
            });
        if (!failed) {
            out.problems.push_back("evaluated, expected the error " + wantCode);
        } else if (!codeSeen) {
            out.problems.push_back("failed with '" + failCode + "' (" + failMessage + "), expected " +
                                   wantCode);
        }
        out.passed = out.problems.empty();
        return out;
    }
    if (!got) {
        std::string codes;
        for (const Diagnostic& d : diags) {
            if (d.level == Diagnostic::Error) {
                codes += " " + d.code + " (" + d.message + ")";
            }
        }
        out.problems.push_back("failed: " + failCode + " " + failMessage + codes);
        return out;
    }
    const Rate wantRate = detail::rateOf(expected.at("rate"));
    if (!(got->rate == wantRate)) {
        out.problems.push_back("rate " + got->rate.toString() + ", expected " + wantRate.toString());
    }
    const json& values = expected.at("values");
    const double tolAbs = expected.at("tolerance").at("abs").get<double>();
    const double tolRel = expected.at("tolerance").at("rel").get<double>();
    if (values.size() != got->values.size()) {
        out.problems.push_back(std::to_string(got->values.size()) + " samples, expected " +
                               std::to_string(values.size()));
        return out;
    }
    std::vector<int> validFlags;
    if (expected.contains("valid")) {
        validFlags = expected.at("valid").get<std::vector<int>>();
    }
    std::size_t reported = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const bool wantValid = !values[i].is_null() && (validFlags.empty() || validFlags[i] != 0);
        const bool isValid = got->validAt(i) && !std::isnan(got->values[i]);
        std::string problem;
        if (wantValid != isValid) {
            problem = isValid ? "valid (" + detail::text(got->values[i]) + "), expected invalid"
                              : "invalid, expected " + detail::text(values[i].get<double>());
        } else if (wantValid) {
            const double want = values[i].get<double>();
            const double diff = std::abs(got->values[i] - want);
            if (!(diff <= tolAbs + tolRel * std::abs(want))) {
                problem = detail::text(got->values[i]) + ", expected " + detail::text(want);
            }
        }
        if (!problem.empty() && reported++ < 6) {
            out.problems.push_back("sample " + std::to_string(i) + ": " + problem);
        }
        ++out.samples;
    }
    if (reported > 6) {
        out.problems.push_back("... " + std::to_string(reported - 6) + " more samples differ");
    }
    out.passed = out.problems.empty();
    return out;
}

} // namespace esses::maths::test
