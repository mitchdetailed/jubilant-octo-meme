// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "ld/ld_decode.hpp"

#include <esses/base/byte_reader.hpp>

#include <bit>
#include <cmath>
#include <limits>

namespace esses::formats::ld {
namespace {

constexpr double kTwo53 = 9007199254740992.0; // 2^53

double pow10(int k) noexcept {
    return std::pow(10.0, static_cast<double>(k));
}

// Largest |raw| a sample type can hold, for the exactness bound; 0 when raw values are not integers.
double maxAbsRaw(SampleType t) noexcept {
    switch (t) {
    case SampleType::I8:
        return 128.0;
    case SampleType::U8:
        return 255.0;
    case SampleType::I16:
        return 32768.0;
    case SampleType::U16:
        return 65535.0;
    case SampleType::I32:
        return 2147483648.0;
    case SampleType::U32:
        return 4294967295.0;
    case SampleType::I64:
    case SampleType::F16:
    case SampleType::F32:
    case SampleType::F64:
        return 0.0;
    }
    return 0.0;
}

template <class Raw> Raw loadRaw(const std::byte* p) noexcept {
    return loadAs<Raw>(p, Endian::Little);
}

template <class Out> void put(void* dst, std::uint64_t i, double v) noexcept {
    if constexpr (std::is_same_v<Out, float>) {
        static_cast<float*>(dst)[i] = narrowToFloat(v);
    } else {
        static_cast<double*>(dst)[i] = v;
    }
}

// Raw -> double for every stored type.
template <class Raw> double rawToDouble(const std::byte* p) noexcept {
    return static_cast<double>(loadRaw<Raw>(p));
}

template <class Raw, class Out> void decodeScaled(const DecodeJob& job) noexcept {
    const ScalingPlan& plan = *job.plan;
    const std::byte* p = job.src;
    if (plan.identity) {
        for (std::uint64_t i = 0; i < job.count; ++i, p += sizeof(Raw)) {
            put<Out>(job.dst, i, rawToDouble<Raw>(p));
        }
        return;
    }
    for (std::uint64_t i = 0; i < job.count; ++i, p += sizeof(Raw)) {
        put<Out>(job.dst, i, applyScaling(plan, rawToDouble<Raw>(p)));
    }
}

template <class Out> void decodeHalf(const DecodeJob& job) noexcept {
    const ScalingPlan& plan = *job.plan;
    const std::byte* p = job.src;
    for (std::uint64_t i = 0; i < job.count; ++i, p += 2) {
        const double v = halfToDouble(loadRaw<std::uint16_t>(p));
        put<Out>(job.dst, i, plan.identity ? v : applyScaling(plan, v));
    }
}

template <class Raw> void decodeToI32(const DecodeJob& job) noexcept {
    auto* out = static_cast<std::int32_t*>(job.dst);
    const std::byte* p = job.src;
    for (std::uint64_t i = 0; i < job.count; ++i, p += sizeof(Raw)) {
        // Types of up to 32 bits other than u32 widen exactly (u32 is stored F64). An int8 sample is a signed
        // value, so the sign extension of signed char is the decode itself.
        out[i] = static_cast<std::int32_t>(loadRaw<Raw>(p)); // NOLINT(bugprone-signed-char-misuse)
    }
}

template <class Out> void dispatchFloatOut(const DecodeJob& job) noexcept {
    switch (job.type) {
    case SampleType::I8:
        decodeScaled<std::int8_t, Out>(job);
        break;
    case SampleType::U8:
        decodeScaled<std::uint8_t, Out>(job);
        break;
    case SampleType::I16:
        decodeScaled<std::int16_t, Out>(job);
        break;
    case SampleType::U16:
        decodeScaled<std::uint16_t, Out>(job);
        break;
    case SampleType::I32:
        decodeScaled<std::int32_t, Out>(job);
        break;
    case SampleType::U32:
        decodeScaled<std::uint32_t, Out>(job);
        break;
    case SampleType::I64:
        decodeScaled<std::int64_t, Out>(job);
        break;
    case SampleType::F16:
        decodeHalf<Out>(job);
        break;
    case SampleType::F32:
        decodeScaled<float, Out>(job);
        break;
    case SampleType::F64:
        decodeScaled<double, Out>(job);
        break;
    }
}

} // namespace

std::string_view shiftOrderName(ShiftOrder order) noexcept {
    return order == ShiftOrder::AfterMul ? "afterMul" : "beforeMul";
}

std::string_view vendorDtypeName(SampleType t) noexcept {
    switch (t) {
    case SampleType::I8:
        return "i8";
    case SampleType::U8:
        return "u8";
    case SampleType::I16:
        return "i16";
    case SampleType::U16:
        return "u16";
    case SampleType::I32:
        return "i32";
    case SampleType::U32:
        return "u32";
    case SampleType::I64:
        return "i64";
    case SampleType::F16:
        return "f16";
    case SampleType::F32:
        return "f32";
    case SampleType::F64:
        return "f64";
    }
    return "i32";
}

std::size_t sampleWidth(SampleType t) noexcept {
    switch (t) {
    case SampleType::I8:
    case SampleType::U8:
        return 1;
    case SampleType::I16:
    case SampleType::U16:
    case SampleType::F16:
        return 2;
    case SampleType::I32:
    case SampleType::U32:
    case SampleType::F32:
        return 4;
    case SampleType::I64:
    case SampleType::F64:
        return 8;
    }
    return 4;
}

ScalingPlan makeScalingPlan(SampleType type, std::int16_t shift, std::int16_t mul, std::int16_t scale,
                            std::int16_t dec, ShiftOrder order) noexcept {
    ScalingPlan plan;
    plan.order = order;
    const int s = scale == 0 ? 1 : scale;
    const int m = mul == 0 ? 1 : mul;
    plan.identity = s == 1 && m == 1 && dec == 0 && shift == 0;
    plan.mul = static_cast<double>(m);
    plan.shift = static_cast<double>(shift);
    if (dec >= 0) {
        plan.rawFactor = 1.0;
        plan.den = static_cast<double>(s) * pow10(dec);
    } else {
        plan.rawFactor = pow10(-dec);
        plan.den = static_cast<double>(s);
    }
    // Exact when |N| and |D| stay below 2^53 for every raw value of the type (all factors are integers).
    const double maxRaw = maxAbsRaw(type);
    if (maxRaw > 0.0 && std::abs(dec) <= 22 && std::isfinite(plan.den) && std::abs(plan.den) <= kTwo53) {
        const double am = std::abs(plan.mul);
        const double shiftTerm = std::abs(plan.shift) * std::abs(plan.den);
        const double bound = order == ShiftOrder::AfterMul ? maxRaw * plan.rawFactor * am + shiftTerm
                                                           : (maxRaw * plan.rawFactor + shiftTerm) * am;
        plan.exact = bound <= kTwo53;
    }
    return plan;
}

double applyScaling(const ScalingPlan& plan, double raw) noexcept {
    if (plan.identity) {
        return raw;
    }
    if (plan.exact) {
        const double n = plan.order == ShiftOrder::AfterMul
                             ? raw * plan.rawFactor * plan.mul + plan.shift * plan.den
                             : (raw * plan.rawFactor + plan.shift * plan.den) * plan.mul;
        return n / plan.den;
    }
    if (plan.order == ShiftOrder::AfterMul) {
        return raw * plan.rawFactor * plan.mul / plan.den + plan.shift;
    }
    return (raw * plan.rawFactor / plan.den + plan.shift) * plan.mul;
}

double halfToDouble(std::uint16_t bits) noexcept {
    const bool negative = (bits & 0x8000u) != 0;
    const unsigned exponent = (bits >> 10) & 0x1Fu;
    const unsigned mantissa = bits & 0x3FFu;
    double v = 0.0;
    if (exponent == 0) {
        v = std::ldexp(static_cast<double>(mantissa), -24); // subnormal
    } else if (exponent == 31) {
        v = mantissa == 0 ? std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::quiet_NaN();
    } else {
        v = std::ldexp(static_cast<double>(mantissa + 1024u), static_cast<int>(exponent) - 25);
    }
    return negative ? -v : v;
}

std::uint16_t doubleToHalf(double v) noexcept {
    const std::uint16_t sign = std::signbit(v) ? 0x8000u : 0u;
    if (std::isnan(v)) {
        return static_cast<std::uint16_t>(sign | 0x7E00u);
    }
    const double a = std::abs(v);
    if (a >= 65520.0) { // rounds to infinity (max finite half is 65504)
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    if (a < std::ldexp(1.0, -25)) { // below half the smallest subnormal (ties to even: 2^-25 -> 0)
        return sign;
    }
    int e = 0;
    std::frexp(a, &e);         // a = f * 2^e, f in [0.5, 1)
    int exponent = e - 1 + 15; // biased half exponent of a
    double scaled = 0.0;
    if (exponent <= 0) {
        scaled = std::ldexp(a, 24); // subnormal: units of 2^-24
        exponent = 0;
    } else {
        scaled = std::ldexp(a, 10 - (e - 1)); // in [1024, 2048)
    }
    double r = std::nearbyint(scaled); // round half to even (default rounding mode)
    unsigned bits = 0;
    if (exponent == 0) {
        bits = static_cast<unsigned>(r); // may carry into the normal range (1024 -> exponent 1)
    } else {
        if (r >= 2048.0) {
            r /= 2.0;
            ++exponent;
        }
        if (exponent >= 31) {
            return static_cast<std::uint16_t>(sign | 0x7C00u);
        }
        bits = (static_cast<unsigned>(exponent) << 10) | (static_cast<unsigned>(r) - 1024u);
    }
    return static_cast<std::uint16_t>(sign | bits);
}

float narrowToFloat(double v) noexcept {
    constexpr auto kMax = static_cast<double>(std::numeric_limits<float>::max());
    if (v > kMax || v < -kMax) {
        // Beyond the float range: round-to-nearest gives max for values below the halfway point to the next
        // power, infinity above it; the saturation keeps the conversion defined.
        const double limit = kMax + std::ldexp(1.0, 103); // max + half an ulp of max
        if (v >= limit) {
            return std::numeric_limits<float>::infinity();
        }
        if (v <= -limit) {
            return -std::numeric_limits<float>::infinity();
        }
        return v > 0 ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max();
    }
    return static_cast<float>(v);
}

void runDecodeJob(void* ctx) noexcept {
    const DecodeJob& job = *static_cast<const DecodeJob*>(ctx);
    if (job.count == 0) {
        return;
    }
    switch (job.out) {
    case store::Dtype::F32:
        dispatchFloatOut<float>(job);
        break;
    case store::Dtype::F64:
        dispatchFloatOut<double>(job);
        break;
    case store::Dtype::I32:
        switch (job.type) {
        case SampleType::I8:
            decodeToI32<std::int8_t>(job);
            break;
        case SampleType::U8:
            decodeToI32<std::uint8_t>(job);
            break;
        case SampleType::I16:
            decodeToI32<std::int16_t>(job);
            break;
        case SampleType::U16:
            decodeToI32<std::uint16_t>(job);
            break;
        case SampleType::I32:
            decodeToI32<std::int32_t>(job);
            break;
        case SampleType::U32: // stored F64 (05 section 3.5): 0xFFFFFFFF is 4294967295, not -1
        case SampleType::I64:
        case SampleType::F16:
        case SampleType::F32:
        case SampleType::F64:
            break; // never chosen (LdFile::decode rejects it)
        }
        break;
    case store::Dtype::I64:
        break; // not a channel dtype
    }
}

} // namespace esses::formats::ld
