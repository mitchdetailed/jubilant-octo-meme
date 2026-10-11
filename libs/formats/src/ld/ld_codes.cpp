// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "ld/ld_codes.hpp"

#include <algorithm>
#include <array>

namespace esses::formats::ld {
namespace {

using L = ImportLogEntry::Level;

constexpr std::array kCodes = {
    // header, pointers and detail records
    CodeInfo{code::kTooSmall, L::Error, true, "file shorter than the 1,762-byte header; not read"},
    CodeInfo{code::kMagicUnexpected, L::Warning, true, "u32 at 0x00 is not 0x40"},
    CodeInfo{code::kHeaderOnly, L::Warning, false,
             "meta_ptr and data_ptr both equal the file size: a valid source without channels"},
    CodeInfo{code::kMetaPtrOutOfRange, L::Error, true,
             "header meta_ptr outside the file; no channel list read"},
    CodeInfo{code::kDataPtrOutOfRange, L::Warning, true,
             "header data_ptr outside the file (informational field)"},
    CodeInfo{code::kDataPtrMismatch, L::Info, true,
             "header data_ptr differs from the lowest channel data pointer"},
    CodeInfo{code::kBlockOutOfRange, L::Info, true,
             "event, venue, vehicle or weather record pointer does not fit in the file; record skipped"},
    CodeInfo{code::kDateUnparsable, L::Info, true,
             "header date/time not DD/MM/YYYY HH:MM[:SS]; no UTC anchor"},
    CodeInfo{code::kProLoggingNonzero, L::Info, true,
             "u32 at 0x5DE is non-zero (recorded as the pro-logging dialect flag, never interpreted)"},
    CodeInfo{code::kStringEncoding, L::Info, false,
             "text encoding used for the file's strings: ascii, utf-8, cp1252 or mixed (writer: UTF-8 used)"},
    CodeInfo{code::kStringTrailingBytes, L::Info, true,
             "non-zero bytes after the NUL terminator of a string field"},
    CodeInfo{code::kLegacyLayout, L::Info, false,
             "legacy converter layout (list at 0x1336, records longer than 124 bytes, no gap/aux pointers); "
             "extension data not recovered"},
    // channel list
    CodeInfo{code::kCountMismatch, L::Warning, true,
             "header channel count (u16 at 0x56) differs from the walk"},
    CodeInfo{code::kListCycle, L::Error, true, "channel list revisits an offset; walk aborted"},
    CodeInfo{code::kListBackwards, L::Info, true, "next pointer is lower than the current record"},
    CodeInfo{code::kListPrevMismatch, L::Info, true, "prev pointer does not name the previous record"},
    CodeInfo{code::kEntryOutOfRange, L::Error, true,
             "channel record outside the file or inside the header; walk stopped"},
    CodeInfo{
        code::kTerminatorEntry, L::Info, true,
        "all-zero last record (rate 0, count 0, type pair 0/0, empty name) treated as the list terminator; "
        "not a channel"},
    CodeInfo{code::kZeroRate, L::Warning, true,
             "channel rate 0 (any record other than the terminator); it cannot be timed and is skipped"},
    CodeInfo{code::kEmptyChannel, L::Info, true, "channel has zero samples"},
    CodeInfo{code::kDataOutOfRange, L::Warning, true,
             "sample array runs past the end of the file; count clamped"},
    CodeInfo{code::kUnknownDtype, L::Warning, true,
             "unrecognised (dtype_a, dtype) pair; decoded as a raw signed integer of that width, or skipped "
             "when the width is not 1, 2, 4 or 8"},
    CodeInfo{code::kDtypeGuessed, L::Info, true,
             "(dtype_a, dtype) pair decoded by a guess (0x04 unsigned, width 1, 0x07/8, 0x08/2|4, 0x06/2)"},
    CodeInfo{code::kZeroScale, L::Info, true, "scale is 0; treated as 1"},
    CodeInfo{code::kZeroMul, L::Warning, true, "mul is 0; treated as 1"},
    CodeInfo{code::kNegativeScaling, L::Info, true, "scale or mul is negative"},
    CodeInfo{code::kShiftNonzero, L::Info, true, "a channel has shift != 0 (once per file)"},
    CodeInfo{code::kShiftMulOrder, L::Info, true,
             "shift != 0 and mul != 1: the two shift orders differ; ctx.detail names the order used"},
    CodeInfo{code::kFloatScalingNonidentity, L::Warning, true,
             "float channel with non-identity scaling; scaling applied"},
    CodeInfo{code::kDuplicateName, L::Info, true,
             "two channels share a name; the later one gets a ' (n)' suffix"},
    CodeInfo{code::kNameUnterminated, L::Info, true, "32-byte name field has no NUL (possible truncation)"},
    CodeInfo{code::kUnitFromShortName, L::Info, false,
             "unit taken from the 8-byte field at 0x40 (unit rule (c)); flag UnitFromShortName"},
    CodeInfo{
        code::kUnitFromShortNameCandidate, L::Info, true,
        "C-series channel with a short name and no unit; the short name does not resolve as a unit and stays "
        "the short name"},
    CodeInfo{code::kDpsFromDec, L::Info, false, "display decimals byte is 0; dps taken from dec_places"},
    CodeInfo{
        code::kInterpDefault, L::Info, false,
        "interpolation byte not 0-3 (or 0 in the C-series logger record layout); the kind default applies"},
    CodeInfo{code::kBeaconChannel, L::Info, false,
             "converter beacon channel (1 Hz int16 lap words) exposed as an ordinary channel"},
    // gap, aux, enumeration and device-event blocks
    CodeInfo{
        code::kGapBlockMalformed, L::Warning, false,
        "gap block or its event array outside the file, or an event kind other than 0/1 (events that fit are "
        "kept); or the per-file gap budget (each event of the file once) is spent and the block is ignored"},
    CodeInfo{code::kGapUnpaired, L::Info, false,
             "gap entry without exit (invalid to the end of the data), or exit/entry out of turn"},
    CodeInfo{
        code::kGapUnsorted, L::Info, false,
        "gap intervals unsorted, overlapping or reversed (a reversed pair is read as [exit, entry)); merged"},
    CodeInfo{code::kGapOutOfRange, L::Info, false, "gap index beyond the sample count; clamped"},
    CodeInfo{code::kGapRateMismatch, L::Info, false, "gap block rate differs from the channel rate"},
    CodeInfo{code::kGapBlockShared, L::Info, false,
             "several channel records name the same gap block; it is parsed once"},
    CodeInfo{
        code::kGapConventionShifted, L::Info, false,
        "gap pairs in the shifted convention (entry = first invalid + 1, exit = last invalid): detected and "
        "read as written, or read as [entry - 1, exit + 1) with the option ld.gapConvention \"shifted\""},
    CodeInfo{
        code::kAuxOutOfRange, L::Info, false,
        "aux block or one of its arguments outside the file, more than 16 arguments (the first 16 read), or "
        "the per-file aux budget (the file size) spent"},
    CodeInfo{code::kAuxUnknownArg, L::Info, false, "aux block argument of unknown type; scan stopped"},
    CodeInfo{
        code::kEnumBlockMalformed, L::Warning, false,
        "enumeration block marker mismatch, item size below 3 or data beyond the file; earlier tables kept"},
    CodeInfo{code::kEnumRefMissing, L::Info, false,
             "channel references an enumeration table that does not exist; channel stays Integer"},
    CodeInfo{code::kEnumScanFailed, L::Info, true,
             "no enumeration block pointer and the fallback scan did not find a referenced table"},
    CodeInfo{code::kEnumIndexDuplicate, L::Info, false,
             "two enumeration tables share an index; the first one is used"},
    CodeInfo{code::kEnumRefIgnored, L::Info, false,
             "a scaled, float or wide-integer channel references an enumeration table; labels not applied "
             "(ctx.detail: scaled, float or wide integer)"},
    CodeInfo{code::kEventBlockUnknown, L::Info, false, "device-event block type is not 1; block ignored"},
    CodeInfo{code::kEventBlockMalformed, L::Warning, false,
             "device-event records beyond the file or with a non-finite time; records that fit are kept"},
    // decode time
    CodeInfo{code::kSourceChanged, L::Warning, false,
             "the .ld changed on disk after open; chunk not decoded"},
    CodeInfo{code::kIoFault, L::Warning, false,
             "page fault while decoding (truncated file, lost share); chunk "
             "sealed AllInvalid"},
    // writer notes
    CodeInfo{code::kTruncated, L::Info, false,
             "writer: string cut at its field width on a code-point boundary"},
    CodeInfo{code::kValueClamped, L::Warning, false,
             "writer: a raw value does not fit the channel's stored width; clamped"},
    CodeInfo{
        code::kChannelSkipped, L::Warning, false,
        "export: a channel the .ld cannot hold (timestamped, a rate that is not a whole number of Hz up to "
        "65535, or text) is not written"},
    CodeInfo{code::kDetailSkipped, L::Info, false,
             "export: a detail whose value does not fit its .ld header or record field is not written there"},
    // .ldx companion
    CodeInfo{code::kLdxParseError, L::Warning, true, ".ldx is not well-formed XML; the .ld still opens"},
    CodeInfo{code::kLdxUnknownElement, L::Info, true,
             ".ldx element outside the known structure (kept verbatim)"},
    CodeInfo{code::kLdxUnknownAttribute, L::Info, false, ".ldx attribute outside the known structure (kept)"},
    CodeInfo{
        code::kLdxUnknownMarkerClass, L::Info, true,
        "marker ClassName outside SOL EOL BCN SPLTBCN IGRDBCN RESET STGSTRT STGEND (kept verbatim, not a "
        "marker)"},
    CodeInfo{code::kLdxMarkerTimeUnparsable, L::Warning, true,
             "marker Time missing or not a finite number (kept verbatim, not a marker)"},
    CodeInfo{code::kLdxMarkerBeyondEnd, L::Info, true, "marker Time is after the end of the longest channel"},
    CodeInfo{code::kLdxMarkerUnsorted, L::Info, true,
             "markers are not in ascending Time order in file order (checked within each marker group)"},
};

} // namespace

std::span<const CodeInfo> allCodes() noexcept {
    return kCodes;
}

const CodeInfo* findCode(std::string_view c) noexcept {
    const auto it =
        std::find_if(kCodes.begin(), kCodes.end(), [&](const CodeInfo& i) { return i.code == c; });
    return it == kCodes.end() ? nullptr : &*it;
}

} // namespace esses::formats::ld
