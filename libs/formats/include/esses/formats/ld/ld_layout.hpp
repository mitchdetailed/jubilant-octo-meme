// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// .ld binary layout constants (05 section 3; docs/formats/ld.md; docs/formats/ld-converter-port.md sections
// 1.1-1.10). Offsets, record sizes, type codes, block markers. No logic. All integers are little-endian; a
// pointer is a u32 absolute file offset and 0 means "absent".
#pragma once

#include <cstdint>

namespace esses::formats::ld::layout {

// ---- file header (0x000-0x6E1) ------------------------------------------------------------------------
inline constexpr std::uint32_t kMagic = 0x40;
inline constexpr std::uint32_t kTicksPerSecond = 1'000'000; // u32 at 0x042 on every file seen

inline constexpr std::uint32_t kHeaderSize = 0x6E2;
inline constexpr std::uint32_t kHdrMagic = 0x000;         // u32
inline constexpr std::uint32_t kHdrUnknownPtr004 = 0x004; // u32, unknown block (C-series)
inline constexpr std::uint32_t kHdrMetaPtr = 0x008;       // u32, first channel record
inline constexpr std::uint32_t kHdrDataPtr = 0x00C;       // u32, first sample array
inline constexpr std::uint32_t kHdrEventPtr = 0x024;      // u32, event record
inline constexpr std::uint32_t kHdrUnknownPtr02C = 0x02C; // u32, unknown (C-series: an offset into the file)
inline constexpr std::uint32_t kHdrUnknown040 = 0x040; // u16: 2 on M1/PC layout, 0 on C-series logger files
inline constexpr std::uint32_t kHdrTicks = 0x042;      // u32, 1'000'000
inline constexpr std::uint32_t kHdrSerial = 0x046;     // u32
inline constexpr std::uint32_t kHdrDeviceType = 0x04A; // 8 bytes
inline constexpr std::uint32_t kHdrDeviceVersion = 0x052;  // u16
inline constexpr std::uint32_t kHdrUnknown054 = 0x054;     // u16: 0 as downloaded, 0x0080 PC layout
inline constexpr std::uint32_t kHdrChannelCount = 0x056;   // u16
inline constexpr std::uint32_t kHdrChannelCount2 = 0x058;  // u16: the count again, or 0
inline constexpr std::uint32_t kHdrUnknown05A = 0x05A;     // u16
inline constexpr std::uint32_t kHdrUnknown05C = 0x05C;     // u16
inline constexpr std::uint32_t kHdrDate = 0x05E;           // 16 bytes read, 10 written ("DD/MM/YYYY")
inline constexpr std::uint32_t kHdrTime = 0x07E;           // 16 bytes read, 8 written ("HH:MM:SS")
inline constexpr std::uint32_t kHdrDriver = 0x09E;         // 64
inline constexpr std::uint32_t kHdrVehicleId = 0x0DE;      // 64
inline constexpr std::uint32_t kHdrEngineId = 0x11E;       // 64
inline constexpr std::uint32_t kHdrVenue = 0x15E;          // 64
inline constexpr std::uint32_t kHdrProLogging = 0x5DE;     // u32; never computed, written as 0 (D-143)
inline constexpr std::uint32_t kHdrSession = 0x5E4;        // 64
inline constexpr std::uint32_t kHdrShortComment = 0x624;   // 64
inline constexpr std::uint32_t kHdrUnknown687 = 0x687;     // u8
inline constexpr std::uint32_t kHdrDeviceEventPtr = 0x688; // u32, log-event block
inline constexpr std::uint32_t kHdrUnknownPtr68C = 0x68C;  // u32, unknown block
inline constexpr std::uint32_t kHdrEnumPtr = 0x690;        // u32, enumeration definition block
inline constexpr std::uint32_t kHdrTeam = 0x694;           // 32

inline constexpr std::uint32_t kDateReadWidth = 16;
inline constexpr std::uint32_t kDateWriteWidth = 10;
inline constexpr std::uint32_t kTimeReadWidth = 16;
inline constexpr std::uint32_t kTimeWriteWidth = 8;
inline constexpr std::uint32_t kText64 = 64;
inline constexpr std::uint32_t kText32 = 32;
inline constexpr std::uint32_t kDeviceTypeWidth = 8;

// ---- detail records ------------------------------------------------------------------------------------
inline constexpr std::uint32_t kEventRecordSize = 0xC54;   // 3,156
inline constexpr std::uint32_t kVenueRecordSize = 0xC1E;   // 3,102
inline constexpr std::uint32_t kVehicleRecordSize = 0xCF4; // 3,316
inline constexpr std::uint32_t kWeatherRecordSize = 0x800; // 2,048

inline constexpr std::uint32_t kEvName = 0x000;        // 64
inline constexpr std::uint32_t kEvSession = 0x040;     // 64
inline constexpr std::uint32_t kEvLongComment = 0x080; // 1024
inline constexpr std::uint32_t kEvVenuePtr = 0x480;    // u32
inline constexpr std::uint32_t kEvWeatherPtr = 0x484;  // u32
inline constexpr std::uint32_t kEvUsed = 0x488;        // the last used field ends here
inline constexpr std::uint32_t kLongCommentWidth = 1024;

inline constexpr std::uint32_t kVnName = 0x000;       // 64
inline constexpr std::uint32_t kVnLengthMm = 0x042;   // u32
inline constexpr std::uint32_t kVnVehiclePtr = 0x44A; // u32
inline constexpr std::uint32_t kVnCategory = 0x44E;   // 32
inline constexpr std::uint32_t kVnUsed = 0x46E;

inline constexpr std::uint32_t kVhId = 0x000;          // 64
inline constexpr std::uint32_t kVhDesc = 0x040;        // 64
inline constexpr std::uint32_t kVhEngineId = 0x080;    // 64
inline constexpr std::uint32_t kVhWeightKg = 0x0C0;    // u16
inline constexpr std::uint32_t kVhFuelTankX10 = 0x0C2; // u16, litres x 10
inline constexpr std::uint32_t kVhType = 0x0C4;        // 32
inline constexpr std::uint32_t kVhDriveType = 0x0E4;   // 32
inline constexpr std::uint32_t kVhDiffX1000 = 0x104;   // u16, ratio x 1000
inline constexpr std::uint32_t kVhGear1X1000 = 0x106;  // 10 x u16, ratio x 1000
inline constexpr std::uint32_t kVhTrackMm = 0x11A;     // u16
inline constexpr std::uint32_t kVhWheelbaseMm = 0x11C; // u16
inline constexpr std::uint32_t kVhComment = 0x120;     // 1024
inline constexpr std::uint32_t kVhNumber = 0x524;      // 32
inline constexpr std::uint32_t kVhUsed = 0x544;
inline constexpr std::uint32_t kGearCount = 10;

inline constexpr std::uint32_t kWxSky = 0x000;           // 64
inline constexpr std::uint32_t kWxAirTemp = 0x040;       // value 16 + unit 8 at +0x10
inline constexpr std::uint32_t kWxTrackTemp = 0x058;     // value 16 + unit 8
inline constexpr std::uint32_t kWxBaroPressure = 0x070;  // value 16 + unit 8
inline constexpr std::uint32_t kWxRelHumidity = 0x088;   // value 16 + unit 8
inline constexpr std::uint32_t kWxWindSpeed = 0x0A0;     // value 16 + unit 8
inline constexpr std::uint32_t kWxWindDirection = 0x0B8; // 64
inline constexpr std::uint32_t kWxComment = 0x0F8;       // 1024
inline constexpr std::uint32_t kWxUsed = 0x4F8;
inline constexpr std::uint32_t kWxValueWidth = 16;
inline constexpr std::uint32_t kWxUnitOffset = 0x10;
inline constexpr std::uint32_t kWxUnitWidth = 8;

// ---- channel record (124 bytes), doubly linked --------------------------------------------------------
inline constexpr std::uint32_t kRecordSize = 0x7C;
inline constexpr std::uint32_t kRecPrev = 0x00;      // u32
inline constexpr std::uint32_t kRecNext = 0x04;      // u32
inline constexpr std::uint32_t kRecData = 0x08;      // u32
inline constexpr std::uint32_t kRecCount = 0x0C;     // u32, samples
inline constexpr std::uint32_t kRecCounter = 0x10;   // u16
inline constexpr std::uint32_t kRecTypeCode = 0x12;  // u16 (dtype_a)
inline constexpr std::uint32_t kRecWidth = 0x14;     // u16 (bytes per sample)
inline constexpr std::uint32_t kRecRate = 0x16;      // u16, Hz
inline constexpr std::uint32_t kRecShift = 0x18;     // i16
inline constexpr std::uint32_t kRecMul = 0x1A;       // i16
inline constexpr std::uint32_t kRecScale = 0x1C;     // i16
inline constexpr std::uint32_t kRecDec = 0x1E;       // i16
inline constexpr std::uint32_t kRecName = 0x20;      // 32
inline constexpr std::uint32_t kRecShortName = 0x40; // 8 (or a 20-byte unit, unit rule)
inline constexpr std::uint32_t kRecUnit = 0x48;      // 12
inline constexpr std::uint32_t kRecRawMax = 0x54;    // i32
inline constexpr std::uint32_t kRecRawMin = 0x58;    // i32
inline constexpr std::uint32_t kRecRangeA = 0x5C;    // f32
inline constexpr std::uint32_t kRecRangeB = 0x60;    // f32
inline constexpr std::uint32_t kRecDps = 0x64;       // u8
inline constexpr std::uint32_t kRecInterp = 0x65;    // u8
inline constexpr std::uint32_t kRecGapPtr = 0x74;    // u32
inline constexpr std::uint32_t kRecAuxPtr = 0x78;    // u32
inline constexpr std::uint32_t kNameWidth = 32;
inline constexpr std::uint32_t kShortNameWidth = 8;
inline constexpr std::uint32_t kUnitWidth = 12;
inline constexpr std::uint32_t kLongUnitWidth = 20;
inline constexpr std::uint16_t kFirstCounter = 20101; // PC-layout counters: 20101 + index

// Legacy converter layout (port specification Q-W30): channel list where the venue record would sit.
inline constexpr std::uint32_t kLegacyListOffset = 0x1336;
inline constexpr std::uint32_t kLegacyRecordSize = 132;

// ---- gap (validity) block at record +0x74 --------------------------------------------------------------
inline constexpr std::uint32_t kGapRate = 0x02;       // u16
inline constexpr std::uint32_t kGapCount = 0x04;      // u32, 6-byte events (2 per gap)
inline constexpr std::uint32_t kGapArrayPtr = 0x08;   // u32, block + 44 on every file seen
inline constexpr std::uint32_t kGapHeaderSize = 0x2C; // 44
inline constexpr std::uint32_t kGapEventSize = 6;     // u16 kind + u32 sample index
inline constexpr std::uint16_t kGapEnter = 0;         // invalid from this sample
inline constexpr std::uint16_t kGapExit = 1;          // valid again from this sample

// ---- aux block at record +0x78 -------------------------------------------------------------------------
inline constexpr std::uint32_t kAuxContentPtr = 0x02;  // u32, block + 70 on every file seen
inline constexpr std::uint32_t kAuxContentOffset = 70; // 0x46
inline constexpr std::uint32_t kAuxContentMarker = 1;  // u32 at the content pointer, then u32 argument count
inline constexpr std::uint32_t kAuxArgLongName = 4;    // u32 4, u16 0, u16 len, char[len] incl. NUL
inline constexpr std::uint32_t kAuxArgEnumRef = 5;     // u32 5, u16 0, u16 2, u16 table index
inline constexpr std::uint32_t kAuxArgHeaderSize = 8;

// ---- enumeration definition block at header 0x690 -------------------------------------------------------
inline constexpr std::uint32_t kEnumBlockMarker = 1;
inline constexpr std::uint32_t kEnumTableMarker = 2;
inline constexpr std::uint32_t kEnumValueMarker = 3;
inline constexpr std::uint32_t kEnumSeverityMarker = 6;
inline constexpr std::uint16_t kEnumIndexSize = 2;
inline constexpr std::uint32_t kEnumSeverityBlockSize = 10;

// ---- log-event (device event) block at header 0x688 -----------------------------------------------------
inline constexpr std::uint32_t kEventBlockType = 1;
inline constexpr std::uint32_t kEventBlockHeaderSize = 0x2C; // type, count, array pointer, 32 reserved
inline constexpr std::uint32_t kEventRecordBytes = 20;       // u16 code, f64 time in us, 10 reserved

// ---- type codes (dtype_a) ------------------------------------------------------------------------------
inline constexpr std::uint16_t kTypeInt0 = 0x00;
inline constexpr std::uint16_t kTypeInt3 = 0x03;
inline constexpr std::uint16_t kTypeUnsigned4 = 0x04; // guess: raw unsigned, bitfield
inline constexpr std::uint16_t kTypeInt5 = 0x05;
inline constexpr std::uint16_t kTypeInt6 = 0x06; // plain integer (S-1 D9)
inline constexpr std::uint16_t kTypeFloat = 0x07;
inline constexpr std::uint16_t kTypeDouble = 0x08;

// Interpolation byte values (port specification section 1.7).
inline constexpr std::uint8_t kInterpClosest = 0;
inline constexpr std::uint8_t kInterpPrevious = 1;
inline constexpr std::uint8_t kInterpNext = 2;
inline constexpr std::uint8_t kInterpLinear = 3;

} // namespace esses::formats::ld::layout
