// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/assert.hpp>
#include <esses/base/casefold.hpp>
#include <esses/model/layer.hpp>

#include <limits>
#include <string>
#include <utility>

namespace esses::model {

const Channel* DataLayer::find(std::string_view channelName) const {
    const auto it = byNameCI.find(caseFold(channelName));
    return it == byNameCI.end() ? nullptr : &channels[it->second];
}

const Channel* DataLayer::findLogged(std::string_view loggedName) const {
    const auto it = byLoggedNameCI.find(caseFold(loggedName));
    return it == byLoggedNameCI.end() ? nullptr : &channels[it->second];
}

const Channel* DataLayer::findById(const ChannelId& id) const {
    const auto it = byId.find(id);
    return it == byId.end() ? nullptr : &channels[it->second];
}

Channel* DataLayer::findMutable(std::string_view channelName) {
    const auto it = byNameCI.find(caseFold(channelName));
    return it == byNameCI.end() ? nullptr : &channels[it->second];
}

const EnumTable* DataLayer::enumTable(EnumTableId id) const noexcept {
    for (const EnumTable& t : enumTables) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

const Marker* DataLayer::findMarker(const MarkerId& id) const noexcept {
    for (const Marker& m : markers) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

std::uint32_t DataLayer::addChannel(Channel channel) {
    ESSES_ASSERT(channels.size() < std::numeric_limits<std::uint32_t>::max());
    const auto index = static_cast<std::uint32_t>(channels.size());
    byNameCI.emplace(caseFold(channel.desc.name), index);
    byLoggedNameCI.emplace(caseFold(channel.desc.loggedName), index);
    byId.emplace(channel.desc.id, index);
    channels.push_back(std::move(channel));
    return index;
}

void DataLayer::rebuildIndex() {
    byNameCI.clear();
    byLoggedNameCI.clear();
    byId.clear();
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        byNameCI.emplace(caseFold(channels[i].desc.name), index);
        byLoggedNameCI.emplace(caseFold(channels[i].desc.loggedName), index);
        byId.emplace(channels[i].desc.id, index);
    }
}

} // namespace esses::model
