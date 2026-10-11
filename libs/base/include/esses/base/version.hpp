// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Build identification (08 section 3.10). The values are stamped by cmake/version.cmake at configure time
// from `git describe` (falling back to the project() version) into a generated private header of esses_base,
// so a version change recompiles one translation unit only.
#pragma once

#include <esses/base/config.hpp>

#include <string>
#include <string_view>

namespace esses {

struct VersionInfo {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string_view string;    // "0.1.0", "0.1.0+3-gabc1234", "0.1.0+3-gabc1234.dirty"
    std::string_view gitSha;    // "unknown" when built outside a git checkout or before the first commit
    std::string_view buildDate; // ISO date, from SOURCE_DATE_EPOCH when set
    std::string_view appName;   // ESSES_APP_NAME of cmake/naming.cmake
};

const VersionInfo& versionInfo() noexcept;
std::string_view versionString() noexcept;
// "Esses 0.1.0 (git unknown, built 2026-10-10)"
std::string versionBanner();

} // namespace esses
