// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The IIR kernels (03 section 5.3, D-022): filter_lp / filter_hp (first-order RC sections) and the
// Chebyshev type I filters filter_cheby_lp / _hp / _bp / _bs (Butterworth when the ripple is 0). Every filter
// is a list of passes, each a cascade of second-order sections in transposed direct form II with F64 state,
// run forward or backward over the whole extent:
//
//   filter_lp/hp(x, fc[, order])         order 1, 2, 3: that many forward RC passes (default 1); -1: one
//                                        backward pass; -2: forward then backward (zero phase). The RC
//                                        section is the classic discretisation: low pass y += a (x - y) with
//                                        a = dt / (RC + dt), high pass y = b (y' + x - x') with
//                                        b = RC / (RC + dt), RC = 1 / (2 pi fc) [compat: iir.rc-form].
//   filter_cheby_lp/hp(x, fc[, order[, ripple]])
//   filter_cheby_bp/bs(x, f_lo, f_hi[, order[, ripple]])
//                                        order 2, 4, 6: one forward pass with a prototype of that order
//                                        (default 2); -4, -8, -12: forward then backward with half the order
//                                        each way. The analogue prototype (ripple r % > 0: Chebyshev type I,
//                                        the pass-band gain rippling between 1 and 1 + r/100 and fc the edge
//                                        of the ripple band; r = 0: Butterworth, fc the -3 dB point) is
//                                        mapped by the bilinear transform with pre-warped edges; band filters
//                                        use the centre sqrt(f_lo f_hi) and width f_hi - f_lo of the
//                                        pre-warped edges and have twice the prototype's poles. Each section
//                                        is normalised to gain 1 at DC (low pass, band stop), at Nyquist
//                                        (high pass) or at the centre (band pass) [compat: iir.cheby-design].
//
// A pass is primed with the first valid sample of its input as a steady state (forward; backward: the last
// valid sample), so a constant input has no start-up transient; an invalid sample is passed through invalid
// while the state holds, and the first valid sample after a gap of more than 1 s re-primes the pass
// [compat: iir.priming]. A cutoff at or above half the result rate is E-ARG-VALUE when the kernel learns the
// rate (Kernel::status). The design is our own from the textbook formulas; Iir1 is not vendored (D-169).
//
// A filter whose passes all run forward is Streamable (stream.hpp, D-191): its passes run interleaved, each
// sample through pass 1, then pass 2, ..., with the state carried from step to step, which gives every pass
// the same input sequence as running the passes one after the other. A filter with a backward pass (orders
// -1, -2, -4, -8, -12) runs over the whole extent. The common shapes keep their state in locals (runPasses).

#include "kernel_util.hpp"
#include "kernels_internal.hpp"
#include "stream.hpp"

#include <esses/maths/eval_context.hpp>
#include <esses/maths/kernel.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <locale>
#include <memory>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace esses::maths::kern {

namespace {

using Complex = std::complex<double>;
constexpr double kPi = std::numbers::pi;
constexpr double kGapSeconds = 1.0; // a longer gap re-primes the filter

struct Section { // y = b0 x + s1; s1 = b1 x - a1 y + s2; s2 = b2 x - a2 y
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    double dcGain() const noexcept { return (b0 + b1 + b2) / (1.0 + a1 + a2); }
    Complex response(Complex z) const noexcept {
        const Complex zi = 1.0 / z;
        return (b0 + b1 * zi + b2 * zi * zi) / (1.0 + a1 * zi + a2 * zi * zi);
    }
    void scale(double g) noexcept {
        b0 *= g;
        b1 *= g;
        b2 *= g;
    }
};

struct Pass {
    std::vector<Section> sections;
    bool backward = false;
};

// The state of one pass between samples: (s1, s2) per section, whether it is primed, and the invalid run
// before the next sample.
struct PassState {
    std::vector<double> s1;
    std::vector<double> s2;
    bool primed = false;
    std::size_t gap = 0;
};

// One sample through a pass of m sections (v becomes the pass's output): an invalid sample passes through
// invalid and extends the gap; a valid one primes the pass as a steady state the first time and after a gap
// longer than gapSamples, then runs the cascade.
inline void passSample(double& v, const Section* sec, std::size_t m, double* s1, double* s2, bool& primed,
                       std::size_t& gap, double gapSamples) noexcept {
    if (isInvalid(v)) {
        ++gap;
        return;
    }
    if (!primed || static_cast<double>(gap) > gapSamples) {
        double u = v;
        for (std::size_t k = 0; k < m; ++k) {
            const Section& s = sec[k];
            const double y = s.dcGain() * u;
            s2[k] = s.b2 * u - s.a2 * y;
            s1[k] = s.b1 * u - s.a1 * y + s2[k];
            u = y;
        }
        primed = true;
    }
    gap = 0;
    for (std::size_t k = 0; k < m; ++k) {
        const Section& s = sec[k];
        const double y = s.b0 * v + s1[k];
        s1[k] = s.b1 * v - s.a1 * y + s2[k];
        s2[k] = s.b2 * v - s.a2 * y;
        v = y;
    }
}

// Runs P passes of M sections each (all in the direction of `stride`) over n samples, sample by sample:
// out[k * stride] is x[k * stride] after pass 0, then pass 1, ... Every pass sees the previous pass's output
// at each sample in the same order as when the passes run one after the other over the whole extent, so the
// values are the same. The state is held in locals for the run and goes back to `st` at its end; `sec` holds
// the P * M sections pass by pass. x and out may be the same buffer.
template <std::size_t P, std::size_t M>
void runPasses(const Section* sec, PassState* st, double gapSamples, const double* x, double* out,
               std::size_t n, std::ptrdiff_t stride) noexcept {
    Section c[P][M];
    double s1[P][M];
    double s2[P][M];
    bool primed[P];
    std::size_t gap[P];
    for (std::size_t p = 0; p < P; ++p) {
        for (std::size_t k = 0; k < M; ++k) {
            c[p][k] = sec[p * M + k];
            s1[p][k] = st[p].s1[k];
            s2[p][k] = st[p].s2[k];
        }
        primed[p] = st[p].primed;
        gap[p] = st[p].gap;
    }
    for (std::size_t step = 0; step < n; ++step) {
        const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(step) * stride;
        double v = x[at];
        for (std::size_t p = 0; p < P; ++p) {
            passSample(v, c[p], M, s1[p], s2[p], primed[p], gap[p], gapSamples);
        }
        out[at] = v;
    }
    for (std::size_t p = 0; p < P; ++p) {
        for (std::size_t k = 0; k < M; ++k) {
            st[p].s1[k] = s1[p][k];
            st[p].s2[k] = s2[p][k];
        }
        st[p].primed = primed[p];
        st[p].gap = gap[p];
    }
}

// P passes of one section each (the RC filters): the same per-sample operations as passSample() with each
// pass's state in a local struct the compiler keeps in registers, so the passes' dependency chains overlap.
template <std::size_t P>
void runSingleSectionPasses(const Section* sec, PassState* st, double gapSamples, const double* x,
                            double* out, std::size_t n, std::ptrdiff_t stride) noexcept {
    struct One {
        Section c;
        double s1 = 0.0;
        double s2 = 0.0;
        bool primed = false;
        std::size_t gap = 0;
    };
    One r[P];
    for (std::size_t p = 0; p < P; ++p) {
        r[p] = One{sec[p], st[p].s1[0], st[p].s2[0], st[p].primed, st[p].gap};
    }
    for (std::size_t step = 0; step < n; ++step) {
        const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(step) * stride;
        double v = x[at];
        for (std::size_t p = 0; p < P; ++p) {
            One& q = r[p];
            if (isInvalid(v)) {
                ++q.gap;
                continue;
            }
            if (!q.primed || static_cast<double>(q.gap) > gapSamples) {
                const double y = q.c.dcGain() * v;
                q.s2 = q.c.b2 * v - q.c.a2 * y;
                q.s1 = q.c.b1 * v - q.c.a1 * y + q.s2;
                q.primed = true;
            }
            q.gap = 0;
            const double y = q.c.b0 * v + q.s1;
            q.s1 = q.c.b1 * v - q.c.a1 * y + q.s2;
            q.s2 = q.c.b2 * v - q.c.a2 * y;
            v = y;
        }
        out[at] = v;
    }
    for (std::size_t p = 0; p < P; ++p) {
        st[p].s1[0] = r[p].s1;
        st[p].s2[0] = r[p].s2;
        st[p].primed = r[p].primed;
        st[p].gap = r[p].gap;
    }
}

// Any number of passes and sections (the state stays in `st`).
void runPassesAny(const Section* sec, std::size_t passes, std::size_t m, PassState* st, double gapSamples,
                  const double* x, double* out, std::size_t n, std::ptrdiff_t stride) noexcept {
    for (std::size_t step = 0; step < n; ++step) {
        const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(step) * stride;
        double v = x[at];
        for (std::size_t p = 0; p < passes; ++p) {
            passSample(v, sec + p * m, m, st[p].s1.data(), st[p].s2.data(), st[p].primed, st[p].gap,
                       gapSamples);
        }
        out[at] = v;
    }
}

// The register-resident forms for the common shapes, RC orders 1-3 (1-3 passes of one section) and
// Chebyshev low and high passes of order 2-6 (one pass of 1-3 sections); runPassesAny otherwise.
void filterSamples(const Section* sec, std::size_t passes, std::size_t m, PassState* st, double gapSamples,
                   const double* x, double* out, std::size_t n, std::ptrdiff_t stride) noexcept {
    if (m == 1 && passes == 1) {
        runSingleSectionPasses<1>(sec, st, gapSamples, x, out, n, stride);
    } else if (m == 1 && passes == 2) {
        runSingleSectionPasses<2>(sec, st, gapSamples, x, out, n, stride);
    } else if (m == 1 && passes == 3) {
        runSingleSectionPasses<3>(sec, st, gapSamples, x, out, n, stride);
    } else if (m == 2 && passes == 1) {
        runPasses<1, 2>(sec, st, gapSamples, x, out, n, stride);
    } else if (m == 3 && passes == 1) {
        runPasses<1, 3>(sec, st, gapSamples, x, out, n, stride);
    } else {
        runPassesAny(sec, passes, m, st, gapSamples, x, out, n, stride);
    }
}

PassState freshState(const Pass& pass) {
    PassState st;
    st.s1.assign(pass.sections.size(), 0.0);
    st.s2.assign(pass.sections.size(), 0.0);
    return st;
}

// Runs one pass over `x` in place (NaN = invalid), forward or backward.
// With `from`: the pass reads `from` and writes `x` (the first pass of a whole-extent run reads the stage
// input directly; an empty pass copies it).
void runPass(std::span<double> x, const Pass& pass, double rateHz, const double* from = nullptr) {
    const std::size_t n = x.size();
    const std::size_t m = pass.sections.size();
    if (n == 0) {
        return;
    }
    if (m == 0) {
        if (from != nullptr) {
            std::copy(from, from + n, x.data());
        }
        return;
    }
    PassState st = freshState(pass);
    const std::size_t at = pass.backward ? n - 1 : 0;
    const double* src = (from != nullptr ? from : x.data()) + at;
    filterSamples(pass.sections.data(), 1, m, &st, kGapSeconds * rateHz, src, x.data() + at, n,
                  pass.backward ? -1 : 1);
}

// ---- first-order RC sections
// -------------------------------------------------------------------------------

Section rcSection(bool highPass, double fc, double rateHz) {
    const double rc = 1.0 / (2.0 * kPi * fc);
    const double dt = 1.0 / rateHz;
    Section s;
    if (!highPass) {
        const double a = dt / (rc + dt);
        s.b0 = a;
        s.a1 = -(1.0 - a);
    } else {
        const double b = rc / (rc + dt);
        s.b0 = b;
        s.b1 = -b;
        s.a1 = -b;
    }
    return s;
}

// ---- Chebyshev type I / Butterworth design
// -----------------------------------------------------------------

enum class Band : std::uint8_t { Low, High, Pass, Stop };

// The left-half-plane poles of the normalised analogue low-pass prototype of even order n, one of each
// conjugate pair (k = 1 .. n/2).
std::vector<Complex> prototypePoles(int n, double ripplePercent) {
    std::vector<Complex> poles;
    if (ripplePercent <= 0.0) {
        for (int k = 1; k <= n / 2; ++k) {
            const double theta = kPi * static_cast<double>(2 * k + n - 1) / static_cast<double>(2 * n);
            poles.emplace_back(std::cos(theta), std::sin(theta));
        }
        return poles;
    }
    // After the unit-gain normalisation the pass band ripples between 1 and 1 + r/100 (even orders).
    const double peak = 1.0 + ripplePercent / 100.0;
    const double eps = std::sqrt(peak * peak - 1.0);
    const double mu = std::asinh(1.0 / eps) / static_cast<double>(n);
    for (int k = 1; k <= n / 2; ++k) {
        const double theta = kPi * static_cast<double>(2 * k - 1) / static_cast<double>(2 * n);
        poles.emplace_back(-std::sinh(mu) * std::sin(theta), std::cosh(mu) * std::cos(theta));
    }
    return poles;
}

Complex bilinear(Complex s) {
    return (1.0 + s) / (1.0 - s);
}

Section fromPoles(Complex z, double nb0, double nb1, double nb2) {
    Section s;
    s.b0 = nb0;
    s.b1 = nb1;
    s.b2 = nb2;
    s.a1 = -2.0 * z.real();
    s.a2 = std::norm(z);
    return s;
}

// Sections of one pass. `wLo`/`wHi` are the pre-warped edges tan(pi f / rate) (low/high pass: wLo only).
std::vector<Section> chebySections(Band band, int order, double ripplePercent, double wLo, double wHi) {
    std::vector<Section> out;
    const std::vector<Complex> proto = prototypePoles(order, ripplePercent);
    const double w0 = std::sqrt(wLo * wHi);
    const double bw = wHi - wLo;
    const double omega0 = 2.0 * std::atan(w0); // digital centre (rad/sample)
    for (const Complex& p : proto) {
        switch (band) {
        case Band::Low:
            out.push_back(fromPoles(bilinear(wLo * p), 1.0, 2.0, 1.0));
            break;
        case Band::High:
            out.push_back(fromPoles(bilinear(wLo / p), 1.0, -2.0, 1.0));
            break;
        case Band::Pass:
        case Band::Stop: {
            // s^2 - c s + w0^2 = 0 with c = p bw (pass) or bw / p (stop).
            const Complex c = band == Band::Pass ? p * bw : bw / p;
            const Complex root = std::sqrt(c * c - 4.0 * w0 * w0);
            for (const Complex& q : {(c + root) / 2.0, (c - root) / 2.0}) {
                if (band == Band::Pass) {
                    out.push_back(fromPoles(bilinear(q), 1.0, 0.0, -1.0));
                } else {
                    out.push_back(fromPoles(bilinear(q), 1.0, -2.0 * std::cos(omega0), 1.0));
                }
            }
            break;
        }
        }
    }
    // Unit gain at the reference frequency, section by section.
    const Complex at = band == Band::Low || band == Band::Stop ? Complex(1.0, 0.0)
                       : band == Band::High                    ? Complex(-1.0, 0.0)
                                                               : std::polar(1.0, omega0);
    for (Section& s : out) {
        const double g = std::abs(s.response(at));
        if (g > 0.0 && std::isfinite(g)) {
            s.scale(1.0 / g);
        }
    }
    return out;
}

std::string hz(double v) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << v;
    return os.str();
}

class IirKernel final : public Kernel, public Streamable {
public:
    IirKernel(IntrinsicId id, double fLo, double fHi, int order, double ripple)
        : id_(id), fLo_(fLo), fHi_(fHi), order_(order), ripple_(ripple) {}

    KernelClass cls() const override { return KernelClass::IIR; }
    ExtentMode extent() const override { return ExtentMode::Whole; }

    void reset(const EvalContext& ctx) override { design(ctx.resultRate); }
    Result<void> status() const override { return status_; }

    // Forward passes stream (D-191): they run interleaved sample by sample with their state carried from
    // step to step. A backward pass needs the whole extent first.
    bool canStream() const noexcept override { return status_.ok() && forwardOnly(); }
    void begin(const BlockCtx& extent) override {
        if (!designed_ || !(designedRate_ == extent.rate)) {
            design(extent.rate);
        }
        gapSamples_ = kGapSeconds * extent.rate.hz();
        state_.clear();
        flat_.clear();
        for (const Pass& p : passes_) {
            state_.push_back(freshState(p));
            flat_.insert(flat_.end(), p.sections.begin(), p.sections.end());
        }
    }
    void step(const BlockCtx& /*extent*/, std::span<const StreamInput> in, double* out, std::uint64_t from,
              std::uint64_t to) override {
        const auto n = static_cast<std::size_t>(to - from);
        if (in.empty() || !status_ || !forwardOnly()) {
            std::fill(out, out + n, kNaN);
            return;
        }
        filterSamples(flat_.data(), passes_.size(), passes_.front().sections.size(), state_.data(),
                      gapSamples_, in[0].at(from), out, n, 1);
    }

    void run(BlockCtx& ctx, std::span<const BlockView> in, BlockView out) override {
        if (in.empty() || out.n == 0) {
            return;
        }
        if (!designed_ || !(designedRate_ == ctx.rate)) {
            design(ctx.rate);
        }
        if (canStream()) {
            runStreamed(*this, ctx, in, out);
            return;
        }
        // The passes run in place, one after the other: in the output buffer itself when it is F64 (the 0.1
        // evaluator's), else in a copy that is stored at the end.
        std::vector<double> copy;
        std::span<double> x;
        if (out.dtype == store::Dtype::F64) {
            x = std::span<double>(static_cast<double*>(out.data), out.n);
        } else {
            copy.resize(out.n);
            x = copy;
        }
        const BlockView& v = in[0];
        const bool direct = status_ && !passes_.empty() && v.dtype == store::Dtype::F64 &&
                            v.validity == nullptr && v.data != nullptr && v.n >= out.n;
        if (!direct) {
            const Reader input(v);
            for (std::uint32_t i = 0; i < out.n; ++i) {
                x[i] = status_ ? input(i) : kNaN;
            }
        }
        if (status_) {
            // The first pass reads an F64 input without a bitmap (the evaluator's) in place of a copy of it.
            const double* from = direct ? static_cast<const double*>(v.data) : nullptr;
            for (const Pass& p : passes_) {
                runPass(x, p, ctx.rate.hz(), from);
                from = nullptr;
                if (ctx.cancel.cancelled()) {
                    return;
                }
            }
        }
        if (copy.empty()) {
            const Writer bits(out); // the values are in place; the writer fills the bitmap from the NaN marks
        } else {
            storeAll(out, copy);
        }
    }

private:
    bool rc() const noexcept { return id_ == IntrinsicId::FilterLp || id_ == IntrinsicId::FilterHp; }
    // Every pass forward with the same number of sections (RC orders 1-3, Chebyshev orders 2-6).
    bool forwardOnly() const noexcept {
        if (passes_.empty() || passes_.front().sections.empty()) {
            return false;
        }
        for (const Pass& p : passes_) {
            if (p.backward || p.sections.size() != passes_.front().sections.size()) {
                return false;
            }
        }
        return true;
    }
    bool band() const noexcept {
        return id_ == IntrinsicId::FilterChebyBp || id_ == IntrinsicId::FilterChebyBs;
    }

    void design(Rate rate) {
        designed_ = true;
        designedRate_ = rate;
        passes_.clear();
        status_ = Result<void>{};
        const double fs = rate.hz();
        const std::string name(intrinsicName(id_));
        const double top = band() ? fHi_ : fLo_;
        if (!(fs > 0.0) || !(top < fs / 2.0)) {
            status_ = kernelError("E-ARG-VALUE", name + "(): " + hz(top) +
                                                     " Hz is not below half the result rate (" +
                                                     hz(fs / 2.0) + " Hz)");
            return;
        }
        if (rc()) {
            const Section s = rcSection(id_ == IntrinsicId::FilterHp, fLo_, fs);
            const int passes = order_ < 0 ? 1 : order_;
            for (int k = 0; k < passes; ++k) {
                passes_.push_back(Pass{{s}, order_ == -1});
            }
            if (order_ == -2) {
                passes_.push_back(Pass{{s}, true});
            }
            return;
        }
        const Band b = id_ == IntrinsicId::FilterChebyLp   ? Band::Low
                       : id_ == IntrinsicId::FilterChebyHp ? Band::High
                       : id_ == IntrinsicId::FilterChebyBp ? Band::Pass
                                                           : Band::Stop;
        const double wLo = std::tan(kPi * fLo_ / fs);
        const double wHi = band() ? std::tan(kPi * fHi_ / fs) : wLo;
        const bool zeroPhase = order_ < 0;
        const int each = zeroPhase ? -order_ / 2 : order_;
        const std::vector<Section> sections = chebySections(b, each, ripple_, wLo, wHi);
        passes_.push_back(Pass{sections, false});
        if (zeroPhase) {
            passes_.push_back(Pass{sections, true});
        }
    }

    IntrinsicId id_;
    double fLo_ = 0.0;
    double fHi_ = 0.0;
    int order_ = 1;
    double ripple_ = 0.0;
    bool designed_ = false;
    Rate designedRate_{};
    std::vector<Pass> passes_;
    Result<void> status_;
    // Streaming: the passes' sections pass by pass, their state and the re-prime gap in samples.
    std::vector<Section> flat_;
    std::vector<PassState> state_;
    double gapSamples_ = 0.0;
};

} // namespace

KernelResult makeIir(IntrinsicId id, const Ast& ast, std::uint32_t call) {
    const std::string name(intrinsicName(id));
    const bool rcFilter = id == IntrinsicId::FilterLp || id == IntrinsicId::FilterHp;
    const bool bandFilter = id == IntrinsicId::FilterChebyBp || id == IntrinsicId::FilterChebyBs;
    const auto number = [&](std::size_t k, double fallback) {
        const Node* n = argNode(ast, call, k);
        return isNumberLiteral(n) ? n->number : fallback;
    };
    const double fLo = number(1, kNaN);
    const double fHi = bandFilter ? number(2, kNaN) : fLo;
    const std::size_t orderArg = bandFilter ? 3 : 2;
    const double order = number(orderArg, rcFilter ? 1.0 : 2.0);
    const double ripple = number(orderArg + 1, 0.0);
    if (!(fLo > 0.0) || !(fHi > 0.0)) {
        return kernelError("E-ARG-VALUE", name + "(): the frequency must be a constant above 0 Hz");
    }
    if (bandFilter && !(fLo < fHi)) {
        return kernelError("E-ARG-VALUE",
                           name + "(): f_lo (" + hz(fLo) + " Hz) must be below f_hi (" + hz(fHi) + " Hz)");
    }
    const int o = order >= -12.0 && order <= 12.0 ? static_cast<int>(order) : 0; // 0: never a valid order
    const bool orderOk = static_cast<double>(o) == order &&
                         (rcFilter ? (o >= -2 && o <= 3 && o != 0)
                                   : (o == 2 || o == 4 || o == 6 || o == -4 || o == -8 || o == -12));
    if (!orderOk) {
        return kernelError("E-ARG-VALUE", name + "(): order " + hz(order) + " is not supported");
    }
    if (!(ripple >= 0.0) || !std::isfinite(ripple)) {
        return kernelError("E-ARG-VALUE", name + "(): the ripple must be a percentage of at least 0");
    }
    return std::unique_ptr<Kernel>(std::make_unique<IirKernel>(id, fLo, fHi, o, ripple));
}

} // namespace esses::maths::kern
