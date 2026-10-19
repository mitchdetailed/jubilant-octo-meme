// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "ld/ld_chunk_provider.hpp"

#include "ld/ld_codes.hpp"

#include <esses/base/assert.hpp>

#include <mutex>
#include <utility>
#include <vector>

namespace esses::formats::ld {

namespace {

// Makes "ld.io_fault once per source" hold when two channels of a source fault at the same time on two
// workers: their check of the shared DecodeLog and the add are one step. Faults are rare, so one lock for
// every source is enough.
std::mutex& ioFaultLogMutex() {
    static std::mutex m;
    return m;
}

} // namespace

Result<void> decodeConverted(const LdFile& file, std::uint32_t ch, std::uint64_t first, std::uint64_t count,
                             store::Dtype outDtype, const std::optional<LdConversion>& conversion,
                             std::span<std::byte> out) {
    if (!conversion) {
        return file.decode(ch, first, count, outDtype, out);
    }
    if (outDtype != conversion->dtype || !store::isFloatDtype(outDtype)) {
        return fail(ErrorCode::Invalid, "decodeConverted: a conversion writes its own float dtype");
    }
    if (out.size() < count * store::dtypeSize(outDtype)) {
        return fail(ErrorCode::Invalid, "decodeConverted: output buffer too small");
    }
    // The channel's scaling into F64, then the affine conversion, then one narrowing.
    std::vector<double> scratch(static_cast<std::size_t>(count));
    if (auto r = file.decode(ch, first, count, store::Dtype::F64, std::as_writable_bytes(std::span(scratch)));
        !r) {
        return r;
    }
    const double a = conversion->a;
    const double b = conversion->b;
    if (outDtype == store::Dtype::F64) {
        auto* dst = reinterpret_cast<double*>(out.data());
        for (std::size_t i = 0; i < scratch.size(); ++i) {
            dst[i] = a * scratch[i] + b;
        }
    } else {
        auto* dst = reinterpret_cast<float*>(out.data());
        for (std::size_t i = 0; i < scratch.size(); ++i) {
            dst[i] = narrowToFloat(a * scratch[i] + b);
        }
    }
    return ok();
}

LdChunkProvider::LdChunkProvider(std::shared_ptr<const LdFile> file, std::uint32_t channel,
                                 std::shared_ptr<DecodeLog> log, std::optional<LdConversion> conversion)
    : file_(std::move(file)), channel_(channel), log_(std::move(log)), conversion_(conversion) {
    ESSES_ASSERT_MSG(file_ != nullptr && channel_ < file_->channels().size(),
                     "LdChunkProvider: no such channel");
    ESSES_ASSERT_MSG(!conversion_ || store::isFloatDtype(conversion_->dtype),
                     "LdChunkProvider: a conversion writes F32 or F64");
}

store::SampleIdx LdChunkProvider::size() const {
    if (!file_ || channel_ >= file_->channels().size()) {
        return 0;
    }
    return static_cast<store::SampleIdx>(file_->channels()[channel_].sampleCount);
}

store::Dtype LdChunkProvider::dtype() const {
    if (conversion_) {
        return conversion_->dtype;
    }
    if (!file_ || channel_ >= file_->channels().size()) {
        return store::Dtype::F32;
    }
    return file_->channels()[channel_].dtype;
}

std::string LdChunkProvider::debugName() const {
    if (!file_ || channel_ >= file_->channels().size()) {
        return "ld:?";
    }
    return "ld:" + file_->channels()[channel_].loggedName;
}

Result<void> LdChunkProvider::materialise(store::SampleIdx chunkNo, store::ChunkBuilder& out,
                                          CancelToken cancel) {
    if (!file_ || channel_ >= file_->channels().size()) {
        return fail(ErrorCode::NotFound, "LdChunkProvider: no such channel");
    }
    const LdChannel& c = file_->channels()[channel_];
    const std::uint32_t count = store::chunkSampleCount(size(), chunkNo);
    if (count == 0) {
        return fail(ErrorCode::Invalid,
                    "LdChunkProvider: chunk " + std::to_string(chunkNo) + " is outside " + debugName());
    }
    if (cancel.cancelled()) {
        return fail(ErrorCode::Cancelled, "LdChunkProvider: cancelled");
    }
    if (const MappedFile* mf = file_->mapped(); mf != nullptr && mf->changedOnDisk()) {
        // Once for the source (its channels share the DecodeLog), as ld.io_fault (D-217).
        if (log_ && !changeLogged_.exchange(true)) {
            const std::scoped_lock once(ioFaultLogMutex());
            if (!log_->contains(code::kSourceChanged)) {
                ImportLogEntry::Context ctx;
                ctx.channel = c.loggedName;
                log_->add(ImportLogEntry::Level::Warning, std::string(code::kSourceChanged),
                          "the .ld file changed on disk after it was opened; reload it", std::move(ctx));
            }
        }
        ErrorContext ectx;
        ectx.path = mf->path();
        ectx.channel = c.loggedName;
        return fail(ErrorCode::Io, "the .ld file changed on disk after it was opened", std::move(ectx));
    }

    const auto first = static_cast<std::uint64_t>(store::chunkFirstSample(chunkNo));
    const store::Dtype outDtype = dtype();
    const LdFile::RangeValidity validity = file_->validity(channel_, first, count);
    if (validity == LdFile::RangeValidity::AllInvalid) {
        return out.allocateAllInvalid(count, outDtype);
    }
    const bool bitmap = validity == LdFile::RangeValidity::Mixed;
    if (auto r = out.allocate(count, outDtype, bitmap); !r) {
        return r;
    }
    const std::span<std::byte> samples = out.sampleBytes();
    if (auto r = decoder_(*file_, channel_, first, count, outDtype, conversion_, samples); !r) {
        if (r.error().code != ErrorCode::Io) {
            return r;
        }
        // Page fault inside the mapping (05 section 3.11): seal this chunk AllInvalid, keep the process, and
        // log it once for the source (its channels share the DecodeLog; 02 section 1 behaviour 5).
        if (log_) {
            const std::scoped_lock once(ioFaultLogMutex());
            if (!log_->contains(code::kIoFault)) {
                ImportLogEntry::Context ctx;
                ctx.channel = c.loggedName;
                ctx.offset = c.enc.dataOffset;
                log_->add(ImportLogEntry::Level::Warning, std::string(code::kIoFault),
                          "page fault while decoding chunk " + std::to_string(chunkNo) +
                              "; chunk marked invalid",
                          std::move(ctx));
            }
        }
        out.markAllInvalid();
        return ok();
    }
    if (bitmap) {
        file_->clearInvalidBits(channel_, first, count, out.bitmapWords());
        file_->writeInvalidNaN(channel_, first, count, outDtype, samples);
    }
    return ok();
}

} // namespace esses::formats::ld
