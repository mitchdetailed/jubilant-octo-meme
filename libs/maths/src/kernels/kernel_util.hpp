// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers shared by the non-elementwise kernels of src/kernels (03 sections 5.2-5.7, 6.5, 6.6): reading and
// writing BlockViews of any dtype with their validity (a cleared bit or a float NaN is an invalid sample; an
// invalid output is written as NaN with a cleared bit), the literal parameters that compile() folds into the
// AST handed to makeIntrinsicKernel (README "Stage inputs of an intrinsic call"), window sizes at the result
// rate (03 section 2.4 rule 2) and the gate convention of the Stateful functions (03 section 5.4).
// Values are doubles throughout (F64 accumulators); the output dtype is whatever the stage buffer holds.
// Internal to esses_maths.
#pragma once

#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/ast.hpp>
#include <esses/maths/kernel.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses::maths::kern {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

constexpr bool isInvalid(double v) noexcept {
    return v != v; // NOLINT(misc-redundant-expression): true only for NaN
}

// Sample k of a view as a double; NaN when its validity bit is clear or its float value is NaN.
inline double readAt(const BlockView& v, std::uint32_t k) noexcept {
    if (v.validity != nullptr && ((v.validity[k >> 3] >> (k & 7u)) & 1u) == 0) {
        return kNaN;
    }
    switch (v.dtype) {
    case store::Dtype::F32:
        return static_cast<double>(static_cast<const float*>(v.data)[k]);
    case store::Dtype::F64:
        return static_cast<const double*>(v.data)[k];
    case store::Dtype::I32:
        return static_cast<double>(static_cast<const std::int32_t*>(v.data)[k]);
    case store::Dtype::I64:
        return static_cast<double>(static_cast<const std::int64_t*>(v.data)[k]);
    }
    return kNaN;
}

// Writes sample k: the value (NaN = invalid) and, when the view has a bitmap, its validity bit. Integer views
// store an invalid sample as 0 with a cleared bit.
inline void writeAt(const BlockView& v, std::uint32_t k, double value) noexcept {
    const bool valid = !isInvalid(value);
    switch (v.dtype) {
    case store::Dtype::F32:
        static_cast<float*>(v.data)[k] = static_cast<float>(value);
        break;
    case store::Dtype::F64:
        static_cast<double*>(v.data)[k] = value;
        break;
    case store::Dtype::I32:
        static_cast<std::int32_t*>(v.data)[k] = valid ? static_cast<std::int32_t>(value) : 0;
        break;
    case store::Dtype::I64:
        static_cast<std::int64_t*>(v.data)[k] = valid ? static_cast<std::int64_t>(value) : 0;
        break;
    }
    if (v.validity != nullptr) {
        const auto bit = static_cast<std::uint8_t>(1u << (k & 7u));
        v.validity[k >> 3] = valid ? static_cast<std::uint8_t>(v.validity[k >> 3] | bit)
                                   : static_cast<std::uint8_t>(v.validity[k >> 3] & ~bit);
    }
}

// Sample access for the kernels' loops: F64 views (what the 0.1 evaluator hands every stage) are read through
// their pointer and bitmap; any other dtype goes through readAt.
class Reader {
public:
    explicit Reader(const BlockView& v) noexcept
        : v_(&v), f64_(v.dtype == store::Dtype::F64 ? static_cast<const double*>(v.data) : nullptr),
          bits_(v.validity) {}

    double operator()(std::uint64_t k) const noexcept {
        if (f64_ != nullptr) {
            if (bits_ != nullptr && ((bits_[k >> 3] >> (k & 7u)) & 1u) == 0) {
                return kNaN;
            }
            return f64_[k];
        }
        return readAt(*v_, static_cast<std::uint32_t>(k));
    }

private:
    const BlockView* v_;
    const double* f64_;
    const std::uint8_t* bits_;
};

// Writes values (NaN = invalid). An F64 view is written through its pointer and its bitmap is filled from the
// NaN marks, eight samples at a time, when the writer goes out of scope (finish()); other dtypes go through
// writeAt.
class Writer {
public:
    explicit Writer(const BlockView& v) noexcept
        : v_(v), f64_(v.dtype == store::Dtype::F64 ? static_cast<double*>(v.data) : nullptr) {}
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) = delete;
    Writer& operator=(Writer&&) = delete;
    ~Writer() { finish(); }

    void operator()(std::uint32_t k, double value) const noexcept {
        if (f64_ != nullptr) {
            f64_[k] = value;
        } else {
            writeAt(v_, k, value);
        }
    }
    void finish() const noexcept {
        if (f64_ == nullptr || v_.validity == nullptr) {
            return;
        }
        std::uint32_t k = 0;
        for (; k + 8 <= v_.n; k += 8) {
            unsigned byte = 0;
            for (unsigned b = 0; b < 8; ++b) {
                byte |= (isInvalid(f64_[k + b]) ? 0u : 1u) << b;
            }
            v_.validity[k >> 3] = static_cast<std::uint8_t>(byte);
        }
        for (; k < v_.n; ++k) {
            writeAt(v_, k, f64_[k]);
        }
    }

private:
    BlockView v_;
    double* f64_;
};

// A whole view as doubles (NaN at invalid samples).
inline std::vector<double> loadAll(const BlockView& v) {
    std::vector<double> out(v.n);
    const Reader r(v);
    for (std::uint32_t k = 0; k < v.n; ++k) {
        out[k] = r(k);
    }
    return out;
}

inline void storeAll(const BlockView& out, std::span<const double> values) noexcept {
    const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(out.n, values.size()));
    const Writer w(out);
    for (std::uint32_t k = 0; k < n; ++k) {
        w(k, values[k]);
    }
}

// Non-zero and valid: a gate (cond) or a Bool operand of flip_flop, edge_delay and pulse_width (invalid =
// false, 03 section 5.4).
constexpr bool truthy(double v) noexcept {
    return !isInvalid(v) && v != 0.0;
}

// ---- literal parameters of a call (folded by compile()) ---------------------------------------------------

inline const Node* argNode(const Ast& ast, std::uint32_t call, std::size_t k) noexcept {
    if (call >= ast.nodes.size() || k >= ast.nodes[call].kids.size()) {
        return nullptr;
    }
    const std::uint32_t i = ast.nodes[call].kids[k];
    return i < ast.nodes.size() ? &ast.nodes[i] : nullptr;
}

inline std::size_t argCount(const Ast& ast, std::uint32_t call) noexcept {
    return call < ast.nodes.size() ? ast.nodes[call].kids.size() : 0;
}

inline bool isNumberLiteral(const Node* n) noexcept {
    return n != nullptr && (n->kind == NodeKind::IntLit || n->kind == NodeKind::DecLit);
}

// A window argument (03 section 2.4 rule 2): IntLit = points, DecLit = seconds (already in s).
struct WindowSpec {
    bool seconds = false;
    double value = 2.0; // points or seconds
    bool given = false;
};

// The number of points of a window at `rate`: seconds become n = 2 * floor(t * rate / 2) + 1 (odd, >= 1;
// the 1e-9 guards against t * rate landing just below a whole number); n of 0 or 1 is widened to 2 (03
// section 5.2).
inline std::uint64_t windowPoints(const WindowSpec& w, Rate rate) noexcept {
    double n = w.value;
    if (w.seconds) {
        const double samples = w.value * rate.hz();
        n = 2.0 * std::floor(samples / 2.0 + 1e-9) + 1.0;
    }
    if (!(n >= 2.0)) {
        return 2;
    }
    if (n > 1e15) {
        return std::uint64_t{1} << 50;
    }
    return static_cast<std::uint64_t>(n);
}

// The window [i - before, i + after] of n points: odd n centred, even n biased one point earlier.
struct WindowShape {
    std::uint64_t before = 0;
    std::uint64_t after = 0;
};
inline WindowShape windowShape(std::uint64_t n) noexcept {
    if (n % 2 == 1) {
        return {(n - 1) / 2, (n - 1) / 2};
    }
    return {n / 2, n / 2 - 1};
}

// ---- runtime failures (Kernel::status) --------------------------------------------------------------------

inline Failure kernelError(std::string_view code, std::string message) {
    ErrorContext ctx;
    ctx.detail = std::string(code);
    return Failure{Error{ErrorCode::Invalid, std::string(code) + ": " + message, ctx}};
}

} // namespace esses::maths::kern
