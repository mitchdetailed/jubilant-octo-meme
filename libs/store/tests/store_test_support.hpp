// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers for the esses_store tests: a configurable chunk provider, random column generators with NaN
// sprinkles, invalid runs and all-invalid chunks, and brute-force min/max references.
#pragma once

#include <esses/base/assert.hpp>
#include <esses/base/env.hpp>
#include <esses/store/store.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace esses::store::test {

// Property-test seed (08 section 6.2): ESSES_TEST_SEED when set, else a fixed default.
inline std::uint64_t testSeed() {
    if (const auto v = getEnv(envName("TEST_SEED")); v && !v->empty()) {
        return std::stoull(*v, nullptr, 0);
    }
    return 0x5EED'0000'2026'1010ull;
}

// Polls `condition` every millisecond for up to `timeout`.
inline bool waitFor(const std::function<bool()>& condition,
                    std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Records assertion failures instead of aborting, for the duration of the scope.
class RecordingAssertHandler {
public:
    RecordingAssertHandler() : previous_(setAssertHandler(&record)) { count().store(0); }
    ~RecordingAssertHandler() { setAssertHandler(previous_); }
    RecordingAssertHandler(const RecordingAssertHandler&) = delete;
    RecordingAssertHandler& operator=(const RecordingAssertHandler&) = delete;
    static int failures() { return count().load(); }

private:
    static std::atomic<int>& count() {
        static std::atomic<int> c{0};
        return c;
    }
    static void record(const char*, const char*, int, const char*) { ++count(); }
    AssertHandler previous_;
};

// Raw column data plus validity: valid[i] == 0 marks a bitmap-invalid sample; chunks listed in allInvalid are
// produced with allocateAllInvalid.
template <class T> struct ColumnData {
    std::vector<T> v;
    std::vector<std::uint8_t> valid; // empty = all valid
    std::set<SampleIdx> allInvalid;

    SampleIdx size() const { return static_cast<SampleIdx>(v.size()); }
    // The effective validity the store must report.
    bool effectiveValid(SampleIdx i) const {
        if (allInvalid.count(chunkOf(i)) != 0) {
            return false;
        }
        if (!valid.empty() && valid[static_cast<std::size_t>(i)] == 0) {
            return false;
        }
        if constexpr (std::is_floating_point_v<T>) {
            return !std::isnan(v[static_cast<std::size_t>(i)]);
        } else {
            return true;
        }
    }
    std::vector<std::uint8_t> effectiveMask() const {
        std::vector<std::uint8_t> m(v.size(), 1);
        std::uint8_t* out = m.data();
        const T* s = v.data();
        const std::uint8_t* ok = valid.empty() ? nullptr : valid.data();
        for (std::size_t i = 0; i < v.size(); ++i) {
            bool good = ok == nullptr || ok[i] != 0;
            if constexpr (std::is_floating_point_v<T>) {
                good = good && !std::isnan(s[i]);
            }
            out[i] = good ? 1 : 0;
        }
        for (const SampleIdx c : allInvalid) {
            const auto b = static_cast<std::size_t>(chunkFirstSample(c));
            std::fill(out + b, out + std::min(v.size(), b + kChunkSamples), std::uint8_t{0});
        }
        return m;
    }
};

// A provider over ColumnData with call counting, an optional delay, an optional hook (nested requires,
// failure injection), a settable version and an optional sequential mode.
template <class T> class TestProvider final : public IChunkProvider {
public:
    explicit TestProvider(ColumnData<T> data, bool independent = true)
        : data_(std::move(data)), independent_(independent),
          calls_(static_cast<std::size_t>(chunkCountFor(data_.size()))) {}

    SampleIdx size() const override { return data_.size(); }
    Dtype dtype() const override { return kDtypeOf<T>; }
    std::uint64_t version() const override { return version_.load(std::memory_order_acquire); }
    bool chunkIndependent() const override { return independent_; }
    std::string debugName() const override { return "test:" + std::string(dtypeName(kDtypeOf<T>)); }

    Result<void> materialise(SampleIdx n, ChunkBuilder& out, CancelToken cancel) override {
        const std::uint64_t ver = version(); // one definition per chunk, like a real provider
        calls_[static_cast<std::size_t>(n)].fetch_add(1);
        total_.fetch_add(1);
        {
            std::lock_guard lock(orderMutex_);
            order_.push_back(n);
        }
        if (delayMs_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
        }
        if (hook_) {
            if (Result<void> r = hook_(n, cancel); !r) {
                return r;
            }
        }
        if (cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        const std::uint32_t count = chunkSampleCount(data_.size(), n);
        if (data_.allInvalid.count(n) != 0) {
            return out.allocateAllInvalid(count, dtype());
        }
        const auto start = static_cast<std::size_t>(chunkFirstSample(n));
        bool bitmap = false;
        if (!data_.valid.empty()) {
            for (std::size_t i = start; i < start + count && !bitmap; ++i) {
                bitmap = data_.valid[i] == 0;
            }
        }
        if (Result<void> r = out.allocate(count, dtype(), bitmap); !r) {
            return r;
        }
        std::span<T> s = out.template samples<T>();
        for (std::uint32_t i = 0; i < count; ++i) {
            s[i] = valueFn_ ? valueFn_(ver, static_cast<SampleIdx>(start + i)) : data_.v[start + i];
        }
        if (bitmap) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (data_.valid[start + i] == 0) {
                    out.setValid(i, false);
                }
            }
        }
        return ok();
    }

    const ColumnData<T>& data() const { return data_; }
    int calls(SampleIdx n) const { return calls_[static_cast<std::size_t>(n)].load(); }
    int totalCalls() const { return total_.load(); }
    std::vector<SampleIdx> order() const {
        std::lock_guard lock(orderMutex_);
        return order_;
    }
    void bumpVersion() { version_.fetch_add(1, std::memory_order_acq_rel); }
    void setDelayMs(int ms) { delayMs_ = ms; }
    void setHook(std::function<Result<void>(SampleIdx, CancelToken)> hook) { hook_ = std::move(hook); }
    // Sample values computed from (version, index) instead of the stored data.
    void setValueFn(std::function<T(std::uint64_t, SampleIdx)> fn) { valueFn_ = std::move(fn); }

private:
    ColumnData<T> data_;
    bool independent_;
    std::vector<std::atomic<int>> calls_;
    std::atomic<int> total_{0};
    std::atomic<std::uint64_t> version_{1};
    int delayMs_ = 0;
    std::function<Result<void>(SampleIdx, CancelToken)> hook_;
    std::function<T(std::uint64_t, SampleIdx)> valueFn_;
    mutable std::mutex orderMutex_;
    std::vector<SampleIdx> order_;
};

// Brute-force min/max over the effective-valid samples of [begin, end): exact for F32, the exact result
// rounded directionally to float for the wider dtypes (the documented pyramid contract). Written with plain
// comparisons (no shared fold helpers) plus the documented zero-sign rule: -0 is the minimum and +0 the
// maximum of a set holding both.
template <class T>
MinMax bruteMinMax(const std::vector<T>& v, const std::vector<std::uint8_t>& mask, SampleIdx begin,
                   SampleIdx end) {
    begin = std::max<SampleIdx>(begin, 0);
    end = std::min<SampleIdx>(end, static_cast<SampleIdx>(v.size()));
    const T* s = v.data();
    const std::uint8_t* m = mask.data();
    bool any = false;
    bool negZero = false;
    bool posZero = false;
    T lo{};
    T hi{};
    for (SampleIdx i = begin; i < end; ++i) {
        if (m[i] == 0) {
            continue;
        }
        const T x = s[i];
        if (!any) {
            lo = x;
            hi = x;
            any = true;
        } else {
            if (x < lo) {
                lo = x;
            }
            if (x > hi) {
                hi = x;
            }
        }
        if constexpr (std::is_floating_point_v<T>) {
            if (x == T(0)) {
                (std::signbit(x) ? negZero : posZero) = true;
            }
        }
    }
    if (!any) {
        return MinMax::empty();
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (lo == T(0) && negZero) {
            lo = -T(0);
        }
        if (hi == T(0) && posZero) {
            hi = T(0);
        }
    }
    if constexpr (std::is_same_v<T, float>) {
        return MinMax{lo, hi};
    } else {
        return encloseDirectional(static_cast<double>(lo), static_cast<double>(hi));
    }
}

// Exact double min/max (for enclosure checks); nullopt when no sample is valid.
template <class T>
std::optional<std::pair<double, double>>
exactMinMax(const std::vector<T>& v, const std::vector<std::uint8_t>& mask, SampleIdx begin, SampleIdx end) {
    begin = std::max<SampleIdx>(begin, 0);
    end = std::min<SampleIdx>(end, static_cast<SampleIdx>(v.size()));
    const T* s = v.data();
    const std::uint8_t* m = mask.data();
    bool any = false;
    double lo = 0.0;
    double hi = 0.0;
    for (SampleIdx i = begin; i < end; ++i) {
        if (m[i] == 0) {
            continue;
        }
        const auto x = static_cast<double>(s[i]);
        if (!any) {
            lo = x;
            hi = x;
            any = true;
        } else {
            if (x < lo) {
                lo = x;
            }
            if (x > hi) {
                hi = x;
            }
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return std::make_pair(lo, hi);
}

// Random column: a random walk with jumps, NaN sprinkles (floats), bitmap-invalid runs, special values, and
// optionally an all-invalid chunk and an all-NaN chunk. `nanRate` and `invalidRunRate` vary per seed.
template <class T>
ColumnData<T> randomColumn(std::mt19937_64& rng, SampleIdx size, double nanRate, double invalidRunRate,
                           bool withAllInvalidChunk, bool withAllNaNChunk) {
    ColumnData<T> d;
    d.v.resize(static_cast<std::size_t>(size));
    d.valid.assign(static_cast<std::size_t>(size), 1);
    // Two 64-bit draws per sample, split into fields (cheap in Debug builds, where the <random> distributions
    // dominate the property tests' run time).
    constexpr double kUnit = 1.0 / 9'007'199'254'740'992.0; // 2^-53
    auto u01 = [&rng] { return static_cast<double>(rng() >> 11) * kUnit; };
    double walk = 0.0;
    for (SampleIdx i = 0; i < size; ++i) {
        const std::uint64_t a = rng();
        const std::uint64_t b = rng();
        walk += (static_cast<double>(a & 0xFFFF) - 32'767.5) / 16'384.0; // steps in (-2, 2)
        if (((a >> 16) & 0x3FF) == 0) {
            walk += (static_cast<double>((a >> 26) & 0xFFFF) - 32'767.5) / 32.0; // rare jumps of up to +-1000
        }
        double x = walk;
        const double r = static_cast<double>(b >> 11) * kUnit;              // special values
        const double rNan = static_cast<double>(a >> 44) / 1'048'576.0;     // 20 bits
        const double rRun = static_cast<double>(b & 0xFFFFF) / 1'048'576.0; // 20 bits
        if constexpr (std::is_same_v<T, float>) {
            if (r < 0.0005) {
                x = r < 0.00025 ? 0.0 : -0.0; // signed zeros
            } else if (r < 0.00052) {
                x = r < 0.00051 ? std::numeric_limits<double>::infinity()
                                : -std::numeric_limits<double>::infinity();
            }
            d.v[static_cast<std::size_t>(i)] = static_cast<float>(x);
        } else if constexpr (std::is_same_v<T, double>) {
            // GPS-like values whose low digits do not fit a float, plus rare values beyond the float range.
            x = -33.4479 + walk * 1e-7;
            if (r < 0.0005) {
                x = r < 0.00025 ? 0.0 : -0.0;
            } else if (r < 0.0006) {
                x = (r < 0.00055 ? 1.0 : -1.0) * 3.0e39 * (1.0 + u01());
            }
            d.v[static_cast<std::size_t>(i)] = x;
        } else {
            // Values above 2^24 so the float rounding is visible, and the int32 extremes.
            const double big = std::clamp(walk * 4.0e5, -2.0e9, 2.0e9);
            auto xi = static_cast<std::int32_t>(big);
            if (r < 0.0002) {
                xi = r < 0.0001 ? std::numeric_limits<std::int32_t>::max()
                                : std::numeric_limits<std::int32_t>::min();
            }
            d.v[static_cast<std::size_t>(i)] = xi;
        }
        if constexpr (std::is_floating_point_v<T>) {
            if (rNan < nanRate) {
                d.v[static_cast<std::size_t>(i)] = std::numeric_limits<T>::quiet_NaN();
            }
        }
        if (rRun < invalidRunRate) {
            const auto len = static_cast<SampleIdx>(1 + u01() * 300.0);
            for (SampleIdx k = i; k < std::min(size, i + len); ++k) {
                d.valid[static_cast<std::size_t>(k)] = 0;
            }
        }
    }
    const SampleIdx chunks = chunkCountFor(size);
    if (withAllInvalidChunk && chunks > 1) {
        d.allInvalid.insert(static_cast<SampleIdx>(rng() % static_cast<std::uint64_t>(chunks)));
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (withAllNaNChunk && chunks > 1) {
            const auto c = static_cast<SampleIdx>(rng() % static_cast<std::uint64_t>(chunks));
            const SampleIdx b = chunkFirstSample(c);
            for (SampleIdx i = b; i < std::min(size, b + SampleIdx{kChunkSamples}); ++i) {
                d.v[static_cast<std::size_t>(i)] = std::numeric_limits<T>::quiet_NaN();
            }
        }
    }
    return d;
}

} // namespace esses::store::test
