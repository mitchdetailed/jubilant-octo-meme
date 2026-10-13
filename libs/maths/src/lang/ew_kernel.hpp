// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// EwKernel (03 sections 6.0 item 2 and 6.1): a fused elementwise subtree, interpreted block by block with one
// tight loop per instruction over double registers of kBlockSamples lanes (the 0.1 implementation; the v1.0
// register bytecode replaces the interpreter, not the interface). NaN is "invalid" inside the registers
// (ew_ops.hpp); loads turn invalid input samples into NaN and the store writes NaN plus the validity bitmap.
// Integer-typed values are exact in the F64 lanes up to 2^53 (03 section 6.1). Internal to esses_maths.
#pragma once

#include "../kernels/stream.hpp"

#include <esses/maths/ast.hpp>
#include <esses/maths/intrinsics.hpp>
#include <esses/maths/kernel.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace esses::maths::lang {

struct EwOperand { // an immediate unless it names a register (unused operands are immediates)
    bool imm = true;
    std::uint16_t reg = 0;
    double value = 0.0; // imm
};

struct EwInstr {
    enum class Op : std::uint8_t {
        Load,     // dst = input `input`, invalid samples as NaN
        Fill,     // dst = a.value
        Unary,    // dst = uop(a)
        Binary,   // dst = bop(a, b)
        Call,     // dst = fn(a, b, c)
        RandVal,  // dst = one value in [0, 1) per evaluation
        RandChan, // dst[i] = a value in [0, 1) per absolute sample index
        Affine    // dst = a * k0 + k1 (the result unit to base conversion)
    };
    Op op = Op::Fill;
    UnaryOp uop{};
    BinaryOp bop{};
    IntrinsicId fn = IntrinsicId::Count;
    std::uint16_t dst = 0;
    std::uint16_t input = 0;
    EwOperand a, b, c;
    double k0 = 1.0;
    double k1 = 0.0;
};

class EwKernel final : public Kernel, public kern::Streamable {
public:
    // `code` writes registers [0, regs); `result` holds the output; `seed` is the plan's random seed (D-087),
    // mixed with the source id by reset().
    EwKernel(std::vector<EwInstr> code, std::uint16_t regs, std::uint16_t result, std::uint64_t seed);

    KernelClass cls() const override { return KernelClass::Elementwise; }
    // Any ctx.n: the kernel steps through it in blocks of kBlockSamples. `in` follows Stage::inputs.
    void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) override;
    void reset(const EvalContext& ctx) override;
    // Streamable (D-191): a step is run() over the step's samples (every output depends on its own inputs).
    void begin(const BlockCtx& /*extent*/) override {}
    std::optional<std::uint64_t> restartPoint(std::uint64_t need) const noexcept override { return need; }
    void step(const BlockCtx& extent, std::span<const kern::StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override;

    const std::vector<EwInstr>& code() const noexcept { return code_; }
    std::uint16_t registers() const noexcept { return regs_; }

private:
    void runBlock(SampleIdx first, std::uint32_t offset, std::uint32_t n, std::span<const BlockView> in,
                  BlockView out);

    std::vector<EwInstr> code_;
    std::uint16_t regs_ = 0;
    std::uint16_t result_ = 0;
    std::uint64_t seed_ = 0;
    std::uint64_t evalSeed_ = 0;
    std::vector<double> scratch_;
    std::vector<const double*> views_; // per register: its storage or an input read in place (runBlock)
    std::vector<BlockView> stepViews_;
};

} // namespace esses::maths::lang
