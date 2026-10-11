// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/assert.hpp>
#include <esses/store/memory_provider.hpp>

#include <cstring>
#include <utility>

namespace esses::store {

namespace {

template <class T> std::vector<std::byte> toBytes(const std::vector<T>& v) {
    std::vector<std::byte> bytes(v.size() * sizeof(T));
    if (!v.empty()) {
        std::memcpy(bytes.data(), v.data(), bytes.size());
    }
    return bytes;
}

} // namespace

std::shared_ptr<InMemoryProvider> InMemoryProvider::fromF32(const std::vector<float>& samples,
                                                            std::vector<bool> valid, std::string name) {
    return std::make_shared<InMemoryProvider>(Dtype::F32, toBytes(samples), std::move(valid),
                                              std::move(name));
}

std::shared_ptr<InMemoryProvider> InMemoryProvider::fromF64(const std::vector<double>& samples,
                                                            std::vector<bool> valid, std::string name) {
    return std::make_shared<InMemoryProvider>(Dtype::F64, toBytes(samples), std::move(valid),
                                              std::move(name));
}

std::shared_ptr<InMemoryProvider> InMemoryProvider::fromI32(const std::vector<std::int32_t>& samples,
                                                            std::vector<bool> valid, std::string name) {
    return std::make_shared<InMemoryProvider>(Dtype::I32, toBytes(samples), std::move(valid),
                                              std::move(name));
}

std::shared_ptr<InMemoryProvider> InMemoryProvider::fromI64(const std::vector<std::int64_t>& samples,
                                                            std::string name) {
    return std::make_shared<InMemoryProvider>(Dtype::I64, toBytes(samples), std::vector<bool>{},
                                              std::move(name));
}

InMemoryProvider::InMemoryProvider(Dtype dtype, std::vector<std::byte> samples, std::vector<bool> valid,
                                   std::string name)
    : dtype_(dtype), size_(static_cast<SampleIdx>(samples.size() / dtypeSize(dtype))),
      samples_(std::move(samples)), valid_(std::move(valid)), name_(std::move(name)) {
    ESSES_ASSERT_MSG(valid_.empty() || valid_.size() == static_cast<std::size_t>(size_),
                     "InMemoryProvider: one validity flag per sample");
    if (dtype_ == Dtype::I64 || valid_.size() != static_cast<std::size_t>(size_)) {
        valid_.clear(); // time columns are always valid
    }
}

Result<void> InMemoryProvider::materialise(SampleIdx chunkNo, ChunkBuilder& out, CancelToken cancel) {
    const std::uint32_t count = chunkSampleCount(size_, chunkNo);
    if (count == 0) {
        return fail(ErrorCode::Invalid, "chunk number out of range");
    }
    if (cancel.cancelled()) {
        return fail(ErrorCode::Cancelled, "cancelled");
    }
    const auto start = static_cast<std::size_t>(chunkFirstSample(chunkNo));
    bool bitmap = false;
    if (!valid_.empty()) {
        for (std::size_t i = start; i < start + count && !bitmap; ++i) {
            bitmap = !valid_[i];
        }
    }
    if (auto r = out.allocate(count, dtype_, bitmap); !r) {
        return r;
    }
    const std::size_t width = dtypeSize(dtype_);
    std::memcpy(out.sampleBytes().data(), samples_.data() + start * width,
                static_cast<std::size_t>(count) * width);
    if (bitmap) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!valid_[start + i]) {
                out.setValid(i, false);
            }
        }
    }
    return ok();
}

} // namespace esses::store
