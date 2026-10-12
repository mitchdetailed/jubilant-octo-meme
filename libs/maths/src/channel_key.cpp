// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/base/casefold.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/model/ids.hpp>

namespace esses::maths {

ChannelKey ChannelKey::make(const model::SourceId& src, std::string_view layer, std::string_view name) {
    return ChannelKey{src, std::string(model::canonicalLayerName(layer)), caseFold(name)};
}

} // namespace esses::maths
