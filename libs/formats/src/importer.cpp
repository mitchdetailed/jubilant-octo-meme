// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/formats/importer.hpp>

#include <string>

namespace esses::formats {

store::Interp IReader::interp(LayerIx layer, ChannelIx channel) const {
    const ProbeResult& p = probe();
    if (layer < p.layers.size() && channel < p.layers[layer].channels.size() &&
        p.layers[layer].channels[channel].kind == store::SampleKind::Real) {
        return store::Interp::Linear;
    }
    return store::Interp::Previous;
}

Result<void> IReader::setConversion(LayerIx layer, ChannelIx channel, const ChannelConversion& conversion) {
    const ProbeResult& p = probe();
    if (layer >= p.layers.size() || channel >= p.layers[layer].channels.size()) {
        return fail(ErrorCode::NotFound, "setConversion: no channel " + std::to_string(channel) +
                                             " in layer " + std::to_string(layer));
    }
    if (conversion.toBase.identity() && conversion.dtype == p.layers[layer].channels[channel].dtype) {
        return ok();
    }
    return fail(ErrorCode::Unsupported, "this reader does not convert samples at decode");
}

} // namespace esses::formats
