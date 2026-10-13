// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/project/shipped_data.hpp>

namespace esses::project {

std::filesystem::path shippedFile(const Paths& paths, std::string_view relative) {
    return paths.dataDir() / std::filesystem::path(relative);
}

} // namespace esses::project
