// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// FunctionRegistry (03 section 10.1, D-024): the functions an expression may call, intrinsics and external
// functions under one manifest shape. Names are looked up case-folded (a call whose spelling differs from the
// canonical lower-case name gives W-STYLE-CASE, 03 section 2.1). An external function whose name collides
// with an intrinsic is refused (E-FN-COLLISION). Not thread-safe for mutation: fill it before sharing it
// (const access is safe from any thread).
//
// M2 contract deviations (libs/maths/README.md): addExternal returns Result<void> so the collision reaches
// the caller; intrinsicId, external and builtin are additions that compile() and the hosts need.
#pragma once

#include <esses/base/result.hpp>
#include <esses/maths/intrinsics.hpp>
#include <esses/maths/math_function.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::maths {

class FunctionRegistry {
public:
    void addIntrinsic(const FunctionManifest& manifest, IntrinsicId id);
    // Invalid (E-FN-COLLISION in context.detail) when the name is taken.
    Result<void> addExternal(std::shared_ptr<IMathFunction> fn);

    const FunctionManifest* find(std::string_view nameCf) const;
    std::optional<IntrinsicId> intrinsicId(std::string_view nameCf) const;
    std::shared_ptr<IMathFunction> external(std::string_view nameCf) const;
    // Every manifest in name order (editor auto-complete, `esses-cli maths functions --json`).
    std::vector<const FunctionManifest*> all() const;

    // The process-wide registry holding every intrinsic (registerIntrinsics), built on first use.
    static const FunctionRegistry& builtin();

private:
    struct Entry {
        FunctionManifest manifest;
        std::optional<IntrinsicId> intrinsic;
        std::shared_ptr<IMathFunction> external;
    };
    std::map<std::string, Entry, std::less<>> entries_; // key: case-folded name
};

} // namespace esses::maths
