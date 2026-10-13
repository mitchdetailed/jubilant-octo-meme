// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// libFuzzer target fuzz_maths_parser (03 section 14.3, 07 section 5.1): one input is one maths expression.
// Parser::parse under both settings of MathsCompat::unaryMinusAbovePower; an expression that parses is
// printed and must parse again to the same canonical text (03 section 14.2 property 1); the TypeChecker runs
// against the data-less scope and a small data-backed one, and compile() lowers what checks without errors.
// Invariants: no crash, no sanitizer report, no exception, bounded memory (the parser refuses nesting beyond
// its depth limit with E-EXPR-TOO-DEEP).

#include <esses/base/rate.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/parser.hpp>
#include <esses/maths/plan.hpp>
#include <esses/maths/scope.hpp>
#include <esses/maths/type_checker.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace esses;
using namespace esses::maths;

// The shipped unit table (the build passes its path), so [unit] lookups and conversions are exercised.
const units::UnitRegistry& unitTable() {
    static const units::UnitRegistry registry = [] {
        auto r = units::UnitRegistry::loadFiles(ESSES_FUZZ_UNITS_JSON);
        if (!r) {
            std::abort(); // data/units.json of this source tree must load
        }
        return std::move(r).value();
    }();
    return registry;
}

class FuzzScope final : public Scope {
public:
    FuzzScope() : Scope(FunctionRegistry::builtin(), unitTable()) {}
    std::optional<ScopeEntry> lookup(std::string_view nameCf,
                                     const ChannelKey& /*requester*/) const override {
        ScopeEntry e;
        if (nameCf == "speed") {
            e.type.kind = Kind::Series;
            e.type.unit = *unitTable().find("m/s");
            e.type.rate = Rate::fromHz(100);
            e.key = ChannelKey::make({}, "Normal", "Speed");
            return e;
        }
        if (nameCf == "gear") {
            e.type.kind = Kind::Series;
            e.type.integer = true;
            e.type.rate = Rate::fromHz(10);
            e.key = ChannelKey::make({}, "Normal", "Gear");
            return e;
        }
        if (nameCf == "gain") {
            e.what = ScopeEntry::Constant;
            e.type.kind = Kind::Scalar;
            e.number = 2.0;
            return e;
        }
        if (nameCf == "driver") {
            e.what = ScopeEntry::DetailText;
            e.type.kind = Kind::Text;
            e.text = "Joe";
            return e;
        }
        return std::nullopt;
    }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 64 * 1024) {
        return 0; // expressions are short; keep each run fast
    }
    const std::string_view src(reinterpret_cast<const char*>(data), size);
    static const Scope dataless(FunctionRegistry::builtin(), unitTable());
    static const FuzzScope scope;
    for (const bool on : {false, true}) {
        MathsCompat compat;
        compat.unaryMinusAbovePower = on;
        Diagnostics diags;
        Result<Ast> ast = Parser::parse(src, compat, diags);
        if (!ast) {
            continue;
        }
        const std::string printed = print(*ast);
        Diagnostics again;
        Result<Ast> reparsed = Parser::parse(printed, compat, again);
        if (!reparsed || print(*reparsed) != printed) {
            std::abort(); // the canonical form must parse back to itself
        }
        Ast forData = *ast;
        (void)TypeChecker::check(*ast, dataless, diags);
        Diagnostics checked;
        (void)TypeChecker::check(forData, scope, checked);
        if (!hasError(checked)) {
            Diagnostics compiled;
            (void)compile(forData, scope, MathItem{}, compat, compiled);
        }
    }
    return 0;
}
