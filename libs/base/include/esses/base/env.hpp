// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Environment access in UTF-8. On Windows the wide API is used (GetEnvironmentVariableW), never getenv, which
// returns the ANSI code page and mangles non-ASCII profile paths (01 section 9).
#pragma once

#include <esses/base/config.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace esses {

// nullopt when the variable is not set.
std::optional<std::string> getEnv(std::string_view name);
// Sets (or, with nullopt, removes) a variable of this process. For tests and tools.
bool setEnv(std::string_view name, const std::optional<std::string>& value);
// "<prefix>_<suffix>" with the environment prefix of cmake/naming.cmake, e.g. envName("LOG") == "ESSES_LOG".
std::string envName(std::string_view suffix);

} // namespace esses
