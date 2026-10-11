// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// libFuzzer target fuzz_ldx (08 section 6.8, 05 section 9.3; AC-M0-06): one input is one .ldx document. The
// tolerant .ldx parser reads it (pugixml, marker mapping, unknown elements kept verbatim); a document it
// accepts is written back and must parse again (unknown elements are re-emitted byte-identically at their
// positions, so the output stays well-formed). Invariants: no crash, no sanitizer report, no exception.
#include "ld/ldx.hpp"

#include <esses/formats/import_log.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace esses::formats;
    ImportLog log;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    auto doc = ld::parseLdx(bytes, log, esses::Micros{60'000'000});
    if (!doc) {
        return 0;
    }
    const std::string text = ld::writeLdx(*doc);
    ImportLog again;
    auto reparsed = ld::parseLdx(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()), again);
    if (!reparsed) {
        std::abort(); // the writer produced a document its own parser refuses
    }
    return 0;
}
