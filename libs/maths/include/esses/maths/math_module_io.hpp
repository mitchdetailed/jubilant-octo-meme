// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathModuleIo (03 sections 1 and 4.1, 02 sections 6.5-6.6, D-019, D-072): maths-1 documents <-> MathModule.
// Global modules live in <Project>/maths/<module>.maths.json; local maths is the sidecar's localMaths member
// (the same item and constant schema, id "local"). Every number is read as {v, unit} through the
// UnitRegistry and written back in its unit; `rate` accepts "auto", {num, den} or a plain Hz number. Unknown
// top-level members are preserved (MathModule::unknown) and written sorted; output is the canonical text of
// 02 section 6.5 rule 1, so load(canonical) -> toJson reproduces the bytes (AC-M2-07). Schema validation
// against maths-1 is esses_project's SchemaRegistry (03 section 1), not done here.
#pragma once

#include <esses/base/result.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/units/unit_registry.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace esses::maths {

class MathModuleIo {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:maths-1";
    static constexpr int kSchemaVersion = 1;

    // Errors: Format (syntax, context.offset), Invalid (a maths-1 rule, context.detail = JSON pointer),
    // Unsupported (another schema major, context.schemaId). Lenient notes (missing $schema, a newer minor) go
    // to `log`.
    static Result<MathModule> parse(std::string_view json, const units::UnitRegistry& units,
                                    std::vector<std::string>& log);
    // From an already parsed document (sidecar localMaths).
    static Result<MathModule> fromJson(const model::OpaqueJson& doc, const units::UnitRegistry& units,
                                       std::vector<std::string>& log);
    // `load` adds context.path to the errors of parse().
    static Result<MathModule> load(const std::filesystem::path& path, const units::UnitRegistry& units,
                                   std::vector<std::string>& log);

    // The document (sidecar localMaths) and its canonical text. Invalid for a non-finite number or a unit id
    // the registry does not know.
    static Result<model::OpaqueJson> toJsonDoc(const MathModule& module, const units::UnitRegistry& units);
    static Result<std::string> toJson(const MathModule& module, const units::UnitRegistry& units);
    // Atomic write of toJson() (a transient module is refused with Invalid: it is never saved).
    static Result<void> save(const MathModule& module, const std::filesystem::path& path,
                             const units::UnitRegistry& units);
};

} // namespace esses::maths
