// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/assert.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/utf8.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/channel.hpp>

#include <string>

namespace esses::model {

std::string_view channelSourceName(ChannelSource s) noexcept {
    switch (s) {
    case ChannelSource::Logged:
        return "logged";
    case ChannelSource::Maths:
        return "maths";
    case ChannelSource::Detail:
        return "detail";
    case ChannelSource::Alias:
        return "alias";
    case ChannelSource::Mapped:
        return "mapped";
    case ChannelSource::Constant:
        return "constant";
    case ChannelSource::Setup:
        return "setup";
    }
    return "logged";
}

std::optional<ChannelSource> parseChannelSource(std::string_view text) noexcept {
    for (const ChannelSource s :
         {ChannelSource::Logged, ChannelSource::Maths, ChannelSource::Detail, ChannelSource::Alias,
          ChannelSource::Mapped, ChannelSource::Constant, ChannelSource::Setup}) {
        if (text == channelSourceName(s)) {
            return s;
        }
    }
    return std::nullopt;
}

const EnumEntry* EnumTable::find(std::int32_t value) const noexcept {
    for (const EnumEntry& e : entries) {
        if (e.value == value) {
            return &e;
        }
    }
    return nullptr;
}

std::string ChannelDescriptor::displayShortName() const {
    if (!shortName.empty()) {
        return shortName;
    }
    std::string out;
    std::size_t pos = 0;
    for (int n = 0; n < 8 && pos < name.size(); ++n) {
        const std::size_t start = pos;
        static_cast<void>(utf8::decodeNext(name, pos));
        out.append(name, start, pos - start);
    }
    return out;
}

std::string_view channelStatusName(ChannelStatus s) noexcept {
    switch (s) {
    case ChannelStatus::Ok:
        return "Ok";
    case ChannelStatus::Pending:
        return "Pending";
    case ChannelStatus::Error:
        return "Error";
    }
    return "Ok";
}

ChannelStatus statusFromText(std::string_view statusText) noexcept {
    if (statusText == "Pending") {
        return ChannelStatus::Pending;
    }
    if (statusText.starts_with("Error")) {
        return ChannelStatus::Error;
    }
    return ChannelStatus::Ok;
}

void Channel::setStatus(ChannelStatus s, std::string_view message) {
    status = s;
    switch (s) {
    case ChannelStatus::Ok:
        desc.statusText.clear();
        break;
    case ChannelStatus::Pending:
        desc.statusText = "Pending";
        break;
    case ChannelStatus::Error:
        desc.statusText = message.empty() ? std::string("Error") : "Error: " + std::string(message);
        break;
    }
}

namespace {

ChannelId derive(std::string_view kind, const SourceId& source, std::string_view layer, std::string_view name,
                 std::string_view last) {
    const std::string hex = source.hex();
    return ChannelId{uuidFromHash(hash128({kind, hex, canonicalLayerName(layer), name, last}))};
}

} // namespace

ChannelId channelIdLogged(const SourceId& source, std::string_view layer, std::string_view loggedName,
                          std::string_view importerId) {
    return derive("logged", source, layer, loggedName, importerId);
}

ChannelId channelIdMathsModule(const SourceId& source, std::string_view layer, std::string_view name,
                               std::string_view moduleId) {
    const std::string last = "module:" + std::string(moduleId);
    return derive("maths", source, layer, name, last);
}

ChannelId channelIdMathsLocal(const SourceId& source, std::string_view layer, std::string_view name) {
    return derive("maths", source, layer, name, "local");
}

ChannelId channelIdOther(ChannelSource kind, const SourceId& source, std::string_view layer,
                         std::string_view name) {
    ESSES_ASSERT_MSG(kind != ChannelSource::Logged && kind != ChannelSource::Maths,
                     "channelIdOther: logged and maths channels have their own derivations");
    return derive(channelSourceName(kind), source, layer, name, "");
}

} // namespace esses::model
