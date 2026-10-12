// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Typed reading of JSON documents with JSON-pointer error reporting (private to esses_model). The readers are
// esses_base's (<esses/base/json_nlohmann.hpp>, shared with esses_project); this header brings them into
// model::detail under the names the document code uses. A violation is thrown as JsonViolation inside the
// library and turned into Result at the public boundary by guardJson(), so no exception crosses the library
// boundary (01 section 6.2 rule 1).
#pragma once

#include <esses/base/json_nlohmann.hpp>

namespace esses::model::detail {

using jsonio::asBool;
using jsonio::asInt;
using jsonio::asNumber;
using jsonio::asString;
using jsonio::asU32;
using jsonio::asU64;
using jsonio::asUuid;
using jsonio::checkSchemaId;
using jsonio::guardJson;
using jsonio::JsonViolation;
using jsonio::ObjectReader;
using jsonio::parseJson;
using jsonio::pointerChild;
using jsonio::pointerIndex;
using jsonio::violate;

} // namespace esses::model::detail
