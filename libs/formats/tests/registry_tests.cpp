// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ImporterRegistry (05 section 2): sniff ranking and confidence bands (05 section 1.3 rule 1, section 3.1),
// companion discovery, the importer and writer catalogue, the open entry points and their errors, load()
// on a TaskPool and the DecodeLog registry.
#include "formats_test_support.hpp"
#include "synthetic_families.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/base/taskpool.hpp>

#include <catch2/catch_test_macros.hpp>

#if defined(ESSES_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::test;
namespace fs = std::filesystem;

namespace {

// An importer that claims every file with a fixed confidence (ranking tests).
class FixedImporter final : public IImporter {
public:
    FixedImporter(std::string id, int confidence) : id_(std::move(id)), confidence_(confidence) {}
    std::string_view id() const override { return id_; }
    std::string_view version() const override { return "1.0.0"; }
    std::span<const std::string> extensions() const override { return {}; }
    SniffResult sniff(const SniffInput&) const override { return {confidence_, {}, "fixed"}; }
    Result<ProbeResult> probe(const ByteSource&) const override {
        return fail(ErrorCode::Unsupported, "test importer");
    }
    Result<std::unique_ptr<IReader>> open(std::shared_ptr<const ByteSource>,
                                          const ImportOptions&) const override {
        return fail(ErrorCode::Unsupported, "test importer");
    }

private:
    std::string id_;
    int confidence_;
};

std::vector<std::byte> smallLd() {
    ld::LdWriteInput in;
    in.channels.push_back(ld::test::intChannel("Gear", ld::layout::kTypeInt5, 4, 10, {1, 2, 3}));
    auto bytes = ld::LdWriter::build(in, fixedIdentity());
    REQUIRE(bytes.ok());
    return std::move(bytes).value();
}

void putU32(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }
}

} // namespace

TEST_CASE("registry: built-ins, lookup and the catalogue", "[unit][registry]") {
    const auto reg = ImporterRegistry::withBuiltins();
    REQUIRE(reg->find("motec.ld") != nullptr);
    CHECK(reg->find("motec.ld")->version() == kLdImporterVersion);
    // M2 (D-044): the shipped text profiles and the two CSV writers follow motec.ld.
    CHECK(reg->find("motec.csv") != nullptr);
    CHECK(reg->find("text.profile:generic.csv") != nullptr);
    REQUIRE(reg->writer("motec.ld") != nullptr);
    CHECK(reg->writer("motec.csv") != nullptr);
    CHECK(reg->writer("csv") != nullptr);
    CHECK(reg->importers().size() == 3);
    CHECK(reg->writers().size() == 3);
    const auto cat = nlohmann::json::parse(reg->catalogue().compact());
    CHECK(cat["importers"][0]["id"] == "motec.ld");
    CHECK(cat["importers"][0]["extensions"] == nlohmann::json::array({".ld"}));
    CHECK(cat["importers"][0]["optionsSchema"]["properties"]["ld"]["properties"].contains("shiftOrder"));
    CHECK(cat["writers"][0]["id"] == "motec.ld");
    CHECK(cat["writers"][0]["optionsSchema"]["properties"]["ld"]["properties"].contains("dialect"));
}

TEST_CASE("registry: sniff confidence bands and companions", "[unit][registry]") {
    ld::test::TempDir dir("sniff");
    const auto reg = ImporterRegistry::withBuiltins();
    const std::vector<std::byte> good = smallLd();

    writeBytes(dir.path() / "run.ld", good);
    writeText(dir.path() / "run.ldx", "<LDXFile/>");
    auto r = reg->sniff(dir.path() / "run.ld");
    REQUIRE(r.ok());
    REQUIRE(r->size() == 1);
    CHECK(r->front().first->id() == kLdImporterId);
    CHECK(r->front().second.confidence == 95);
    CHECK(r->front().second.reason == "magic 0x40, pointers plausible");
    REQUIRE(r->front().second.companions.size() == 1);
    CHECK(r->front().second.companions[0].filename() == fs::path("run.ldx"));

    // Structure checks fail, magic matches: 60 with the .ld extension, 50 without (rule 1 bands).
    std::vector<std::byte> broken = good;
    putU32(broken, ld::layout::kHdrMetaPtr, 0x7FFFFFF0u);
    writeBytes(dir.path() / "broken.ld", broken);
    writeBytes(dir.path() / "broken.bin", broken);
    auto b1 = reg->sniff(dir.path() / "broken.ld");
    REQUIRE(b1.ok());
    REQUIRE(b1->size() == 1);
    CHECK(b1->front().second.confidence == 60);
    CHECK(b1->front().second.reason.find("meta_ptr") != std::string::npos);
    CHECK(b1->front().second.companions.empty());
    auto b2 = reg->sniff(dir.path() / "broken.bin");
    REQUIRE(b2.ok());
    REQUIRE(b2->size() == 1);
    CHECK(b2->front().second.confidence == 50);

    // A first record whose prev pointer is set fails the structure check too.
    std::vector<std::byte> prev = good;
    const std::uint32_t meta = 0x6E2u + ld::layout::kEventRecordSize + ld::layout::kVenueRecordSize +
                               ld::layout::kVehicleRecordSize + ld::layout::kWeatherRecordSize;
    putU32(prev, meta + ld::layout::kRecPrev, 0x1234u);
    writeBytes(dir.path() / "prev.ld", prev);
    CHECK(reg->sniff(dir.path() / "prev.ld")->front().second.confidence == 60);

    // Header only: still 95.
    ld::LdWriteInput empty;
    writeLd(dir.path() / "empty.ld", empty, fixedIdentity());
    auto h = reg->sniff(dir.path() / "empty.ld");
    REQUIRE(h.ok());
    CHECK(h->front().second.confidence == 95);
    CHECK(h->front().second.reason.find("header only") != std::string::npos);

    // Extension only: 25; neither: no candidate. A file shorter than 4 bytes is not an error.
    writeText(dir.path() / "notes.ld", "this is text, not a log");
    writeText(dir.path() / "notes.dat", "this is text, not a log");
    writeText(dir.path() / "notes.txt", "this is text, not a log");
    writeText(dir.path() / "tiny.ld", "@");
    CHECK(reg->sniff(dir.path() / "notes.ld")->front().second.confidence == 25);
    CHECK(reg->sniff(dir.path() / "notes.dat")->empty());
    // M2: the generic delimited-text profile claims .txt by its extension (05 section 7.3, at most 30).
    REQUIRE(reg->sniff(dir.path() / "notes.txt")->size() == 1);
    CHECK(reg->sniff(dir.path() / "notes.txt")->front().first->id() == "text.profile:generic.csv");
    CHECK(reg->sniff(dir.path() / "notes.txt")->front().second.confidence == 30);
    CHECK(reg->sniff(dir.path() / "tiny.ld")->front().second.confidence == 25);
    CHECK(reg->sniff(dir.path() / "missing.ld").error().code == ErrorCode::Io);
    // A directory named like a log is Io on every OS (POSIX opens it as a stream and reads an empty head).
    fs::create_directories(dir.path() / "folder.ld");
    const auto folder = reg->sniff(dir.path() / "folder.ld");
    REQUIRE_FALSE(folder.ok());
    CHECK(folder.error().code == ErrorCode::Io);
    CHECK(folder.error().context.path == dir.path() / "folder.ld");

    // Only the 64 KiB head is visible: SniffInput built by hand.
    SniffInput in;
    in.path = "x.ld";
    in.head = std::span<const std::byte>(good).first(0x6E2);
    in.size = good.size();
    CHECK(reg->sniff(in).front().second.confidence == 95); // the first record lies beyond the head here
}

// Regression (Windows): sniff(path) read the head through a std::ifstream, which shares no delete access, so
// it was refused while the handle that renamed a replacement into place was still open, and an open stream
// made the replacement fail. It reads through readFileHead now, which shares delete access (the read-file
// tests run readFileHead's open against a writer thread).
TEST_CASE("registry: sniff reads a log through the handles of an atomic replacement", "[unit][registry]") {
    ld::test::TempDir dir("sniff-replaced");
    const auto reg = ImporterRegistry::withBuiltins();
    const std::vector<std::byte> good = smallLd();
    const fs::path log = dir.path() / "run.ld";
    REQUIRE(AtomicFile::write(log, good).ok());

#if defined(ESSES_OS_WINDOWS)
    // Held by a writer's handle (write and delete access) or by the renaming handle (delete access only),
    // both sharing everything: the head is read all the same (a std::ifstream is refused by both).
    for (const DWORD access : {DWORD{GENERIC_WRITE | DELETE}, DWORD{DELETE}}) {
        const HANDLE holder =
            CreateFileW(log.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(holder != INVALID_HANDLE_VALUE);
        const auto held = reg->sniff(log);
        CloseHandle(holder);
        REQUIRE(held.ok());
        REQUIRE_FALSE(held->empty());
        CHECK(held->front().second.confidence == 95);
    }
#endif

    // Replaced and sniffed at once, again and again.
    int writeFailures = 0;
    int sniffFailures = 0;
    int wrongResults = 0;
    for (int i = 0; i < 200; ++i) {
        if (!AtomicFile::write(log, good).ok()) {
            ++writeFailures;
            continue;
        }
        const auto r = reg->sniff(log);
        if (!r.ok()) {
            ++sniffFailures;
        } else if (r->empty() || r->front().second.confidence != 95) {
            ++wrongResults;
        }
    }
    CHECK(writeFailures == 0);
    CHECK(sniffFailures == 0);
    CHECK(wrongResults == 0);
}

TEST_CASE("registry: ranking is best first, ties keep registration order", "[unit][registry]") {
    ImporterRegistry reg;
    reg.add(std::make_unique<FixedImporter>("test.a", 40));
    reg.add(std::make_unique<FixedImporter>("test.b", 70));
    reg.add(std::make_unique<FixedImporter>("test.c", 40));
    reg.add(std::make_unique<FixedImporter>("test.zero", 0));
    SniffInput in;
    const auto r = reg.sniff(in);
    REQUIRE(r.size() == 3);
    CHECK(r[0].first->id() == "test.b");
    CHECK(r[1].first->id() == "test.a");
    CHECK(r[2].first->id() == "test.c");
}

TEST_CASE("registry: open entry points and their errors", "[unit][registry]") {
    ld::test::TempDir dir("open");
    const auto reg = ImporterRegistry::withBuiltins();
    const fs::path path = dir.path() / "run.ld";
    writeBytes(path, smallLd());

    auto probed = reg->probe(path);
    REQUIRE(probed.ok());
    CHECK(probed->second.layers.at(0).channels.size() == 1);

    auto opened = reg->open(path, {}, context());
    REQUIRE(opened.ok());
    CHECK(opened->source->prov.importerId == kLdImporterId);
    CHECK(opened->source->prov.importerVersion == kLdImporterVersion);
    CHECK(opened->source->prov.path == path);
    CHECK(opened->source->prov.size == fs::file_size(path));
    CHECK(opened->probe == probed->second);
    CHECK(logHas(opened->log, open_code::kUnitsSkipped));
    CHECK(reg->decodeLog(opened->source->id) == opened->decodeLog);

    // The same bytes in memory: the same SourceId (D-130), no sidecar, no file details.
    auto mem = reg->open(makeMemorySource(smallLd(), "run.ld"), {}, context());
    REQUIRE(mem.ok());
    CHECK(mem->source->id == opened->source->id);

    CHECK(reg->open(dir.path() / "missing.ld", {}, context()).error().code == ErrorCode::Io);
    writeText(dir.path() / "notes.dat", "plain text");
    CHECK(reg->open(dir.path() / "notes.dat", {}, context()).error().code == ErrorCode::NotFound);
    // M2: a .txt file goes to the generic text profile, which finds no table in it.
    writeText(dir.path() / "notes.txt", "plain text");
    CHECK(reg->open(dir.path() / "notes.txt", {}, context()).error().code == ErrorCode::Format);
    ImportOptions unknown;
    unknown.importerId = "test.none";
    CHECK(reg->open(path, unknown, context()).error().code == ErrorCode::NotFound);
    CHECK(reg->probe(path, unknown).error().code == ErrorCode::NotFound);

    // The user's override opens a file the sniffer would not choose.
    writeBytes(dir.path() / "run.bin", smallLd());
    ImportOptions forced;
    forced.importerId = "motec.ld";
    CHECK(reg->open(dir.path() / "run.bin", forced, context()).ok());

    // Bad importer options are Invalid with the JSON pointer.
    ImportOptions bad;
    bad.options = model::OpaqueJson::parse(R"({"ld": {"shiftOrder": "sideways"}})").value();
    auto r = reg->open(path, bad, context());
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Invalid);
    CHECK(r.error().context.detail.value_or("") == "/ld/shiftOrder");
    bad.options = model::OpaqueJson::parse(R"({"other": 1})").value();
    CHECK(reg->open(path, bad, context()).error().code == ErrorCode::Invalid);

    // No chunk cache installed and none given.
    if (store::ChunkCache::installed() == nullptr) {
        OpenContext none;
        CHECK(reg->open(path, {}, none).error().code == ErrorCode::Invalid);
    }

    // Cancellation before the open.
    CancelSource cancel;
    cancel.cancel();
    CHECK(reg->open(path, {}, context(), cancel.token()).error().code == ErrorCode::Cancelled);

    reg->release(opened->source->id);
    CHECK(reg->decodeLog(opened->source->id) == nullptr);
}

TEST_CASE("registry: load() runs the open as a TaskPool task", "[unit][registry]") {
    ld::test::TempDir dir("load");
    const auto reg = ImporterRegistry::withBuiltins(&shippedUnits());
    const fs::path path = dir.path() / "mixed.ld";
    writeLd(path, mixedInput(), fixedIdentity());
    TaskPool pool(2);
    OpenContext ctx = context(&shippedUnits());
    ctx.pool = &pool;
    auto future = reg->load(path, {}, pool, ctx);
    auto loaded = future.get();
    REQUIRE(loaded.ok());
    auto direct = reg->open(path, {}, ctx);
    REQUIRE(direct.ok());
    CHECK(loaded->source->id == direct->source->id);
    CHECK(loaded->probe == direct->probe);
    CHECK(loaded->source->layers.at(0).channels.size() == direct->source->layers.at(0).channels.size());
    // A column of the loaded source materialises through the pool's chunk tasks too.
    const auto& speed = channelNamed(*loaded->source, "Engine Speed");
    CHECK(dumpColumn(columnOf(speed)).values.size() == 500);
    pool.join();
}
