// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Low-level text scanning for the TextEngine (05 section 7.1): encoding detection and transcoding, line
// iteration (LF, CRLF and CR-only files), cell splitting with quotes and trimming, number parsing through
// fast_float (decimal comma included; never std::from_chars, which Apple Clang 15's libc++ lacks for
// floating point), bounded regular expressions and the rational rate search. Internal to esses_formats.
#pragma once

#include <esses/base/rate.hpp>
#include <esses/base/result.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses {
class MappedFile;
} // namespace esses

namespace esses::formats::text {

enum class Enc : std::uint8_t { Utf8, Utf8Bom, Utf16Le, Utf16Be, Cp1252 };
std::string_view encName(Enc e) noexcept; // "utf-8", "utf-8-bom", "utf-16le", "utf-16be", "cp1252"

// BOM, else valid UTF-8 over the head (a sequence cut at the end of the head is tolerated), else cp1252.
Enc detectEncoding(std::span<const std::byte> head) noexcept;
// UTF-16 bytes (after the BOM) to UTF-8; unpaired surrogates and an odd trailing byte become U+FFFD.
std::string utf16ToUtf8(std::span<const std::byte> bytes, bool bigEndian);
// A string from the file as UTF-8: valid UTF-8 is kept (Utf8 encodings), cp1252 is transcoded, and invalid
// UTF-8 in a UTF-8 file falls back to cp1252 for that string (05 section 7.1 step 1, as the .ld strings).
std::string toUtf8(std::string_view s, Enc enc);

// ---- lines
// ---------------------------------------------------------------------------------------------------------

// Iterates the lines of a text: '\n' terminates a line (a preceding '\r' is dropped); in a file without any
// '\n', '\r' does (CR-only files). The view of a line excludes its terminator.
class LineCursor {
public:
    LineCursor(std::string_view text, std::size_t pos, bool crOnly) noexcept
        : text_(text), pos_(pos), crOnly_(crOnly) {}
    // false at the end of the text.
    bool next(std::string_view& line) noexcept;
    std::size_t pos() const noexcept { return pos_; }         // offset of the next line
    std::size_t lineStart() const noexcept { return start_; } // offset of the line next() returned

private:
    std::string_view text_;
    std::size_t pos_ = 0;
    std::size_t start_ = 0;
    bool crOnly_ = false;
};

// Copies text[offset, offset + out.size()) into `out`. With a file mapping (`file` non-null, `text` inside
// its view) the copy runs through MappedFile::guarded(): a page fault, or a file truncated under the mapping,
// is fail(Io) instead of a crash (02 section 1 behaviour 5, D-190). Without one it is a plain copy.
Result<void> guardedCopy(const MappedFile* file, std::string_view text, std::size_t offset,
                         std::span<char> out);

// LineCursor over a text that may be a file mapping (D-190): the lines of text[begin, end) as LineCursor
// gives them, but over a mapping the bytes are copied window by window through guardedCopy(), so a fault ends
// the iteration (next() false, error() set) instead of the process. A line longer than `maxLine` is cut to
// its first `maxLine` bytes (the rest is skipped through the guard); the copy holds one window and the line
// being read. Without a mapping it reads the text in place, exactly as LineCursor (no copy, no cut). The view
// next() returns is valid until the next call.
class GuardedLines {
public:
    static constexpr std::size_t kWindow = std::size_t{4} << 20;
    static constexpr std::size_t kMaxLine = std::size_t{16} << 20;

    GuardedLines(std::string_view text, std::size_t begin, bool crOnly, const MappedFile* file,
                 std::size_t window = kWindow, std::size_t maxLine = kMaxLine);
    bool next(std::string_view& line);
    std::size_t pos() const noexcept { return base_ + cur_; } // text offset of the next line
    std::size_t lineStart() const noexcept { return lineStart_; }
    const std::optional<Error>& error() const noexcept { return error_; }

private:
    bool refill();
    bool cutLongLine(std::string_view& line);

    std::string_view text_;
    bool crOnly_ = false;
    const MappedFile* file_ = nullptr;
    std::size_t window_ = kWindow;
    std::size_t maxLine_ = kMaxLine;
    std::vector<char> buf_;
    std::string_view view_; // the window: the text itself without a mapping, else buf_
    std::size_t base_ = 0;  // text offset of view_[0]
    std::size_t cur_ = 0;   // the next unread byte in view_
    std::size_t lineStart_ = 0;
    std::optional<Error> error_;
};

// True when a text has no '\n' in its first `probe` bytes but has a '\r' (old Mac line ends).
bool isCrOnly(std::string_view text, std::size_t probe = 65536) noexcept;
// Only spaces, tabs and '\r'.
bool isBlank(std::string_view line) noexcept;
std::string_view trimSpaces(std::string_view s) noexcept;

// ---- cells
// ---------------------------------------------------------------------------------------------------------

struct Dialect {
    char delimiter = ',';
    char decimal = '.';
    bool quoted = true; // `quote` is a quote character
    char quote = '"';
    bool trim = true;
};

struct Cell {
    std::string_view text; // without the quotes; trimmed when the dialect trims
    bool quoted = false;
    bool escaped = false; // contains a doubled quote ("" inside a quoted cell)
};

// Splits one line into cells (a delimiter inside quotes does not split; a quote that does not open the cell
// is kept as text). `out` is cleared first. A line always yields at least one cell. At most `maxCells` (at
// least 1) are produced: the scan stops after the last one, so a hostile line of delimiters costs no more
// than the cells the caller can use (a row is ragged when it has more than the header's cells, so callers
// past the header pass the header's count plus two).
void splitCells(std::string_view line, const Dialect& d, std::vector<Cell>& out,
                std::size_t maxCells = std::numeric_limits<std::size_t>::max());
// The most cells a header line may have (05 section 7.1 step 5): a longer names row is a Format error.
inline constexpr std::size_t kMaxFields = 65536;
// Whether a line ends inside a quoted field that splitCells() would open (an RFC 4180 field holding a line
// break, which this engine does not join: the text engine reads one line per row).
bool endsInOpenQuote(std::string_view line, const Dialect& d) noexcept;
// The cell's text with doubled quotes collapsed (a copy only when escaped).
std::string cellText(const Cell& c, const Dialect& d);
// Number of delimiters outside quotes (delimiter detection).
std::size_t countDelimiters(std::string_view line, char delimiter, bool quoted, char quote) noexcept;

// ---- numbers
// -------------------------------------------------------------------------------------------------------

// The whole text as a number ('.' or ',' decimal point; leading '+' accepted; surrounding spaces ignored;
// "inf" and "nan" are not numbers). nullopt otherwise.
std::optional<double> parseNumber(std::string_view s, char decimal) noexcept;
// An optionally signed run of digits only.
bool isIntegerText(std::string_view s) noexcept;
// Significant decimal digits of a number's text (leading zeros, sign, point and exponent excluded).
int significantDigits(std::string_view s) noexcept;
// "hh:mm:ss[.fff]" or "mm:ss[.fff]" in seconds; nullopt otherwise.
std::optional<double> parseClock(std::string_view s, char decimal) noexcept;
// An exact decimal text ("20.480") as a reduced rational rate; nullopt when not a positive decimal that fits.
std::optional<Rate> rateFromDecimalText(std::string_view s) noexcept;
// The rational with the smallest denominator <= 1,000 within 0.1 % of hz (05 section 7.1); nullopt when
// none (hz <= 0, not finite, or out of range).
std::optional<Rate> rationalRate(double hz) noexcept;
// Every such rational (reduced, without repeats), smallest denominator first.
std::vector<Rate> rationalRates(double hz);

// ---- regular expressions
// -------------------------------------------------------------------------------------------

// ECMAScript std::regex compiled from a profile (05 section 7.2). std::regex recurses per character on some
// engines, so every subject is cut to kMaxRegexSubject bytes and a regex_error at match time counts as "no
// match".
inline constexpr std::size_t kMaxRegexSubject = 4096;
std::optional<std::regex> compileRegex(std::string_view pattern, bool icase = false) noexcept;
// A repeated group that itself repeats inside ("(a+)+", "(\w+\s?)*", "(x|b+){2,}") unless each of its
// alternatives holds a character that must appear exactly once ("(\.[a-z]+)*"): the shape whose backtracking
// grows exponentially with the subject, which libstdc++ and libc++ neither bound nor report (D-190). Profiles
// and dictionaries refuse such patterns. A cheap syntactic check: other ambiguous shapes ("(a|a)*") pass.
bool hasNestedQuantifier(std::string_view pattern);
// The compiled form of a pattern, compiled once per process (a bounded cache, thread-safe); nullptr when the
// pattern is not a valid ECMAScript regular expression.
std::shared_ptr<const std::regex> cachedRegex(std::string_view pattern);
bool regexSearch(const std::regex& re, std::string_view subject,
                 std::match_results<const char*>* m = nullptr);
bool regexFullMatch(const std::regex& re, std::string_view subject);
// Multi-line search with '^' meaning "at a line start" on every standard library (MSVC's ECMAScript '^'
// already matches after a line end; libstdc++'s and libc++'s do not without the C++17 multiline flag, which
// not every supported library has): a search over the subject, then, for an `anchored` pattern (its text
// starts with '^'), one attempt pinned to each later line start. Linear in the subject.
bool regexSearchLines(const std::regex& re, std::string_view subject, bool anchored);

} // namespace esses::formats::text
