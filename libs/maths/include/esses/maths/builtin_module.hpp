// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// IBuiltinModule (03 section 9, D-016, D-024): a module whose outputs are ordinary graph nodes (shadowing
// rules apply, they show in Channel Status and are cached by the settings hash); the algorithms live in
// esses_analysis and the module wraps them.
//
// M1 deviations from 03 section 9 (recorded in libs/maths/README.md):
//  - settings and the settings schema are model::OpaqueJson, because esses_base's JsonDoc is still a
//    reserved name;
//  - expand() takes the UnitRegistry instead of a maths Scope (the Scope arrives with the parser in M2);
//  - BuiltinOutput::evaluate is a whole-extent function (the 0.1 whole-node path of 03 section 6.0)
//    instead of the expression-or-Kernel variant; Kernel classes arrive in M2. It reports runtime
//    diagnostics (W-CSD-FALLBACK) through its Diagnostics argument;
//  - BuiltinOutput::inputs names the logged channels an output may read, so the node version hashes exactly
//    what the node reads (03 section 7.2, D-140) without a compiled Plan.
#pragma once

#include <esses/analysis/corrected_speed.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/eval_context.hpp>
#include <esses/maths/math_item.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/store/types.hpp>
#include <esses/units/unit_registry.hpp>

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace esses::maths {

// Evaluates one output over the whole layer extent (03 section 6.0). `upstream` holds the evaluated outputs
// named in BuiltinOutput::upstream, in that order (Corr Dist reads Corr Speed), read back from their sealed
// columns in base units. Runtime warnings go to `diags`. Runs on a worker; honours ctx.cancel.
using WholeExtentFn = std::function<Result<analysis::UniformSignal>(
    const EvalContext& ctx, std::span<const analysis::UniformSignal> upstream, Diagnostics& diags)>;

struct BuiltinOutput {
    std::string name;                  // "Corr Speed"
    std::vector<std::string> upstream; // outputs of the same module this one reads
    // Logged channels this output may read, in resolution order (fallback candidates included). The node
    // version hashes the identity, store version, unit and time base of each one present in the layer and
    // the absence of the others (03 section 7.2: exactly the logged columns the node reads).
    std::vector<std::string> inputs;
    WholeExtentFn evaluate;
    // M2: logged channels that must all be present in the layer; when one is missing the output is Disabled
    // with `missingSummary` instead of being evaluated (03 section 9.2: "Disabled (no GPS channels)", never
    // an error). An output named like one of its `inputs` is self-maths: it shadows that channel and its
    // descriptor reuses the shadowed channel's (03 section 8).
    std::vector<std::string> required;
    std::string missingSummary;
    units::QuantityId quantity = units::kUnitless;
    units::UnitId resultUnit = units::kNoUnit; // base-stored values (03 section 3.2 rule 11)
    DisplayDefaults display;
    store::Interp interp = store::Interp::Linear;
    store::Dtype dtype = store::Dtype::F32;
};

class IBuiltinModule {
public:
    virtual ~IBuiltinModule() = default;
    virtual std::string id() const = 0;
    virtual int version() const = 0;
    // "builtin-<id>-<version>": the id of schemas/builtin-<id>-<version>.schema.json.
    virtual std::string settingsSchemaId() const = 0;
    virtual const model::OpaqueJson& settingsSchema() const = 0;
    virtual model::OpaqueJson defaultSettings() const = 0;
    virtual Result<std::vector<BuiltinOutput>>
    expand(const model::OpaqueJson& settings, const units::UnitRegistry& units, Diagnostics& diags) const = 0;
    // M2: whether a disabled `builtin` item of this module (sidecar localMaths or a global module) disables
    // its outputs (true, the default) or only the optional steps the module's `enabled` flag gates
    // (esses.gps_corrections: the zero-fix step runs whatever `enabled` says, 03 section 9.2).
    virtual bool disabledItemDisablesOutputs() const { return true; }

protected:
    IBuiltinModule() = default;
    IBuiltinModule(const IBuiltinModule&) = default;
    IBuiltinModule& operator=(const IBuiltinModule&) = default;
};

// Validates `settings` against module.settingsSchema() (draft-07, the validator esses-cli validate uses).
// Errors: NotFound when the schema is not loaded (null), Format when it is not a usable schema, Invalid when
// the settings break it (context.schemaId = settingsSchemaId(), context.detail = the JSON pointer of the
// first error, the message names every error).
Result<void> validateSettings(const IBuiltinModule& module, const model::OpaqueJson& settings);

} // namespace esses::maths
