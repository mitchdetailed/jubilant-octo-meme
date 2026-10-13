// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// FunctionRegistry (03 section 10.1): case-folded name -> manifest with the intrinsic id or the external
// function. The container is final; builtin() is complete once registerIntrinsics() is.

#include <esses/base/casefold.hpp>
#include <esses/maths/function_registry.hpp>

#include <utility>

namespace esses::maths {

void FunctionRegistry::addIntrinsic(const FunctionManifest& manifest, IntrinsicId id) {
    Entry e;
    e.manifest = manifest;
    e.intrinsic = id;
    entries_.insert_or_assign(caseFold(manifest.name), std::move(e));
}

Result<void> FunctionRegistry::addExternal(std::shared_ptr<IMathFunction> fn) {
    if (fn == nullptr) {
        return fail(ErrorCode::Invalid, "addExternal: null function");
    }
    std::string key = caseFold(fn->manifest().name);
    if (entries_.find(key) != entries_.end()) {
        ErrorContext ctx;
        ctx.detail = "E-FN-COLLISION";
        return fail(ErrorCode::Invalid, "function '" + fn->manifest().name + "' is already defined", ctx);
    }
    Entry e;
    e.manifest = fn->manifest();
    e.external = std::move(fn);
    entries_.emplace(std::move(key), std::move(e));
    return ok();
}

const FunctionManifest* FunctionRegistry::find(std::string_view nameCf) const {
    const auto it = entries_.find(nameCf);
    return it == entries_.end() ? nullptr : &it->second.manifest;
}

std::optional<IntrinsicId> FunctionRegistry::intrinsicId(std::string_view nameCf) const {
    const auto it = entries_.find(nameCf);
    return it == entries_.end() ? std::nullopt : it->second.intrinsic;
}

std::shared_ptr<IMathFunction> FunctionRegistry::external(std::string_view nameCf) const {
    const auto it = entries_.find(nameCf);
    return it == entries_.end() ? nullptr : it->second.external;
}

std::vector<const FunctionManifest*> FunctionRegistry::all() const {
    std::vector<const FunctionManifest*> out;
    out.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        out.push_back(&entry.manifest);
    }
    return out;
}

const FunctionRegistry& FunctionRegistry::builtin() {
    static const FunctionRegistry registry = [] {
        FunctionRegistry r;
        registerIntrinsics(r);
        return r;
    }();
    return registry;
}

} // namespace esses::maths
