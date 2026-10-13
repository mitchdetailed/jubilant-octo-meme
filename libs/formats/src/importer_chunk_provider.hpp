// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ImporterChunkProvider (05 section 2 step 3): the generic store::IChunkProvider over IReader::readBlock, for
// readers that do not supply their own provider. A reader error marks the chunk AllInvalid and is logged once
// per channel in the source's DecodeLog (05 section 2 "Failure modes"); Cancelled is passed through.
#pragma once

#include <esses/formats/import_log.hpp>
#include <esses/formats/importer.hpp>
#include <esses/store/chunk_provider.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace esses::formats {

class ImporterChunkProvider final : public store::IChunkProvider {
public:
    // `readerMutex`: shared by every provider of one reader whose chunkIndependent() is false, so its
    // readBlock calls are serialised across channels (05 section 2, IReader: "false: serialise"); null makes
    // one per provider for such a reader.
    ImporterChunkProvider(std::shared_ptr<IReader> reader, LayerIx layer, ChannelIx channel,
                          store::Dtype dtype, std::shared_ptr<DecodeLog> log,
                          std::shared_ptr<std::mutex> readerMutex = {});

    store::SampleIdx size() const override { return size_; }
    store::Dtype dtype() const override { return dtype_; }
    std::uint64_t version() const override { return 1; }
    bool chunkIndependent() const override { return reader_->chunkIndependent(); }
    Result<void> materialise(store::SampleIdx chunkNo, store::ChunkBuilder& out, CancelToken cancel) override;
    std::string debugName() const override { return name_; }

private:
    std::shared_ptr<IReader> reader_;
    LayerIx layer_;
    ChannelIx channel_;
    store::Dtype dtype_;
    store::SampleIdx size_ = 0;
    std::string name_;
    std::shared_ptr<DecodeLog> log_;
    std::shared_ptr<std::mutex> readerMutex_; // null for a chunk-independent reader
    std::atomic<bool> errorLogged_{false};
};

// The I64 time column of a timestamped channel over IReader::readTimestamps (M2), for readers that do not
// supply their own (IReader::makeTimesProvider). Always AllValid (02 section 3.1: I64 has no bitmap); a read
// error is logged once and fails the chunk.
class TimestampChunkProvider final : public store::IChunkProvider {
public:
    TimestampChunkProvider(std::shared_ptr<IReader> reader, LayerIx layer, ChannelIx channel,
                           std::shared_ptr<DecodeLog> log, std::shared_ptr<std::mutex> readerMutex = {});

    store::SampleIdx size() const override { return size_; }
    store::Dtype dtype() const override { return store::Dtype::I64; }
    std::uint64_t version() const override { return 1; }
    bool chunkIndependent() const override { return reader_->chunkIndependent(); }
    Result<void> materialise(store::SampleIdx chunkNo, store::ChunkBuilder& out, CancelToken cancel) override;
    std::string debugName() const override { return name_; }

private:
    std::shared_ptr<IReader> reader_;
    LayerIx layer_;
    ChannelIx channel_;
    store::SampleIdx size_ = 0;
    std::string name_;
    std::shared_ptr<DecodeLog> log_;
    std::shared_ptr<std::mutex> readerMutex_;
    std::atomic<bool> errorLogged_{false};
};

} // namespace esses::formats
