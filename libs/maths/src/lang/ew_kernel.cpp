// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// EwKernel: the 0.1 interpreter of a fused elementwise subtree (03 section 6.0 item 2). Every instruction is
// one loop over the block; the operator or function is a template parameter of the loop, so the per-sample
// work is the inlined body of ew_ops.hpp and simple arithmetic auto-vectorises.

#include "ew_kernel.hpp"

#include "ew_ops.hpp"

#include <esses/maths/eval_context.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace esses::maths::lang {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool bitValid(const std::uint8_t* bits, std::uint32_t i) noexcept {
    return ((bits[i >> 3] >> (i & 7u)) & 1u) != 0;
}

void load(const BlockView& v, std::uint32_t offset, std::uint32_t n, double* dst) {
    if (v.data == nullptr) {
        std::fill(dst, dst + n, kNaN);
        return;
    }
    switch (v.dtype) {
    case store::Dtype::F32: {
        const float* p = static_cast<const float*>(v.data) + offset;
        for (std::uint32_t j = 0; j < n; ++j) {
            dst[j] = static_cast<double>(p[j]);
        }
        break;
    }
    case store::Dtype::F64:
        std::memcpy(dst, static_cast<const double*>(v.data) + offset, std::size_t{n} * sizeof(double));
        break;
    case store::Dtype::I32: {
        const std::int32_t* p = static_cast<const std::int32_t*>(v.data) + offset;
        for (std::uint32_t j = 0; j < n; ++j) {
            dst[j] = static_cast<double>(p[j]);
        }
        break;
    }
    case store::Dtype::I64: {
        const std::int64_t* p = static_cast<const std::int64_t*>(v.data) + offset;
        for (std::uint32_t j = 0; j < n; ++j) {
            dst[j] = static_cast<double>(p[j]);
        }
        break;
    }
    }
    if (v.validity != nullptr) {
        std::uint32_t j = 0;
        while (j < n) {
            const std::uint32_t i = offset + j;
            if ((i & 7u) == 0 && j + 8 <= n && v.validity[i >> 3] == 0xFF) {
                j += 8; // a whole valid byte
                continue;
            }
            if (!bitValid(v.validity, i)) {
                dst[j] = kNaN;
            }
            ++j;
        }
    }
}

void store(const double* r, const BlockView& out, std::uint32_t offset, std::uint32_t n) {
    if (out.data != nullptr) {
        switch (out.dtype) {
        case store::Dtype::F32: {
            float* p = static_cast<float*>(out.data) + offset;
            for (std::uint32_t j = 0; j < n; ++j) {
                p[j] = static_cast<float>(r[j]);
            }
            break;
        }
        case store::Dtype::F64:
            std::memcpy(static_cast<double*>(out.data) + offset, r, std::size_t{n} * sizeof(double));
            break;
        case store::Dtype::I32: {
            std::int32_t* p = static_cast<std::int32_t*>(out.data) + offset;
            for (std::uint32_t j = 0; j < n; ++j) {
                const double v = r[j];
                p[j] = !(v >= -2147483648.0 && v <= 2147483647.0) ? 0 : static_cast<std::int32_t>(v);
            }
            break;
        }
        case store::Dtype::I64: {
            std::int64_t* p = static_cast<std::int64_t*>(out.data) + offset;
            for (std::uint32_t j = 0; j < n; ++j) {
                const double v = r[j];
                p[j] = !(v > -9.2e18 && v < 9.2e18) ? 0 : static_cast<std::int64_t>(v);
            }
            break;
        }
        }
    }
    if (out.validity != nullptr) {
        std::uint32_t nans = 0;
        for (std::uint32_t j = 0; j < n; ++j) {
            nans += r[j] != r[j] ? 1u : 0u; // NaN
        }
        if (nans == 0 && (offset & 7u) == 0) {
            // The common case: every sample valid, whole bytes set at once.
            const std::uint32_t full = n / 8;
            std::memset(out.validity + offset / 8, 0xFF, full);
            for (std::uint32_t j = full * 8; j < n; ++j) {
                const std::uint32_t i = offset + j;
                out.validity[i >> 3] = static_cast<std::uint8_t>(out.validity[i >> 3] | (1u << (i & 7u)));
            }
            return;
        }
        std::uint32_t j = 0;
        while (j < n) {
            const std::uint32_t i = offset + j;
            if ((i & 7u) == 0 && j + 8 <= n) {
                unsigned byte = 0;
                for (unsigned b = 0; b < 8; ++b) {
                    byte |= (r[j + b] == r[j + b] ? 1u : 0u) << b; // not NaN
                }
                out.validity[i >> 3] = static_cast<std::uint8_t>(byte);
                j += 8;
                continue;
            }
            const auto bit = static_cast<std::uint8_t>(1u << (i & 7u));
            if (std::isnan(r[j])) {
                out.validity[i >> 3] = static_cast<std::uint8_t>(out.validity[i >> 3] & ~bit);
            } else {
                out.validity[i >> 3] = static_cast<std::uint8_t>(out.validity[i >> 3] | bit);
            }
            ++j;
        }
    }
}

template <class F> void loop1(double* d, const double* a, std::uint32_t n, F f) {
    for (std::uint32_t j = 0; j < n; ++j) {
        d[j] = f(a[j]);
    }
}

template <class F>
void loop2(double* d, const EwOperand& a, const EwOperand& b, const double* const* view, std::uint32_t n,
           F f) {
    if (!a.imm && !b.imm) {
        const double* ra = view[a.reg];
        const double* rb = view[b.reg];
        for (std::uint32_t j = 0; j < n; ++j) {
            d[j] = f(ra[j], rb[j]);
        }
    } else if (!a.imm) {
        const double* ra = view[a.reg];
        const double kb = b.value;
        for (std::uint32_t j = 0; j < n; ++j) {
            d[j] = f(ra[j], kb);
        }
    } else if (!b.imm) {
        const double ka = a.value;
        const double* rb = view[b.reg];
        for (std::uint32_t j = 0; j < n; ++j) {
            d[j] = f(ka, rb[j]);
        }
    } else {
        std::fill(d, d + n, f(a.value, b.value));
    }
}

using BinaryLoop = void (*)(double*, const EwOperand&, const EwOperand&, const double* const*, std::uint32_t);
using UnaryLoop = void (*)(double*, const double*, std::uint32_t);
using CallLoop = void (*)(double*, const double*, const double*, const double*, std::uint32_t);

template <BinaryOp Op>
void binaryLoop(double* d, const EwOperand& a, const EwOperand& b, const double* const* view,
                std::uint32_t n) {
    loop2(d, a, b, view, n, [](double x, double y) { return ew::binaryOp<Op>(x, y); });
}

template <UnaryOp Op> void unaryLoop(double* d, const double* a, std::uint32_t n) {
    loop1(d, a, n, [](double x) { return ew::unaryOp<Op>(x); });
}

template <IntrinsicId Id>
void callLoop(double* d, const double* a, const double* b, const double* c, std::uint32_t n) {
    for (std::uint32_t j = 0; j < n; ++j) {
        d[j] = ew::callOp<Id>(a[j], b[j], c[j]);
    }
}

template <std::size_t... I> constexpr auto binaryTable(std::index_sequence<I...> /*unused*/) {
    return std::array<BinaryLoop, sizeof...(I)>{&binaryLoop<static_cast<BinaryOp>(I)>...};
}
template <std::size_t... I> constexpr auto unaryTable(std::index_sequence<I...> /*unused*/) {
    return std::array<UnaryLoop, sizeof...(I)>{&unaryLoop<static_cast<UnaryOp>(I)>...};
}
template <std::size_t... I> constexpr auto callTable(std::index_sequence<I...> /*unused*/) {
    return std::array<CallLoop, sizeof...(I)>{&callLoop<static_cast<IntrinsicId>(I)>...};
}

constexpr std::size_t kBinaryOps = static_cast<std::size_t>(BinaryOp::Or) + 1;
constexpr std::size_t kUnaryOps = static_cast<std::size_t>(UnaryOp::BitNot) + 1;
constexpr std::size_t kCalls = static_cast<std::size_t>(IntrinsicId::Count);

constexpr auto kBinaryLoops = binaryTable(std::make_index_sequence<kBinaryOps>{});
constexpr auto kUnaryLoops = unaryTable(std::make_index_sequence<kUnaryOps>{});
constexpr auto kCallLoops = callTable(std::make_index_sequence<kCalls>{});

} // namespace

EwKernel::EwKernel(std::vector<EwInstr> code, std::uint16_t regs, std::uint16_t result, std::uint64_t seed)
    : code_(std::move(code)), regs_(regs), result_(result), seed_(seed), evalSeed_(seed) {}

void EwKernel::reset(const EvalContext& ctx) {
    std::uint64_t src = 0;
    if (ctx.source != nullptr) {
        src = ctx.source->id.lo ^ ew::splitmix64(ctx.source->id.hi);
    }
    evalSeed_ = ew::splitmix64(seed_ ^ src);
}

void EwKernel::run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) {
    // Registers, then three scratch registers for immediate operands of unary operators and calls.
    const std::size_t needed = (std::size_t{regs_} + 3) * kBlockSamples;
    if (scratch_.size() < needed) {
        scratch_.assign(needed, 0.0);
    }
    views_.resize(std::max<std::size_t>(regs_, 1));
    for (std::uint32_t offset = 0; offset < ctx.n; offset += kBlockSamples) {
        if (ctx.cancel.cancelled()) {
            return;
        }
        const std::uint32_t n = std::min(kBlockSamples, ctx.n - offset);
        runBlock(ctx.first + offset, offset, n, in, out);
    }
}

void EwKernel::step(const BlockCtx& extent, std::span<const kern::StreamInput> in, double* out,
                    std::uint64_t from, std::uint64_t to) {
    const auto n = static_cast<std::uint32_t>(to - from);
    stepViews_.resize(in.size());
    for (std::size_t k = 0; k < in.size(); ++k) {
        // The loads only read the input (BlockView::data is not const).
        stepViews_[k] = BlockView{const_cast<double*>(in[k].at(from)), nullptr, store::Dtype::F64, n};
    }
    BlockCtx b;
    b.first = extent.first + static_cast<SampleIdx>(from);
    b.n = n;
    b.t0 = extent.t0;
    b.rate = extent.rate;
    b.cancel = extent.cancel;
    run(b, stepViews_, BlockView{out, nullptr, store::Dtype::F64, n});
}

void EwKernel::runBlock(SampleIdx first, std::uint32_t offset, std::uint32_t n, std::span<const BlockView> in,
                        BlockView out) {
    double* regs = scratch_.data();
    const auto reg = [&](std::uint16_t r) { return regs + std::size_t{r} * kBlockSamples; };
    // What each register holds: its own storage, or (after a Load of an F64 input without a bitmap, whose
    // samples need no conversion) the input's samples themselves, read in place instead of copied.
    const double** view = views_.data();
    for (std::uint16_t r = 0; r < regs_; ++r) {
        view[r] = reg(r);
    }
    // An operand as a register: immediates go to one of the three scratch registers.
    const auto operand = [&](const EwOperand& o, int slot) -> const double* {
        if (!o.imm) {
            return view[o.reg];
        }
        double* s = regs + (std::size_t{regs_} + static_cast<std::size_t>(slot)) * kBlockSamples;
        std::fill(s, s + n, o.value);
        return s;
    };
    for (const EwInstr& ins : code_) {
        double* d = reg(ins.dst);
        if (ins.op == EwInstr::Op::Load && ins.input < in.size() &&
            in[ins.input].dtype == store::Dtype::F64 && in[ins.input].validity == nullptr &&
            in[ins.input].data != nullptr) {
            view[ins.dst] = static_cast<const double*>(in[ins.input].data) + offset;
            continue;
        }
        switch (ins.op) {
        case EwInstr::Op::Load:
            if (ins.input < in.size()) {
                load(in[ins.input], offset, n, d);
            } else {
                std::fill(d, d + n, kNaN);
            }
            break;
        case EwInstr::Op::Fill:
            std::fill(d, d + n, ins.a.value);
            break;
        case EwInstr::Op::Unary:
            kUnaryLoops[static_cast<std::size_t>(ins.uop)](d, operand(ins.a, 0), n);
            break;
        case EwInstr::Op::Binary:
            kBinaryLoops[static_cast<std::size_t>(ins.bop)](d, ins.a, ins.b, view, n);
            break;
        case EwInstr::Op::Call:
            if (static_cast<std::size_t>(ins.fn) < kCalls) {
                kCallLoops[static_cast<std::size_t>(ins.fn)](d, operand(ins.a, 0), operand(ins.b, 1),
                                                             operand(ins.c, 2), n);
            } else {
                std::fill(d, d + n, kNaN);
            }
            break;
        case EwInstr::Op::RandVal:
            std::fill(d, d + n, ew::unitInterval(ew::splitmix64(evalSeed_)));
            break;
        case EwInstr::Op::RandChan:
            for (std::uint32_t j = 0; j < n; ++j) {
                const auto index = static_cast<std::uint64_t>(first + static_cast<SampleIdx>(j));
                d[j] = ew::unitInterval(ew::splitmix64(evalSeed_ ^ ew::splitmix64(index)));
            }
            break;
        case EwInstr::Op::Affine: {
            const double* a = operand(ins.a, 0);
            const double k0 = ins.k0;
            const double k1 = ins.k1;
            for (std::uint32_t j = 0; j < n; ++j) {
                d[j] = a[j] * k0 + k1;
            }
            break;
        }
        }
        view[ins.dst] = d; // written: the register holds its own storage again
    }
    store(view[result_], out, offset, n);
}

} // namespace esses::maths::lang
