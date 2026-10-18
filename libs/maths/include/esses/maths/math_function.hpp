// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// IMathFunction and the function manifest (03 section 10.1, D-024): one manifest shape for the intrinsics of
// 03 section 5 and for external functions (the v1.0 C ABI esses_mathfn_v1 and v1.1 Lua), so the editor, the
// type checker and `esses-cli maths functions --json` treat both alike (schema mathfn-manifest-1). M2 ships
// the interface and the manifest schema, no external implementation (07 section 5.1).
#pragma once

#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/maths/kernel.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace esses::maths {

struct EvalContext;

struct ArgSpec {
    enum class Kind : std::uint8_t { Scalar, Series, Text, RangeRef, Keyword };
    std::string name;
    Kind kind = Kind::Series;
    std::string dim = "any"; // "any" | "time" | "length" | ...
    bool optional = false;
    friend bool operator==(const ArgSpec&, const ArgSpec&) = default;
};

struct FunctionManifest {
    std::string name; // canonical lower case; dotted for external functions ("mylib.rolling_rms")
    std::string description;
    std::vector<ArgSpec> args;
    std::vector<std::uint8_t> arities; // accepted argument counts (E-ARITY lists them)
    struct Returns {
        ArgSpec::Kind kind = ArgSpec::Kind::Series;
        std::string unitRule; // 03 section 3.2 rule 9: same_as:x, unitless, angle, per_time:x, ...
        friend bool operator==(const Returns&, const Returns&) = default;
    } returns;
    KernelClass cls = KernelClass::Elementwise;
    bool blockCapable = false;
    bool telemetry = false;
    std::string provenance; // "intrinsic" | plugin id | "lua:<file>"
    friend bool operator==(const FunctionManifest&, const FunctionManifest&) = default;
};

// Values passed to and returned by an external function (03 section 10.1).
struct SeriesValue { // a block of samples with its grid and unit
    BlockView data;
    Rate rate;
    Micros t0 = 0;
    units::UnitId unit = units::kNoUnit;
};
struct RangePath { // "Outings:Laps"
    std::string path;
};
struct Keyword { // rise / fall
    std::string word;
};
struct InvalidValue {};
using Value = std::variant<double, SeriesValue, std::string, RangePath, Keyword, InvalidValue>;

class IMathFunction {
public:
    IMathFunction() = default;
    IMathFunction(const IMathFunction&) = delete;
    IMathFunction& operator=(const IMathFunction&) = delete;
    IMathFunction(IMathFunction&&) = delete;
    IMathFunction& operator=(IMathFunction&&) = delete;
    virtual ~IMathFunction() = default;

    virtual const FunctionManifest& manifest() const = 0;
    // Over the whole math window, or per block when manifest().blockCapable.
    virtual Result<Value> evaluate(EvalContext& ctx, std::span<const Value> args) = 0;
};

} // namespace esses::maths
