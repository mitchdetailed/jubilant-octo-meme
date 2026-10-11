// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/hash.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/range.hpp>

#include <string>
#include <utility>

namespace esses::model {

std::string_view rangeKindName(RangeKind k) noexcept {
    switch (k) {
    case RangeKind::Outing:
        return "Outing";
    case RangeKind::Lap:
        return "Lap";
    case RangeKind::Run:
        return "Run";
    case RangeKind::Stage:
        return "Stage";
    case RangeKind::Section:
        return "Section";
    case RangeKind::Split:
        return "Split";
    case RangeKind::Data:
        return "Data";
    }
    return "Outing";
}

std::string_view lapClassName(LapClass c) noexcept {
    switch (c) {
    case LapClass::Flying:
        return "Flying";
    case LapClass::Out:
        return "Out";
    case LapClass::In:
        return "In";
    case LapClass::Partial:
        return "Partial";
    case LapClass::Pit:
        return "Pit";
    case LapClass::None:
        return "None";
    }
    return "None";
}

std::optional<RangeKind> parseRangeKind(std::string_view text) noexcept {
    for (const RangeKind k : {RangeKind::Outing, RangeKind::Lap, RangeKind::Run, RangeKind::Stage,
                              RangeKind::Section, RangeKind::Split, RangeKind::Data}) {
        if (text == rangeKindName(k)) {
            return k;
        }
    }
    return std::nullopt;
}

std::optional<LapClass> parseLapClass(std::string_view text) noexcept {
    for (const LapClass c :
         {LapClass::Flying, LapClass::Out, LapClass::In, LapClass::Partial, LapClass::Pit, LapClass::None}) {
        if (text == lapClassName(c)) {
            return c;
        }
    }
    return std::nullopt;
}

std::string_view parentGroupPath(std::string_view path) noexcept {
    const auto colon = path.rfind(':');
    return colon == std::string_view::npos ? std::string_view{} : path.substr(0, colon);
}

std::string_view lastGroupSegment(std::string_view path) noexcept {
    const auto colon = path.rfind(':');
    return colon == std::string_view::npos ? path : path.substr(colon + 1);
}

RangeGroup RangeGroup::make(std::string path) {
    RangeGroup g;
    g.name = std::string(lastGroupSegment(path));
    g.path = std::move(path);
    return g;
}

namespace {

// Vec is std::vector<RangeGroup> or const std::vector<RangeGroup>.
template <class Vec> auto findIn(Vec& groups, std::string_view path) noexcept -> decltype(&groups.front()) {
    for (auto& g : groups) {
        if (g.path == path) {
            return &g;
        }
        // Children carry full paths that start with the parent's path and a colon.
        if (path.size() > g.path.size() && path.starts_with(g.path) && path[g.path.size()] == ':') {
            if (auto* found = findIn(g.children, path)) {
                return found;
            }
        }
    }
    return nullptr;
}

template <class Vec, class Fn> void visitGroups(Vec& groups, const Fn& fn) {
    for (auto& g : groups) {
        fn(g);
        visitGroups(g.children, fn);
    }
}

void inherit(std::vector<RangeGroup>& groups, const RangeGroupTree& previous) {
    for (RangeGroup& g : groups) {
        if (const RangeGroup* old = previous.find(g.path)) {
            g.version = old->ranges == g.ranges ? old->version : old->version + 1;
        } else {
            g.version = 1;
        }
        inherit(g.children, previous);
    }
}

} // namespace

const RangeGroup* RangeGroupTree::find(std::string_view path) const noexcept {
    return findIn(roots, path);
}

RangeGroup* RangeGroupTree::find(std::string_view path) noexcept {
    return findIn(roots, path);
}

std::uint64_t RangeGroupTree::rangeVersion(std::string_view path) const noexcept {
    const RangeGroup* g = find(path);
    return g == nullptr ? 0 : g->version;
}

bool RangeGroupTree::setGroup(RangeGroup g) {
    const std::string_view parentPath = parentGroupPath(g.path);
    std::vector<RangeGroup>* siblings = &roots;
    if (!parentPath.empty()) {
        RangeGroup* parent = find(parentPath);
        if (parent == nullptr) {
            return false;
        }
        siblings = &parent->children;
    }
    for (RangeGroup& existing : *siblings) {
        if (existing.path == g.path) {
            if (g.children.empty()) {
                g.children = std::move(existing.children);
            }
            existing = std::move(g);
            return true;
        }
    }
    siblings->push_back(std::move(g));
    return true;
}

bool RangeGroupTree::removeGroup(std::string_view path) {
    const std::string_view parentPath = parentGroupPath(path);
    std::vector<RangeGroup>* siblings = &roots;
    if (!parentPath.empty()) {
        RangeGroup* parent = find(parentPath);
        if (parent == nullptr) {
            return false;
        }
        siblings = &parent->children;
    }
    for (auto it = siblings->begin(); it != siblings->end(); ++it) {
        if (it->path == path) {
            siblings->erase(it);
            return true;
        }
    }
    return false;
}

void RangeGroupTree::inheritVersions(const RangeGroupTree& previous) {
    inherit(roots, previous);
}

void RangeGroupTree::forEach(const std::function<void(const RangeGroup&)>& fn) const {
    visitGroups(roots, fn);
}

void RangeGroupTree::forEach(const std::function<void(RangeGroup&)>& fn) {
    visitGroups(roots, fn);
}

RangeId rangeId(std::string_view groupPath, const MarkerId& anchor, std::string_view qualifier) {
    const std::string anchorText = anchor.v.str();
    return RangeId{uuidFromHash(hash128({"range", groupPath, anchorText, qualifier}))};
}

} // namespace esses::model
