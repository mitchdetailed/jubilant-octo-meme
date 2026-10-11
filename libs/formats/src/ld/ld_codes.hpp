// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Import Log codes of the .ld/.ldx core (05 section 1.3 rule 9). Where tools/ld_ref_decode.py emits a code
// with the same meaning, the reader uses the same name (`python -I tools/ld_ref_decode.py --list-codes`), so
// the reader and the independent oracle speak one vocabulary (D-041). The table is the single list the
// documentation check (AC-M0-12) can read.
#pragma once

#include <esses/formats/import_log.hpp>

#include <span>
#include <string_view>

namespace esses::formats::ld {

struct CodeInfo {
    std::string_view code;
    ImportLogEntry::Level level;
    bool sharedWithOracle; // same name and meaning in tools/ld_ref_decode.py
    std::string_view meaning;
};

// Every code the reader, the chunk provider, the writer and the .ldx parser can emit.
std::span<const CodeInfo> allCodes() noexcept;
// nullptr when the code is not in the table.
const CodeInfo* findCode(std::string_view code) noexcept;

namespace code {
// header, pointers and detail records
inline constexpr std::string_view kTooSmall = "ld.too_small";
inline constexpr std::string_view kMagicUnexpected = "ld.magic_unexpected";
inline constexpr std::string_view kHeaderOnly = "ld.header_only";
inline constexpr std::string_view kMetaPtrOutOfRange = "ld.meta_ptr_out_of_range";
inline constexpr std::string_view kDataPtrOutOfRange = "ld.data_ptr_out_of_range";
inline constexpr std::string_view kDataPtrMismatch = "ld.data_ptr_mismatch";
inline constexpr std::string_view kBlockOutOfRange = "ld.block_out_of_range";
inline constexpr std::string_view kDateUnparsable = "ld.date_unparsable";
inline constexpr std::string_view kProLoggingNonzero = "ld.pro_logging_nonzero";
inline constexpr std::string_view kStringEncoding = "ld.string_encoding";
inline constexpr std::string_view kStringTrailingBytes = "ld.string_trailing_bytes";
inline constexpr std::string_view kLegacyLayout = "ld.legacy_layout";
// channel list
inline constexpr std::string_view kCountMismatch = "ld.count_mismatch";
inline constexpr std::string_view kListCycle = "ld.list_cycle";
inline constexpr std::string_view kListBackwards = "ld.list_backwards";
inline constexpr std::string_view kListPrevMismatch = "ld.list_prev_mismatch";
inline constexpr std::string_view kEntryOutOfRange = "ld.entry_out_of_range";
inline constexpr std::string_view kTerminatorEntry = "ld.terminator_entry";
inline constexpr std::string_view kZeroRate = "ld.zero_rate";
inline constexpr std::string_view kEmptyChannel = "ld.empty_channel";
inline constexpr std::string_view kDataOutOfRange = "ld.data_out_of_range";
inline constexpr std::string_view kUnknownDtype = "ld.unknown_dtype";
inline constexpr std::string_view kDtypeGuessed = "ld.dtype_guessed";
inline constexpr std::string_view kZeroScale = "ld.zero_scale";
inline constexpr std::string_view kZeroMul = "ld.zero_mul";
inline constexpr std::string_view kNegativeScaling = "ld.negative_scaling";
inline constexpr std::string_view kShiftNonzero = "ld.shift_nonzero";
inline constexpr std::string_view kShiftMulOrder = "ld.shift_mul_order";
inline constexpr std::string_view kFloatScalingNonidentity = "ld.float_scaling_nonidentity";
inline constexpr std::string_view kDuplicateName = "ld.duplicate_name";
inline constexpr std::string_view kNameUnterminated = "ld.name_unterminated";
inline constexpr std::string_view kUnitFromShortName = "ld.unit_from_short_name";
inline constexpr std::string_view kUnitFromShortNameCandidate = "ld.unit_from_short_name_candidate";
inline constexpr std::string_view kDpsFromDec = "ld.dps_from_dec";
inline constexpr std::string_view kInterpDefault = "ld.interp_default";
inline constexpr std::string_view kBeaconChannel = "ld.beacon_channel";
// gap, aux, enumeration and device-event blocks
inline constexpr std::string_view kGapBlockMalformed = "ld.gap_block_malformed";
inline constexpr std::string_view kGapUnpaired = "ld.gap_unpaired";
inline constexpr std::string_view kGapUnsorted = "ld.gap_unsorted";
inline constexpr std::string_view kGapOutOfRange = "ld.gap_out_of_range";
inline constexpr std::string_view kGapRateMismatch = "ld.gap_rate_mismatch";
inline constexpr std::string_view kGapBlockShared = "ld.gap_block_shared";
inline constexpr std::string_view kGapConventionShifted = "ld.gap_convention_shifted";
inline constexpr std::string_view kAuxOutOfRange = "ld.aux_out_of_range";
inline constexpr std::string_view kAuxUnknownArg = "ld.aux_unknown_arg";
inline constexpr std::string_view kEnumBlockMalformed = "ld.enum_block_malformed";
inline constexpr std::string_view kEnumRefMissing = "ld.enum_ref_missing";
inline constexpr std::string_view kEnumScanFailed = "ld.enum_scan_failed";
inline constexpr std::string_view kEnumIndexDuplicate = "ld.enum_index_duplicate";
inline constexpr std::string_view kEnumRefIgnored = "ld.enum_ref_ignored";
inline constexpr std::string_view kEventBlockUnknown = "ld.event_block_unknown";
inline constexpr std::string_view kEventBlockMalformed = "ld.event_block_malformed";
// decode time (DecodeLog)
inline constexpr std::string_view kSourceChanged = "ld.source_changed";
inline constexpr std::string_view kIoFault = "ld.io_fault";
// writer notes (WriteReport)
inline constexpr std::string_view kTruncated = "ld.truncated";
inline constexpr std::string_view kValueClamped = "ld.value_clamped";
inline constexpr std::string_view kChannelSkipped = "ld.channel_skipped";
inline constexpr std::string_view kDetailSkipped = "ld.detail_skipped";
// .ldx companion
inline constexpr std::string_view kLdxParseError = "ldx.parse_error";
inline constexpr std::string_view kLdxUnknownElement = "ldx.unknown_element";
inline constexpr std::string_view kLdxUnknownAttribute = "ldx.unknown_attribute";
inline constexpr std::string_view kLdxUnknownMarkerClass = "ldx.unknown_marker_class";
inline constexpr std::string_view kLdxMarkerTimeUnparsable = "ldx.marker_time_unparsable";
inline constexpr std::string_view kLdxMarkerBeyondEnd = "ldx.marker_beyond_end";
inline constexpr std::string_view kLdxMarkerUnsorted = "ldx.marker_unsorted";
} // namespace code

} // namespace esses::formats::ld
