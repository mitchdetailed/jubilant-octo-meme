// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// InMemoryProvider: an IChunkProvider over samples already in memory (small derived columns, the time column
// of a timestamped test series, tests). Chunk-independent, version 1 unless bumped.
#pragma once

#include <esses/store/chunk_provider.hpp>
#include <esses/store/types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esses::store {

class InMemoryProvider final : public IChunkProvider {
public:
    // `valid` (optional): one flag per sample; an empty vector means all valid. Float NaNs are invalid
    // anyway.
    static std::shared_ptr<InMemoryProvider>
    fromF32(const std::vector<float>& samples, std::vector<bool> valid = {}, std::string name = "memory:f32");
    static std::shared_ptr<InMemoryProvider> fromF64(const std::vector<double>& samples,
                                                     std::vector<bool> valid = {},
                                                     std::string name = "memory:f64");
    static std::shared_ptr<InMemoryProvider> fromI32(const std::vector<std::int32_t>& samples,
                                                     std::vector<bool> valid = {},
                                                     std::string name = "memory:i32");
    // I64 time column: always valid.
    static std::shared_ptr<InMemoryProvider> fromI64(const std::vector<std::int64_t>& samples,
                                                     std::string name = "memory:i64");

    InMemoryProvider(Dtype dtype, std::vector<std::byte> samples, std::vector<bool> valid, std::string name);

    SampleIdx size() const override { return size_; }
    Dtype dtype() const override { return dtype_; }
    std::uint64_t version() const override { return version_.load(std::memory_order_acquire); }
    bool chunkIndependent() const override { return true; }
    Result<void> materialise(SampleIdx chunkNo, ChunkBuilder& out, CancelToken cancel) override;
    std::string debugName() const override { return name_; }

    // Invalidates every cached chunk of the columns over this provider (the samples do not change).
    void bumpVersion() noexcept { version_.fetch_add(1, std::memory_order_acq_rel); }

private:
    Dtype dtype_;
    SampleIdx size_;
    std::vector<std::byte> samples_;
    std::vector<bool> valid_;
    std::string name_;
    std::atomic<std::uint64_t> version_{1};
};

} // namespace esses::store
