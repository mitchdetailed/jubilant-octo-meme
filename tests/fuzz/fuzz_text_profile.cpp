// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// libFuzzer target fuzz_text_profile (05 section 9.3 "text_profile"): one input is a text file read by each
// shipped text profile (motec.csv, text.profile:generic.csv): pass 1 (open) and pass 2 of the first slab of
// every channel (values and, for timestamped channels, times). An input that starts with '{' is also read as
// an importer-profile-1, dictionary-1 and unitmap-1 document, and a document that parses must write and
// parse back to the same text. Invariants: no crash, no sanitizer report, no exception, times strictly
// increasing within a timestamped slab.
#include <esses/formats/byte_source.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/dictionary.hpp>
#include <esses/formats/registry.hpp>
#include <esses/formats/text_profile.hpp>
#include <esses/formats/unit_mapping_profile.hpp>
#include <esses/store/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace esses;
using namespace esses::formats;

const ImporterRegistry& registry() {
    static const std::unique_ptr<ImporterRegistry> r = ImporterRegistry::withBuiltins();
    return *r;
}

template <class Doc> void roundTrip(std::string_view text) {
    std::vector<std::string> log;
    auto doc = Doc::parse(text, log);
    if (!doc) {
        return;
    }
    auto written = doc->toJson();
    if (!written) {
        return; // a non-finite number cannot be written; parse refused nothing else
    }
    auto again = Doc::parse(*written, log);
    if (!again || again->toJson().value() != *written) {
        std::abort(); // the canonical writer produced a document its reader does not reproduce
    }
}

void readFirstSlab(IReader& reader) {
    const ProbeResult& p = reader.probe();
    for (LayerIx li = 0; li < p.layers.size(); ++li) {
        // At most 16 channels: each readBlock sweeps the slab again, so all channels of a very wide header
        // would make one input quadratic (the application's slab decoder shares one sweep between them).
        const auto channels = std::min<std::size_t>(p.layers[li].channels.size(), 16);
        for (ChannelIx ci = 0; ci < channels; ++ci) {
            const ChannelInfo& c = p.layers[li].channels[ci];
            const auto n =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(c.sampleCount, store::kChunkSamples));
            if (n == 0) {
                continue;
            }
            std::vector<std::byte> values(std::size_t{n} * store::dtypeSize(c.dtype));
            std::vector<std::uint8_t> bits((n + 7) / 8, 0xFF);
            Block b;
            b.dtype = c.dtype;
            b.count = n;
            b.values = values;
            b.validBits = bits;
            (void)reader.readBlock(li, ci, 0, n, b, {});
            if (std::holds_alternative<Timestamped>(c.timing)) {
                std::vector<Micros> times(n);
                if (reader.readTimestamps(li, ci, 0, n, times, {})) {
                    for (std::size_t i = 1; i < times.size(); ++i) {
                        if (times[i] <= times[i - 1]) {
                            std::abort(); // the time rule keeps samples strictly increasing
                        }
                    }
                }
            }
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (!text.empty() && text.front() == '{') {
        roundTrip<TextProfile>(text);
        roundTrip<Dictionary>(text);
        roundTrip<UnitMappingProfile>(text);
    }
    const std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(data),
                                       reinterpret_cast<const std::byte*>(data) + size);
    for (const std::string_view id : {kLegacyCsvImporterId, kGenericCsvImporterId}) {
        const IImporter* importer = registry().find(id);
        if (importer == nullptr) {
            std::abort();
        }
        ImportOptions options;
        options.importerId = std::string(id);
        auto reader = importer->open(makeMemorySource(bytes, "input.csv"), options);
        if (reader) {
            readFirstSlab(**reader);
        }
    }
    return 0;
}
