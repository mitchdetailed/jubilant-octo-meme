// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LdChunkProvider (05 section 3.11, D-011): an IChunkProvider over one channel of an LdFile. materialise()
// decodes samples [chunkNo * 65536, ...) from the channel's data pointer with the channel's scaling, sets the
// chunk's validity from the gap-block intervals (AllValid when no interval touches the chunk, AllInvalid when
// one covers it, else Bitmap; float chunks also get NaN at invalid samples) and leaves sealing (and the
// pyramid) to the Column. Stateless over the shared LdFile, so chunks decode in parallel.
//
// Before each decode the provider checks MappedFile::changedOnDisk(): a changed file fails the chunk with Io
// and logs ld.source_changed once in the DecodeLog. The decode loop runs through MappedFile::guarded(), so a
// page fault seals the chunk AllInvalid with ld.io_fault instead of terminating the process.
//
// An optional LdConversion (the open pipeline's conversion to the quantity's base unit, D-010) is applied in
// F64 after the channel's scaling and before the one narrowing to the output dtype, so a converted F32 sample
// is rounded once. The core stays free of esses_units: the conversion is the affine pair of units::Converter.
#pragma once

#include "ld/ld_file.hpp"

#include <esses/formats/import_log.hpp>
#include <esses/store/chunk_provider.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace esses::formats::ld {

// value_out = a * decoded + b, stored as `dtype` (F32 or F64).
struct LdConversion {
    double a = 1.0;
    double b = 0.0;
    store::Dtype dtype = store::Dtype::F32;
    friend bool operator==(const LdConversion&, const LdConversion&) = default;
};

// Decodes samples [first, first + count) of channel `ch` into `out` (count * dtypeSize(outDtype) bytes) with
// the channel's scaling and, when given, the conversion (outDtype must then be conversion->dtype). Validity
// is not applied. Errors as LdFile::decode (Io on a page fault inside a mapped file).
Result<void> decodeConverted(const LdFile& file, std::uint32_t ch, std::uint64_t first, std::uint64_t count,
                             store::Dtype outDtype, const std::optional<LdConversion>& conversion,
                             std::span<std::byte> out);

class LdChunkProvider final : public store::IChunkProvider {
public:
    // `log` may be null (no decode diagnostics recorded).
    LdChunkProvider(std::shared_ptr<const LdFile> file, std::uint32_t channel,
                    std::shared_ptr<DecodeLog> log = nullptr,
                    std::optional<LdConversion> conversion = std::nullopt);

    store::SampleIdx size() const override;
    store::Dtype dtype() const override;
    std::uint64_t version() const override { return 1; }
    bool chunkIndependent() const override { return true; }
    Result<void> materialise(store::SampleIdx chunkNo, store::ChunkBuilder& out, CancelToken cancel) override;
    std::string debugName() const override;

    const LdFile& file() const noexcept { return *file_; }
    std::uint32_t channel() const noexcept { return channel_; }
    const std::optional<LdConversion>& conversion() const noexcept { return conversion_; }

private:
    std::shared_ptr<const LdFile> file_;
    std::uint32_t channel_;
    std::shared_ptr<DecodeLog> log_;
    std::optional<LdConversion> conversion_;
    std::atomic<bool> changeLogged_{false};
};

} // namespace esses::formats::ld
