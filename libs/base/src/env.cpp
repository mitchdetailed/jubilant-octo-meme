// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "base_build_config.hpp"

#include <esses/base/env.hpp>
#include <esses/base/utf8.hpp>

#if defined(ESSES_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace esses {

std::optional<std::string> getEnv(std::string_view name) {
#if defined(ESSES_OS_WINDOWS)
    const std::wstring wname = utf8::toWide(name);
    SetLastError(ERROR_SUCCESS);
    DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (needed == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return std::string();
    }
    std::wstring value;
    for (;;) {
        value.resize(needed);
        const DWORD written = GetEnvironmentVariableW(wname.c_str(), value.data(), needed);
        if (written == 0) {
            return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? std::nullopt
                                                            : std::optional<std::string>(std::string());
        }
        if (written < needed) {
            value.resize(written);
            return utf8::fromWide(value);
        }
        needed = written; // the variable grew between the two calls
    }
#else
    const std::string key(name);
    const char* v = std::getenv(key.c_str()); // NOLINT(concurrency-mt-unsafe): read-only use
    if (v == nullptr) {
        return std::nullopt;
    }
    return std::string(v);
#endif
}

bool setEnv(std::string_view name, const std::optional<std::string>& value) {
#if defined(ESSES_OS_WINDOWS)
    const std::wstring wname = utf8::toWide(name);
    if (!value) {
        return SetEnvironmentVariableW(wname.c_str(), nullptr) != 0;
    }
    const std::wstring wvalue = utf8::toWide(*value);
    return SetEnvironmentVariableW(wname.c_str(), wvalue.c_str()) != 0;
#else
    const std::string key(name);
    if (!value) {
        return ::unsetenv(key.c_str()) == 0;
    }
    return ::setenv(key.c_str(), value->c_str(), 1) == 0;
#endif
}

std::string envName(std::string_view suffix) {
    std::string out(ESSES_BUILD_ENV_PREFIX);
    out += '_';
    out += suffix;
    return out;
}

} // namespace esses
