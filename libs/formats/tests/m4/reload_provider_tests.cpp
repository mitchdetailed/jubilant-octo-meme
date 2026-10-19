// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The .ld chunk provider under a changing file (M4 reload package, AC-M4-10; 02 section 1 behaviour 5,
// section 8 AC-4a): a page fault inside MappedFile::guarded() seals the chunk AllInvalid and is logged once
// per source (ld.io_fault), also when its channels fault together on several workers; a file replaced by
// rename-over makes changedOnDisk() flip, so the provider decodes no further chunk from it (Io,
// ld.source_changed) while the chunks it already holds stay valid. On Windows the fault is injected inside
// guarded() (an in-page error raised from the decode, through LdChunkProvider::setDecoder): the sharing
// policy refuses the truncation that would cause a real one. On POSIX it is real: the mapped file is
// truncated after a file of the same size and time took its name, so the provider's re-stat passes and the
// decode meets the missing pages.

#include "ld/ld_chunk_provider.hpp"
#include "ld/ld_codes.hpp"
#include "ld_test_support.hpp"
#include "m4/reload_test_support.hpp"

#include <esses/formats/ld/ld_layout.hpp>
#include <esses/store/store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::ld;
using namespace esses::formats::ld::test;
namespace L = esses::formats::ld::layout;
namespace fs = std::filesystem;
using store::kChunkSamples;
using store::Validity;

namespace {

constexpr std::int64_t kSamples = 2 * kChunkSamples + 500; // three chunks

LdWriteInput faultInput() {
    LdWriteInput in;
    in.channels.push_back(intChannel("Counter", L::kTypeInt5, 4, 1000, ramp(kSamples, 7, 3)));
    in.channels.push_back(intChannel("Gear", L::kTypeInt5, 4, 1000, ramp(kSamples, 0, 0)));
    return in;
}

// Materialises and seals one chunk; REQUIREs both steps.
store::OwnedChunk sealedChunk(LdChunkProvider& p, store::SampleIdx chunkNo) {
    store::ChunkBuilder builder;
    REQUIRE(p.materialise(chunkNo, builder, {}).ok());
    auto sealed = builder.seal(p.version());
    REQUIRE(sealed.ok());
    return std::move(*sealed);
}

#if defined(ESSES_OS_WINDOWS)
struct InPageJob {
    const std::byte* address;
};
// What the memory manager raises when a mapped page cannot be read (a dropped share, a failing disk).
void raiseInPageError(void* ctx) {
    const auto* job = static_cast<InPageJob*>(ctx);
    const ULONG_PTR args[3] = {0, reinterpret_cast<ULONG_PTR>(job->address),
                               static_cast<ULONG_PTR>(0xC000009C)};
    RaiseException(EXCEPTION_IN_PAGE_ERROR, 0, 3, args);
}
// A decode that faults inside the file's guarded() at the middle of its view.
Result<void> faultingDecode(const LdFile& file, std::uint32_t, std::uint64_t, std::uint64_t, store::Dtype,
                            const std::optional<LdConversion>&, std::span<std::byte>) {
    const MappedFile* mapped = file.mapped();
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "the test file is not mapped");
    }
    InPageJob job{mapped->bytes().data() + mapped->size() / 2};
    return mapped->guarded(&raiseInPageError, &job);
}
#endif

} // namespace

#if defined(ESSES_OS_WINDOWS)
TEST_CASE("reload: a fault injected inside guarded() seals the chunk AllInvalid, logged once per source",
          "[unit][m4][reload]") {
    TempDir dir("reload-fault");
    const fs::path path = dir.path() / "fault.ld";
    REQUIRE(LdWriter::write(path, faultInput()).ok());
    auto file = LdFile::open(path);
    REQUIRE(file.ok());
    REQUIRE(file.value()->mapped() != nullptr);
    auto log = std::make_shared<DecodeLog>();
    LdChunkProvider counter(file.value(), 0, log);
    LdChunkProvider gear(file.value(), 1, log); // another channel of the same source: the same DecodeLog
    counter.setDecoder(&faultingDecode);
    gear.setDecoder(&faultingDecode);

    const store::OwnedChunk c0 = sealedChunk(counter, 0);
    CHECK(c0->validity() == Validity::AllInvalid);
    CHECK(c0->count() == static_cast<std::uint32_t>(kChunkSamples));
    CHECK(log->count(code::kIoFault) == 1);
    const store::OwnedChunk c2 = sealedChunk(counter, 2);
    CHECK(c2->validity() == Validity::AllInvalid);
    CHECK(c2->count() == 500u);
    const store::OwnedChunk g1 = sealedChunk(gear, 1);
    CHECK(g1->validity() == Validity::AllInvalid);
    CHECK(log->count(code::kIoFault) == 1); // once per source, not per chunk or channel
    CHECK_FALSE(log->contains(code::kSourceChanged));

    // The process and the mapping are fine: the real decode reads the chunk again.
    counter.setDecoder(nullptr);
    const store::OwnedChunk again = sealedChunk(counter, 0);
    CHECK(again->validity() == Validity::AllValid);
    CHECK(again->samples<std::int32_t>()[0] == 7);
    CHECK(again->samples<std::int32_t>()[100] == 7 + 3 * 100);
}

TEST_CASE("reload: channels faulting together log ld.io_fault once per source", "[unit][m4][reload]") {
    TempDir dir("reload-fault-together");
    const fs::path path = dir.path() / "fault.ld";
    REQUIRE(LdWriter::write(path, faultInput()).ok());
    auto file = LdFile::open(path);
    REQUIRE(file.ok());
    REQUIRE(file.value()->mapped() != nullptr);
    // Eight workers, four per channel, each decoding one chunk of the shared source at once (the pool's
    // workers do the same when a graph asks for many chunks). Started together; no assertion on a thread.
    constexpr int kWorkers = 8;
    constexpr int kRounds = 10;
    for (int pass = 0; pass < kRounds; ++pass) {
        auto log = std::make_shared<DecodeLog>();
        std::vector<std::unique_ptr<LdChunkProvider>> providers;
        for (int t = 0; t < kWorkers; ++t) {
            providers.push_back(
                std::make_unique<LdChunkProvider>(file.value(), static_cast<std::uint32_t>(t % 2), log));
            providers.back()->setDecoder(&faultingDecode);
        }
        std::atomic<int> started{0};
        std::atomic<int> sealedInvalid{0};
        std::vector<std::thread> workers;
        workers.reserve(kWorkers);
        for (int t = 0; t < kWorkers; ++t) {
            workers.emplace_back([&, t] {
                started.fetch_add(1);
                while (started.load() < kWorkers) {
                    std::this_thread::yield();
                }
                LdChunkProvider& p = *providers[static_cast<std::size_t>(t)];
                store::ChunkBuilder builder;
                if (!p.materialise(static_cast<store::SampleIdx>(t % 3), builder, {}).ok()) {
                    return;
                }
                auto sealed = builder.seal(p.version());
                if (sealed.ok() && (*sealed)->validity() == Validity::AllInvalid) {
                    sealedInvalid.fetch_add(1);
                }
            });
        }
        for (std::thread& w : workers) {
            w.join();
        }
        CHECK(sealedInvalid.load() == kWorkers); // every chunk sealed AllInvalid, the process kept
        CHECK(log->count(code::kIoFault) == 1);  // once for the source, whichever worker came first
    }
}
#else
TEST_CASE("reload: a fault inside guarded() over a truncated mapping seals the chunk AllInvalid",
          "[unit][m4][reload]") {
    TempDir dir("reload-fault");
    const fs::path path = dir.path() / "fault.ld";
    const fs::path aside = dir.path() / "fault.ld.aside";
    REQUIRE(LdWriter::write(path, faultInput()).ok());
    auto file = LdFile::open(path);
    REQUIRE(file.ok());
    const MappedFile* mapped = file.value()->mapped();
    REQUIRE(mapped != nullptr);
    auto log = std::make_shared<DecodeLog>();
    LdChunkProvider counter(file.value(), 0, log);
    LdChunkProvider gear(file.value(), 1, log);
    // A file of the same size and modification time takes the name, so the provider's re-stat sees no change;
    // then the mapped file (renamed aside, still the mapping's) is truncated by a writer that ignores
    // sharing.
    fs::rename(path, aside);
    reload_test::writeBytes(path, std::string(static_cast<std::size_t>(fs::file_size(aside)), 'z'));
    fs::last_write_time(path, fs::last_write_time(aside));
    REQUIRE_FALSE(mapped->changedOnDisk());
    fs::resize_file(aside, 0);

    const store::OwnedChunk c0 = sealedChunk(counter, 0);
    CHECK(c0->validity() == Validity::AllInvalid);
    CHECK(c0->count() == static_cast<std::uint32_t>(kChunkSamples));
    const store::OwnedChunk g2 = sealedChunk(gear, 2);
    CHECK(g2->validity() == Validity::AllInvalid);
    CHECK(log->count(code::kIoFault) == 1); // once per source
    CHECK_FALSE(log->contains(code::kSourceChanged));
}
#endif

TEST_CASE("reload: after a rename-over the provider decodes no further chunk", "[unit][m4][reload]") {
    TempDir dir("reload-replaced");
    const fs::path path = dir.path() / "replaced.ld";
    const fs::path next = dir.path() / "replaced.ld.tmp";
    REQUIRE(LdWriter::write(path, faultInput()).ok());
    REQUIRE(reload_test::shiftMtime(path, -std::chrono::hours(1))); // the replacement is newer on any system
    auto file = LdFile::open(path);
    REQUIRE(file.ok());
    auto log = std::make_shared<DecodeLog>();
    LdChunkProvider counter(file.value(), 0, log);
    const store::OwnedChunk before = sealedChunk(counter, 0);
    CHECK(before->validity() == Validity::AllValid);

    // A writer replaces the session (the same channels, other values) by rename-over: allowed while mapped.
    LdWriteInput other = faultInput();
    other.channels[0].rawInts = ramp(kSamples, 1000, 1);
    REQUIRE(LdWriter::write(next, other).ok());
    REQUIRE(reload_test::renameOver(next, path));
    REQUIRE(file.value()->mapped()->changedOnDisk());

    store::ChunkBuilder builder;
    const Result<void> r = counter.materialise(1, builder, {});
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Io);
    REQUIRE(r.error().context.path.has_value());
    CHECK(r.error().context.path->filename() == "replaced.ld");
    CHECK(log->count(code::kSourceChanged) == 1);
    store::ChunkBuilder again;
    CHECK_FALSE(counter.materialise(2, again, {}).ok());
    CHECK(log->count(code::kSourceChanged) == 1); // logged once
    CHECK_FALSE(log->contains(code::kIoFault));
    // The chunk decoded before the change keeps the old values, and the mapping still holds the old bytes.
    CHECK(before->samples<std::int32_t>()[1] == 7 + 3);
    const Result<std::vector<double>> old = file.value()->decodeAsDouble(0, kChunkSamples, 2);
    REQUIRE(old.ok());
    CHECK(old.value()[0] == static_cast<double>(7 + 3 * kChunkSamples));
}
