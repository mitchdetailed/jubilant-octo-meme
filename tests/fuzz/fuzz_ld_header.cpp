// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// libFuzzer target fuzz_ld_header (08 section 6.8, 05 section 9.3; AC-M0-06): one input is one .ld image in
// memory. The motec.ld importer sniffs it and opens it (header, detail records, device events, enumeration
// block, channel walk with gap and aux blocks), probe_json is serialised (inputs with up to 64 channels), and
// the reader decodes the last samples of the first channels through readBlock. Invariants: no crash, no
// sanitizer report, no exception, every loop bounded by the input size.
#include <esses/formats/byte_source.hpp>
#include <esses/formats/importer.hpp>
#include <esses/formats/ld_importer.hpp>
#include <esses/store/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <variant>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace esses;
    using namespace esses::formats;
    static const std::unique_ptr<IImporter> importer = makeLdImporter(nullptr);

    std::vector<std::byte> bytes(size);
    if (size > 0) {
        std::memcpy(bytes.data(), data, size);
    }
    const std::shared_ptr<const ByteSource> source = makeMemorySource(std::move(bytes), "fuzz.ld");

    SniffInput sniffIn;
    sniffIn.path = "fuzz.ld";
    sniffIn.head = source->span(0, kSniffHeadBytes);
    sniffIn.size = source->size();
    (void)importer->sniff(sniffIn);

    // open() parses what probe() parses (the reader's probe equals the importer's), plus the reader state.
    auto reader = importer->open(source, ImportOptions{});
    if (!reader) {
        return 0;
    }
    const ProbeResult& p = (*reader)->probe();
    // probe_json of small inputs only: under ASan the canonical writer costs about 2 ms per channel, which
    // would cut the executions per second of channel-list mutations tenfold.
    std::size_t channels = 0;
    for (const LayerInfo& l : p.layers) {
        channels += l.channels.size();
    }
    if (channels <= 64) {
        (void)p.canonicalJson();
    }
    if (p.layers.empty()) {
        return 0;
    }
    std::size_t decoded = 0;
    for (const ChannelInfo& c : p.layers.front().channels) {
        if (decoded++ == 16) {
            break;
        }
        if (!std::holds_alternative<Rate>(c.timing)) {
            continue;
        }
        const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(c.sampleCount, 64));
        if (n == 0) {
            continue;
        }
        std::vector<std::byte> values(std::size_t{n} * store::dtypeSize(c.dtype));
        std::vector<std::uint8_t> valid((n + 7) / 8);
        Block block;
        block.dtype = c.dtype;
        block.first = c.sampleCount - n; // the end of the array: where truncation shows
        block.count = n;
        block.values = values;
        block.validBits = valid;
        (void)(*reader)->readBlock(0, c.index, block.first, n, block, {});
    }
    return 0;
}
