// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The one marker serializer of sidecar-1 and markers-1 (02 section 5.3 rule 8): keys id, class, group, tUs,
// name, comment, flags (omitted when 0, "an absent flags means 0") and prov. Private to esses_model.
#pragma once

#include <esses/model/marker.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace esses::model::detail {

nlohmann::json markerToJson(const Marker& m);
// Throws JsonViolation; a DeviceEvent outside group "DeviceEvents" and any member outside the vocabulary are
// violations (probe-1's ordinal included).
Marker markerFromJson(const nlohmann::json& j, const std::string& pointer);

} // namespace esses::model::detail
