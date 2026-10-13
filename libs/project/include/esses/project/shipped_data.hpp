// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Locations of the shipped documents under the data directory (02 section 6.1 "Shipped (repo data/)", 08
// section 1, 01 section 9): <exe dir>/data in a build tree (libs/project/CMakeLists.txt copies them) and in
// an installation, data/ in the source tree. The M1 set is the default circuit workbook, the light and dark
// schemes, the default keymap, the Circuit profile and the circuit rules (07 section 4.1).
#pragma once

#include <esses/base/paths.hpp>

#include <filesystem>
#include <string_view>

namespace esses::project {

inline constexpr std::string_view kCircuitDefaultWorkbook = "workbooks/circuit-default.workbook.json";
inline constexpr std::string_view kLightScheme = "schemes/light.scheme.json";
inline constexpr std::string_view kDarkScheme = "schemes/dark.scheme.json";
inline constexpr std::string_view kDefaultKeymap = "keymaps/default.json";
inline constexpr std::string_view kCircuitProfile = "profiles/circuit.profile.json";
inline constexpr std::string_view kRulesDir = "rules";

// paths.dataDir() / relative.
std::filesystem::path shippedFile(const Paths& paths, std::string_view relative);

} // namespace esses::project
