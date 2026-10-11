// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "base_build_config.hpp"

#include <esses/base/version.hpp>

namespace esses {

const VersionInfo& versionInfo() noexcept {
    static constexpr VersionInfo kInfo{ESSES_BUILD_VERSION_MAJOR, ESSES_BUILD_VERSION_MINOR,
                                       ESSES_BUILD_VERSION_PATCH, ESSES_BUILD_VERSION_STRING,
                                       ESSES_BUILD_GIT_SHA,       ESSES_BUILD_DATE,
                                       ESSES_BUILD_APP_NAME};
    return kInfo;
}

std::string_view versionString() noexcept {
    return versionInfo().string;
}

std::string versionBanner() {
    const VersionInfo& v = versionInfo();
    std::string out;
    out.append(v.appName).append(" ").append(v.string);
    out.append(" (git ").append(v.gitSha).append(", built ").append(v.buildDate).append(")");
    return out;
}

} // namespace esses
