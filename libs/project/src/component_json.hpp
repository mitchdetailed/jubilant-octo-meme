// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The workbook-1 component header (04 section 7.3) as nlohmann values (private; component_spec.cpp), shared
// by ComponentSpec and WorkbookFile.
#pragma once

#include "doc_io.hpp"

#include <esses/base/uuid.hpp>
#include <esses/project/component_spec.hpp>

#include <optional>
#include <string>

namespace esses::project::detail {

// Reads one component object at `pointer`. When it has no "id", `fallbackId` is used and `*derivedId` set;
// without a fallback a missing id is Invalid.
ComponentSpec componentFromJson(const json& j, const std::string& pointer,
                                const std::optional<Uuid>& fallbackId, bool* derivedId);
json componentToJson(const ComponentSpec& c);

} // namespace esses::project::detail
