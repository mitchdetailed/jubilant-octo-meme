// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "importer_chunk_provider.hpp"

#include <esses/formats/registry.hpp>

#include <utility>

namespace esses::formats {

ImporterChunkProvider::ImporterChunkProvider(std::shared_ptr<IReader> reader, LayerIx layer,
                                             ChannelIx channel, store::Dtype dtype,
                                             std::shared_ptr<DecodeLog> log,
                                             std::shared_ptr<std::mutex> readerMutex)
    : reader_(std::move(reader)), layer_(layer), channel_(channel), dtype_(dtype), log_(std::move(log)),
      readerMutex_(std::move(readerMutex)) {
    if (reader_->chunkIndependent()) {
        readerMutex_.reset();
    } else if (!readerMutex_) {
        readerMutex_ = std::make_shared<std::mutex>();
    }
    const ProbeResult& p = reader_->probe();
    if (layer_ < p.layers.size() && channel_ < p.layers[layer_].channels.size()) {
        const ChannelInfo& c = p.layers[layer_].channels[channel_];
        size_ = static_cast<store::SampleIdx>(c.sampleCount);
        name_ = p.importerId + ":" + c.loggedName;
    } else {
        name_ = p.importerId + ":?";
    }
}

Result<void> ImporterChunkProvider::materialise(store::SampleIdx chunkNo, store::ChunkBuilder& out,
                                                CancelToken cancel) {
    const std::uint32_t count = store::chunkSampleCount(size_, chunkNo);
    if (count == 0) {
        return fail(ErrorCode::Invalid,
                    "ImporterChunkProvider: chunk " + std::to_string(chunkNo) + " is outside " + name_);
    }
    if (cancel.cancelled()) {
        return fail(ErrorCode::Cancelled, "ImporterChunkProvider: cancelled");
    }
    if (auto r = out.allocate(count, dtype_, true); !r) {
        return r;
    }
    Block block;
    block.dtype = dtype_;
    block.first = static_cast<std::uint64_t>(store::chunkFirstSample(chunkNo));
    block.count = count;
    block.values = out.sampleBytes();
    block.validBits = out.validBits();
    std::unique_lock<std::mutex> serial;
    if (readerMutex_) {
        serial = std::unique_lock<std::mutex>(*readerMutex_); // a sequential reader: one readBlock at a time
    }
    Result<void> read = reader_->readBlock(layer_, channel_, block.first, count, block, cancel);
    if (serial.owns_lock()) {
        serial.unlock();
    }
    if (!read) {
        if (read.error().code == ErrorCode::Cancelled) {
            return read;
        }
        if (log_ && !errorLogged_.exchange(true)) {
            ImportLogEntry::Context ctx;
            ctx.channel = name_;
            ctx.detail = read.error().toString();
            log_->add(ImportLogEntry::Level::Warning, std::string(open_code::kReadError),
                      "reading chunk " + std::to_string(chunkNo) + " failed; the chunk is marked invalid",
                      std::move(ctx));
        }
        out.markAllInvalid();
    }
    return ok();
}

} // namespace esses::formats
