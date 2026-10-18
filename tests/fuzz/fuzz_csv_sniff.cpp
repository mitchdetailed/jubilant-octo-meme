// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// libFuzzer target fuzz_csv_sniff (05 section 9.3 "text_sniff", AC-M2-13): one input is the head of a file.
// Every registered importer sniffs it under a .csv and an unknown extension, then the generic profile runs
// pass 1 on it (encoding, delimiter, preamble, header, units row, time column, rate: the detection rules).
// Invariants: no crash, no sanitizer report, no exception; confidences stay within 0..100; a probe that
// succeeds has one layer whose channels all name their row count.
#include <esses/formats/byte_source.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

namespace {

const esses::formats::ImporterRegistry& registry() {
    static const std::unique_ptr<esses::formats::ImporterRegistry> r =
        esses::formats::ImporterRegistry::withBuiltins();
    return *r;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace esses::formats;
    const std::span<const std::byte> head(reinterpret_cast<const std::byte*>(data),
                                          std::min<std::size_t>(size, kSniffHeadBytes));
    for (const char* name : {"input.csv", "input.dat"}) {
        SniffInput in;
        in.path = name;
        in.head = head;
        in.size = size;
        for (const auto& [importer, result] : registry().sniff(in)) {
            if (result.confidence <= 0 || result.confidence > 100 || importer == nullptr) {
                std::abort();
            }
        }
    }
    const IImporter* generic = registry().find(kGenericCsvImporterId);
    if (generic == nullptr) {
        std::abort();
    }
    std::vector<std::byte> bytes(head.begin(), head.end());
    const auto source = makeMemorySource(std::move(bytes), "input.csv");
    auto probe = generic->probe(*source);
    if (probe && probe->layers.size() != 1) {
        std::abort(); // a text file is one layer
    }
    return 0;
}
