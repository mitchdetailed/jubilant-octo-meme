// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "json_canonical.hpp"
#include "json_read.hpp"

#include <esses/model/opaque_json.hpp>

#include <string>

namespace esses::model {

Result<OpaqueJson> OpaqueJson::parse(std::string_view json) {
    auto parsed = detail::parseJson(json);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    return detail::toOpaque(*parsed);
}

std::string OpaqueJson::canonical() const {
    auto text = detail::canonicalJson(detail::fromOpaque(*this));
    // The compact text was produced by the same writer, so it holds no non-finite number.
    return text ? std::move(text).value() : std::string("null\n");
}

} // namespace esses::model
