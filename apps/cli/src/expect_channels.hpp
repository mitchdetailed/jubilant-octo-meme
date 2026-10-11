// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Expected channel values of a fixture (05 section 9.1): expected.channels/<NNN>_<name>.csv, one file per
// channel the oracle decoded, NNN being the channel's position in the oracle's record walk and <name> the
// oracle's channel name in the file-name encoding of cli_common.hpp. Header "index,timeUs,value,valid"; rows
// may cover a subset of the samples (windows) in ascending index order, but always include the last sample,
// so the reader's sample count is checked as last index + 1. Values are the oracle's F64 text (Python repr:
// shortest round trip, "nan", "inf", "-inf").
//
// Comparison (AC-M0-02): timestamps exactly; integers (I32, I64 storage) exactly; F32 within the fixture's
// F32 tolerance in float ulps; F64 within its F64 tolerance in double ulps. Validity per the fixture's
// oracle.validity: strict (the valid column is the expected validity), or finite-only (the oracle cannot see
// gap blocks: a sample the reader marks invalid while the oracle has a finite value is counted, not
// compared).
#pragma once

#include <esses/base/result.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::cli {

struct ExpectedSample {
    std::uint64_t index = 0;
    std::int64_t timeUs = 0;
    double value = 0.0;
    bool valid = true;
};

struct ExpectedChannel {
    std::filesystem::path file;
    std::uint32_t oracleIndex = 0;
    std::string name; // the oracle's channel name
    std::vector<ExpectedSample> samples;
};

// "007_Engine Speed.csv" -> (7, "Engine Speed"); nullopt for another file name.
std::optional<std::pair<std::uint32_t, std::string>> parseExpectedFileName(std::string_view fileName);
std::string expectedFileName(std::uint32_t oracleIndex, std::string_view name);

Result<std::vector<ExpectedSample>> parseExpectedCsv(std::string_view text);
Result<ExpectedChannel> readExpectedChannel(const std::filesystem::path& file);
// Every expected.channels/*.csv of a fixture directory, by oracle index.
Result<std::vector<ExpectedChannel>> readExpectedChannels(const std::filesystem::path& fixtureDir);

// Rewrites the valid column: 0 for the rows whose index lies in one of the half-open intervals.
std::string markInvalid(std::string_view csvText,
                        const std::vector<std::pair<std::uint64_t, std::uint64_t>>& invalid);

struct Tolerance {
    std::uint64_t f32Ulps = 1;
    std::uint64_t f64Ulps = 1;
};
// oracle.tolerance of fixture-1 ("exact" = 0 ulp, "<n>ulp").
Result<Tolerance> parseTolerance(const nlohmann::json& tolerance);

std::uint64_t ulpDistanceF32(double a, double b) noexcept; // after rounding both to float; 0 for two NaNs
std::uint64_t ulpDistanceF64(double a, double b) noexcept;

// Channel-name compatibility of a reader name with the oracle's 32-byte-field name: equal; equal but for the
// reader's " (n)" duplicate suffix; the reader's long name extends the field (truncation); or the same after
// decoding the reader's UTF-8 bytes as cp1252, which is what the oracle does with every string.
bool namesCompatible(std::string_view readerName, std::string_view oracleName);

} // namespace esses::cli
