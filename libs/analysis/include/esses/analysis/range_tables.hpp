// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RangeTables: the immutable interval tables per range-group path that workers read (01 section 5.3 rules 2
// and 9; 03 section 6.1 BlockCtx::ranges and 03 section 7.5 EvalContext::ranges; DistanceIndex builds, 01
// section 5.2). Published as std::shared_ptr<const RangeTables>; a rebuild after a marker edit publishes a
// new pointer and never mutates the old object.
#pragma once

#include <esses/base/time.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/range.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

struct RangeInterval {
    Micros start = 0; // [start, end)
    Micros end = 0;
    model::RangeId id;
    model::LapClass lapClass = model::LapClass::None;
    int number = 0;
    bool trusted = true;
};

class RangeTables {
public:
    // Every group path of the layer's tree, enabled ranges only, in time order, with the group versions.
    static std::shared_ptr<const RangeTables> build(const model::DataLayer& layer);

    // Empty when the path is absent.
    std::span<const RangeInterval> group(std::string_view path) const noexcept;
    // The interval of `path` containing t, or nullptr.
    const RangeInterval* find(std::string_view path, Micros t) const noexcept;
    // RangeGroupTree::rangeVersion of the snapshot (0 when absent).
    std::uint64_t rangeVersion(std::string_view path) const noexcept;
    // True when the layer has a group with this exact path (it may hold no enabled range); the maths range
    // functions report E-RANGE-UNKNOWN otherwise (03 section 2.4 rule 3).
    bool contains(std::string_view path) const noexcept;
    // Every group path, sorted (the list of available paths in that diagnostic).
    std::vector<std::string> paths() const;

private:
    std::map<std::string, std::vector<RangeInterval>, std::less<>> groups_;
    std::map<std::string, std::uint64_t, std::less<>> versions_;
};

} // namespace esses::analysis
