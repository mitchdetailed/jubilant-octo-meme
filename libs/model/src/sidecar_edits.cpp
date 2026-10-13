// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The M2 sidecar edit commands (02 section 7, D-032; 06 section 13.3 item 6): annotations, local maths and
// "restore logged laps". The undo commands of esses_qtmodel wrap them.

#include <esses/base/uuid.hpp>
#include <esses/model/sidecar.hpp>

#include <algorithm>
#include <utility>

namespace esses::model {

Uuid SidecarDocument::setAnnotation(std::string_view layerName, Annotation a) {
    if (a.id.isNil()) {
        a.id = uuidRandom();
    }
    const Uuid id = a.id;
    auto& notes = layer(layerName).annotations;
    const auto it =
        std::find_if(notes.begin(), notes.end(), [&id](const Annotation& n) { return n.id == id; });
    if (it != notes.end()) {
        *it = std::move(a);
    } else {
        notes.push_back(std::move(a));
    }
    return id;
}

bool SidecarDocument::removeAnnotation(std::string_view layerName, const Uuid& id) {
    if (!hasLayer(layerName)) {
        return false;
    }
    return std::erase_if(layer(layerName).annotations, [&id](const Annotation& n) { return n.id == id; }) !=
           0;
}

void SidecarDocument::setLocalMaths(std::string_view layerName, std::optional<JsonDoc> module) {
    if (!module.has_value() && !hasLayer(layerName)) {
        return;
    }
    layer(layerName).localMaths = std::move(module);
}

bool SidecarDocument::restoreLogged(std::string_view layerName) {
    if (!hasLayer(layerName)) {
        return false;
    }
    SidecarLayer& l = layer(layerName);
    const bool changed = !l.markerEdits.empty() || !l.markers.empty();
    l.markerEdits.clear();
    l.markers.clear();
    return changed;
}

} // namespace esses::model
