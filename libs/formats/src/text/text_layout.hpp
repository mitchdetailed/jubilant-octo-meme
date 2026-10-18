// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The TextEngine's internal model of one opened text file (05 section 7.1): what pass 1 (analyse) learns and
// pass 2 (decodeChunk) reads. Internal to esses_formats.
//
// Rows and samples. A data row is a non-blank line after the header. Columns are grouped into row sets:
//   * the dense set (index 0) holds every column that has no own index. Uniform time: every data row is one
//     sample (time from the rate). Timestamped time (the time column is not uniform): a row is a sample when
//     its time is valid and later than the previous sample's; a row with the same time as the pending sample
//     replaces it (last wins, 02 section 3.3 rule 12), an earlier time is dropped. The dense set's columns
//     share one I64 time column.
//   * a sparse column (profile sparse "timestamped" and the column has empty cells) gets its own set: its
//     samples are the rows where its cell is not empty, under the same time rule, with its own time column.
// Each set records where every chunk of 65,536 samples starts (byte offset, line, the time of the sample
// before it and, for hold columns, the value carried in), so any chunk decodes alone, in any order, and
// re-decodes identically after eviction.
#pragma once

#include "text/text_scan.hpp"

#include <esses/base/time.hpp>
#include <esses/formats/byte_source.hpp>
#include <esses/formats/dictionary.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/formats/importer.hpp>
#include <esses/formats/probe.hpp>
#include <esses/formats/text_profile.hpp>
#include <esses/store/chunk.hpp>
#include <esses/store/types.hpp>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace esses::formats::text {

// Import Log codes of the text engine (docs/formats/text.md lists them with their meaning).
namespace code {
inline constexpr std::string_view kEncoding = "text.encoding";                   // Info: detail = encoding
inline constexpr std::string_view kAutoDelimiter = "text.auto_delimiter";        // Info: detail = name
inline constexpr std::string_view kAutoDecimal = "text.auto_decimal";            // Info: detail = "." | ","
inline constexpr std::string_view kAutoPreamble = "text.auto_preamble";          // Info: detail = lines
inline constexpr std::string_view kAutoUnitsRow = "text.auto_units_row";         // Info: detail = yes | no
inline constexpr std::string_view kAutoTimeColumn = "text.auto_time_column";     // Info: detail = name
inline constexpr std::string_view kAutoTimeUnit = "text.auto_time_unit";         // Info: detail = unit
inline constexpr std::string_view kRateInferred = "text.rate_inferred";          // Info: detail = num/den
inline constexpr std::string_view kRateFromDetail = "text.rate_from_detail";     // Info: detail = num/den
inline constexpr std::string_view kRateDefault = "text.rate_default";            // Warning: no rate found
inline constexpr std::string_view kTimeRateMismatch = "text.time_rate_mismatch"; // Warning: -> timestamped
inline constexpr std::string_view kTimestamped = "text.timestamped";             // Info: time not uniform
inline constexpr std::string_view kTimeNotMonotonic = "text.time_not_monotonic"; // Warning: rows dropped
inline constexpr std::string_view kTimeDuplicate = "text.time_duplicate";      // Info: equal times, last wins
inline constexpr std::string_view kNoTimeColumn = "text.no_time_column";       // Warning
inline constexpr std::string_view kTimeInvalid = "text.time_invalid";          // Warning: rows without time
inline constexpr std::string_view kDuplicateName = "text.duplicate_name";      // Info: detail = new name
inline constexpr std::string_view kEmptyName = "text.empty_name";              // Info: column dropped
inline constexpr std::string_view kSparseColumn = "text.sparse_column";        // Info: own time column
inline constexpr std::string_view kTextColumn = "text.text_column";            // Info: values -> enum
inline constexpr std::string_view kEnumOverflow = "text.enum_overflow";        // Warning: too many labels
inline constexpr std::string_view kNoDataRows = "text.no_data_rows";           // Warning
inline constexpr std::string_view kLapSource = "text.lap_source";              // Info: detail = kind/column
inline constexpr std::string_view kLapSourceAbsent = "text.lap_source_absent"; // Info: optional, none found
inline constexpr std::string_view kLapTableUnverified = "text.lap_table_unverified"; // Warning (D-106)
inline constexpr std::string_view kCaptureUnparsed = "text.capture_unparsed"; // Warning: @input unusable
inline constexpr std::string_view kQuotedNewline = "text.quoted_newline";     // Warning: a field spans lines
// DecodeLog (pass 2, coalesced per (code, channel)).
inline constexpr std::string_view kRaggedRow = "text.ragged_row"; // Warning: missing cells invalid
inline constexpr std::string_view kParseError =
    "text.parse_error"; // Warning: cell neither number nor sentinel
inline constexpr std::string_view kSourceChanged = "text.source_changed"; // Warning: fewer rows than indexed
} // namespace code

enum class TimeUnit : std::uint8_t { Seconds, Millis, Micros, Clock, EpochSeconds, EpochMillis };

inline constexpr std::uint64_t kNoRow = std::numeric_limits<std::uint64_t>::max();

struct ChunkStart {
    std::uint64_t offset = 0;                             // byte offset of the chunk's first sample row
    std::uint64_t line = 0;                               // its 1-based line number (log context)
    Micros prevTime = std::numeric_limits<Micros>::min(); // time of the sample before it (timestamped sets)
    std::vector<double> carry;                            // per hold member: the value carried in (NaN: none)
};

struct RowSet {
    bool timestamped = false;
    std::int32_t sparseColumn = -1;     // the column whose non-empty cells select rows; -1: dense
    std::vector<std::uint32_t> members; // column plan indexes, ascending
    std::vector<ChunkStart> chunks;
    std::uint64_t samples = 0;
    Micros tFirst = 0; // first and last sample time (timestamped sets)
    Micros tLast = 0;
};

struct ColumnPlan {
    std::uint32_t field = 0; // cell index within a row
    std::string loggedName;
    std::string vendorUnit;
    store::SampleKind kind = store::SampleKind::Real;
    store::Dtype dtype = store::Dtype::F32;
    int dps = 0;
    std::optional<store::Interp> interp; // from the profile's columns; nullopt: the kind's default
    std::vector<std::string> sentinels;  // the column's own list, else the profile's
    bool hold = false;                   // sparse "hold": empty cells repeat the previous value
    std::uint32_t rowSet = 0;
    // Text columns (kind Enum): labels in first-seen order; id = position.
    std::optional<std::uint32_t> enumTable;
    std::vector<std::string> enumLabels;
    std::unordered_map<std::string, std::int32_t> enumIds;
    // The profile's column override (name, quantity, unit, interp, kind, dtype) as a dictionary entry for
    // pipeline step 4a; nullopt when the profile has none for this column.
    std::optional<Dictionary::Entry> hint;
};

// Everything pass 2 needs, immutable after analyse().
struct Layout {
    std::shared_ptr<const ByteSource> source; // keeps the mapping alive (null while probing)
    std::string owned;                        // UTF-16 input transcoded to UTF-8
    std::string_view text;                    // the whole file as UTF-8 or cp1252 bytes, BOM skipped
    std::uint64_t textBase = 0;               // byte offset of text[0] in the file (BOM length)
    Enc enc = Enc::Utf8;
    bool crOnly = false;
    Dialect dialect;
    std::size_t headerFields = 0; // cells of the names row (a trailing delimiter's empty cell excluded)
    bool trailingDelimiter = false;
    std::uint64_t dataStart = 0; // offset (in text) of the first data row
    std::uint64_t dataLine = 1;  // its 1-based line number
    std::uint64_t rows = 0;      // data rows
    std::vector<std::string> sentinels;
    // Time.
    std::int32_t timeField = -1;
    TimeUnit timeUnit = TimeUnit::Seconds;
    Micros originUs = 0; // subtracted from every parsed time
    bool uniform = true;
    Rate rate{1, 1};
    Micros t0Us = 0; // uniform: the time of row 0
    std::vector<ColumnPlan> columns;
    std::vector<RowSet> rowSets;       // [0] = the dense set
    std::uint64_t formatHeaderEnd = 0; // the SourceId covers text[0, formatHeaderEnd)
};

struct AnalyseResult {
    std::shared_ptr<Layout> layout;
    ProbeResult probe;
    std::vector<ImportLogEntry> log;
};

// Pass 1 over `bytes` (05 section 7.1 steps 1-8). `mapped` is the file mapping `bytes` lies in (null for an
// in-memory source): its bytes are read through guarded windows (D-190). Errors: Format (no header row, a
// duplicate name with duplicateNames "error", a first line longer than the header window), Unsupported
// (layout "blocks"), Cancelled, Io (the mapped file faulted: it changed while it was read).
Result<AnalyseResult> analyse(const TextProfile& profile, std::span<const std::byte> bytes,
                              std::string_view importerId, std::string_view importerVersion,
                              const CancelToken& cancel, const MappedFile* mapped = nullptr);

// Parses one row's time cell to Micros (origin applied); nullopt when invalid.
std::optional<Micros> rowTime(const Layout& layout, const std::vector<Cell>& cells);

// Pass 2: decodes chunk `chunkNo` of row set `set` (05 section 7.1). Over a file mapping (layout.source's,
// when the text is not a transcoded copy) the file is first checked for a change on disk (fail Io and
// text.source_changed) and its bytes are read through guarded windows, so a file truncated under the mapping
// fails the chunk (Io) instead of the process (D-190). Every requested member column gets its
// builder allocated (count samples, its output dtype, with a validity bitmap) and filled; `times` (I64) gets
// the sample times of a timestamped set when non-null. `outDtype`/`toBase` give each column's stored dtype
// and conversion (pipeline step 4). Invalid cells: NaN and a cleared bit for floats, a cleared bit for I32.
struct MemberOut {
    std::uint32_t column = 0;
    store::ChunkBuilder* builder = nullptr;
};
struct Conversion {
    store::Dtype dtype = store::Dtype::F32;
    double a = 1.0;
    double b = 0.0;
};
Result<void> decodeChunk(const Layout& layout, const std::vector<Conversion>& conversions, std::uint32_t set,
                         std::uint64_t chunkNo, std::span<MemberOut> members, store::ChunkBuilder* times,
                         DecodeLog* log, const CancelToken& cancel);

} // namespace esses::formats::text
