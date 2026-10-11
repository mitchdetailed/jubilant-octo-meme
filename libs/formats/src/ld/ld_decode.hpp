// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// .ld sample decode kernels (05 sections 3.5, 3.6; port specification R8). Decoding dispatches on the stored
// sample type (derived from the (dtype_a, dtype) pair, never from dtype_a alone) and applies the channel's
// scaling in F64, then narrows to the output dtype.
//
// Scaling, with scale 0 -> 1 and mul 0 -> 1 (a zero scale never drops mul):
//   afterMul  (default, D-037 as amended):  value = raw / scale * 10^-dec * mul + shift
//   beforeMul (dialect option):             value = (raw / scale * 10^-dec + shift) * mul
// Both are evaluated as one rational N / D with D = scale * 10^dec (dec >= 0) or scale (dec < 0). When every
// term of N and D is an integer below 2^53 (all integer channels seen so far), N and D are exact in F64 and
// the single division is correctly rounded, so the F64 result equals the exactly rounded rational value.
#pragma once

#include <esses/store/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace esses::formats::ld {

enum class ShiftOrder : std::uint8_t { AfterMul, BeforeMul };
std::string_view shiftOrderName(ShiftOrder order) noexcept; // "afterMul", "beforeMul"

// Stored sample type of a channel record.
enum class SampleType : std::uint8_t { I8, U8, I16, U16, I32, U32, I64, F16, F32, F64 };
std::string_view vendorDtypeName(SampleType t) noexcept; // "i8", "u8", "i16", ..., "f64"
std::size_t sampleWidth(SampleType t) noexcept;
constexpr bool isFloatSample(SampleType t) noexcept {
    return t == SampleType::F16 || t == SampleType::F32 || t == SampleType::F64;
}

struct ScalingPlan {
    bool identity = true; // scale in {0,1}, mul in {0,1}, dec 0, shift 0: values pass through unchanged
    bool exact = false;   // N and D exact in F64 for this sample type (single correctly rounded division)
    ShiftOrder order = ShiftOrder::AfterMul;
    double rawFactor = 1.0; // 10^-dec for dec < 0, else 1
    double mul = 1.0;
    double den = 1.0; // scale * 10^dec for dec >= 0, else scale
    double shift = 0.0;
};

ScalingPlan makeScalingPlan(SampleType type, std::int16_t shift, std::int16_t mul, std::int16_t scale,
                            std::int16_t dec, ShiftOrder order) noexcept;
double applyScaling(const ScalingPlan& plan, double raw) noexcept;

// IEEE binary16 <-> binary64 (round to nearest even on the way down).
double halfToDouble(std::uint16_t bits) noexcept;
std::uint16_t doubleToHalf(double v) noexcept;
// Saturating double -> float: values beyond the float range become +-inf instead of undefined behaviour.
float narrowToFloat(double v) noexcept;

// Plain decode routine for MappedFile::guarded(): owns no objects with destructors.
struct DecodeJob {
    const std::byte* src = nullptr; // first stored sample of the range
    std::uint64_t count = 0;
    SampleType type = SampleType::F32;
    store::Dtype out = store::Dtype::F32;
    void* dst = nullptr; // count * dtypeSize(out) bytes
    const ScalingPlan* plan = nullptr;
};
void runDecodeJob(void* job) noexcept;

} // namespace esses::formats::ld
