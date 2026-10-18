// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// runStreamed() and noNaN() of stream.hpp.

#include "stream.hpp"

#include "kernel_util.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esses::maths::kern {

bool noNaN(const double* p, std::uint64_t n) noexcept {
    // Counted branch-free in blocks so the scan vectorises; stops at the first block with a NaN.
    constexpr std::uint64_t kBlock = 256;
    for (std::uint64_t at = 0; at < n; at += kBlock) {
        const std::uint64_t end = n - at < kBlock ? n : at + kBlock;
        std::uint32_t nans = 0;
        for (std::uint64_t i = at; i < end; ++i) {
            nans += p[i] != p[i] ? 1u : 0u; // NOLINT(misc-redundant-expression): NaN
        }
        if (nans != 0) {
            return false;
        }
    }
    return true;
}

void runStreamed(Streamable& kernel, BlockCtx& ctx, std::span<const BlockView> in, BlockView out) {
    const std::uint64_t n = out.n;
    std::vector<std::vector<double>> copies(in.size());
    std::vector<StreamInput> views(in.size());
    for (std::size_t k = 0; k < in.size(); ++k) {
        const BlockView& v = in[k];
        if (v.dtype == store::Dtype::F64 && v.validity == nullptr && v.data != nullptr) {
            views[k] = StreamInput{static_cast<const double*>(v.data), 0, v.n};
        } else {
            copies[k] = loadAll(v); // NaN at bit-cleared samples
            views[k] = StreamInput{copies[k].data(), 0, v.n};
        }
    }
    kernel.begin(ctx);
    if (n == 0) {
        return;
    }
    if (out.dtype == store::Dtype::F64) {
        kernel.step(ctx, views, static_cast<double*>(out.data), 0, n);
        const Writer bits(out); // the values are in place; the bitmap follows their NaN marks
    } else {
        std::vector<double> values(n);
        kernel.step(ctx, views, values.data(), 0, n);
        storeAll(out, values);
    }
}

} // namespace esses::maths::kern
