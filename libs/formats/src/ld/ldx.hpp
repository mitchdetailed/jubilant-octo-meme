// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The .ldx companion (05 section 4, section 5 item 7): a tolerant DOM parser over pugixml and a writer that
// builds the document node by node. New code (the converter has no .ldx support); structure facts from 05
// section 4 and docs/formats/ld.md.
//
// Known structure: LDXFile > Layers > Layer > MarkerBlock > MarkerGroup(Name, Index) >
// Marker(Version, ClassName, Name, Flags, Time), and LDXFile > Layers > Details > String(Id, Value) (Details
// is also accepted under Layer). Marker Time is cumulative microseconds with two decimals; Flags are kept
// verbatim and never interpreted. Everything else - unknown elements anywhere, markers of an unknown class or
// with an unparsable time, extra Layer/Layers/MarkerBlock/Details elements - is kept as the verbatim source
// text of the element with its parent path and position, and the writer re-emits it byte-identically at that
// position. Attributes of known elements are kept in their original order, unknown ones included.
#pragma once

#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/formats/import_log.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses::formats::ld {

// The marker classes the reader turns into markers (05 section 4 rule 1).
bool isKnownMarkerClass(std::string_view cls) noexcept; // SOL EOL BCN SPLTBCN IGRDBCN RESET STGSTRT STGEND

struct LdxAttribute {
    std::string name, value;
    friend bool operator==(const LdxAttribute&, const LdxAttribute&) = default;
};

struct LdxGroup {
    std::string name;                     // MarkerGroup/@Name (e.g. "Beacons")
    std::string index;                    // MarkerGroup/@Index, opaque
    std::vector<LdxAttribute> attributes; // original order; Name/Index values come from the fields
    friend bool operator==(const LdxGroup&, const LdxGroup&) = default;
};

struct LdxMarker {
    std::uint32_t group = 0; // index into LdxDocument::groups
    std::string className;   // a known class
    std::string name;
    std::string version; // @Version, opaque ("100")
    std::string flags;   // @Flags verbatim
    std::uint32_t flagsValue = 0;
    double timeUs = 0.0;                  // @Time
    Micros tUs = 0;                       // llround(Time)
    std::uint32_t ordinal = 0;            // index among markers of the same (class, group, tUs) in file order
    std::uint32_t fileIndex = 0;          // position in the file among parsed markers
    std::vector<LdxAttribute> attributes; // original order incl. unknown ones; known values come from fields
    friend bool operator==(const LdxMarker&, const LdxMarker&) = default;
};

struct LdxDetail {
    std::string id, value;
    std::vector<LdxAttribute> attributes; // original order incl. unknown ones
    friend bool operator==(const LdxDetail&, const LdxDetail&) = default;
};

// An element kept verbatim: parent path ("LDXFile/Layers/Layer/MarkerBlock/MarkerGroup#0", ...) and the
// index among the parent's element children where it stood.
struct LdxOpaque {
    std::string parentPath;
    std::uint32_t position = 0;
    std::string xml;
    friend bool operator==(const LdxOpaque&, const LdxOpaque&) = default;
};

struct LdxDocument {
    std::vector<LdxAttribute> rootAttributes; // LDXFile: Locale, DefaultLocale, Version, ...
    std::vector<LdxAttribute> layersAttributes, layerAttributes, markerBlockAttributes, detailsAttributes;
    bool hasLayer = false;       // a Layer element exists (created anyway when there are markers)
    bool hasMarkerBlock = false; // a MarkerBlock element exists (likewise)
    bool hasDetails = false;
    bool detailsInLayer = false; // Details found under Layer (defensive form)
    std::vector<LdxGroup> groups;
    std::vector<LdxMarker> markers; // ordered by (tUs, file order)
    std::vector<LdxDetail> details;
    std::vector<LdxOpaque> opaque;

    // An empty document in the observed shape (LDXFile Version 1.6 with Locale attributes).
    static LdxDocument makeDefault();
    // Adds a marker (and its group when new); keeps the order and ordinals.
    void addMarker(std::string_view group, std::string_view className, std::string_view name, double timeUs,
                   std::string_view flags = "77");
    void setDetail(std::string_view id, std::string_view value);
    const LdxDetail* findDetail(std::string_view id) const noexcept;
    // Re-establishes the (tUs, file order) order and the ordinals after edits.
    void normalise();
};

// Parses .ldx bytes. A document that is not well-formed (or whose root is not LDXFile) logs ldx.parse_error
// and fails with Format. With durationUs, markers after the end log ldx.marker_beyond_end.
Result<LdxDocument> parseLdx(std::span<const std::byte> bytes, ImportLog& log,
                             std::optional<Micros> durationUs = std::nullopt);
Result<LdxDocument> readLdxFile(const std::filesystem::path& path, ImportLog& log,
                                std::optional<Micros> durationUs = std::nullopt);

// Serialises the document (UTF-8, LF, one-space indentation, XML declaration). Opaque elements are emitted
// byte-identically at their recorded positions.
std::string writeLdx(const LdxDocument& doc);
Result<void> writeLdxFile(const std::filesystem::path& path, const LdxDocument& doc); // AtomicFile

// Time attribute text: cumulative microseconds with two decimals.
std::string formatLdxTime(double timeUs);

} // namespace esses::formats::ld
