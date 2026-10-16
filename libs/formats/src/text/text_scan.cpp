// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "text/text_scan.hpp"

#include "ld/ld_strings.hpp"

#include <esses/base/mapped_file.hpp>
#include <esses/base/utf8.hpp>

#include <fast_float/fast_float.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <utility>
#include <vector>

namespace esses::formats::text {

std::string_view encName(Enc e) noexcept {
    switch (e) {
    case Enc::Utf8:
        return "utf-8";
    case Enc::Utf8Bom:
        return "utf-8-bom";
    case Enc::Utf16Le:
        return "utf-16le";
    case Enc::Utf16Be:
        return "utf-16be";
    case Enc::Cp1252:
        break;
    }
    return "cp1252";
}

namespace {

std::uint8_t byteAt(std::span<const std::byte> b, std::size_t i) noexcept {
    return std::to_integer<std::uint8_t>(b[i]);
}

// Length of the UTF-8 sequence starting with lead byte b (0: invalid lead).
int seqLength(std::uint8_t b) noexcept {
    if (b < 0x80) {
        return 1;
    }
    if (b >= 0xC2 && b <= 0xDF) {
        return 2;
    }
    if (b >= 0xE0 && b <= 0xEF) {
        return 3;
    }
    if (b >= 0xF0 && b <= 0xF4) {
        return 4;
    }
    return 0;
}

} // namespace

Enc detectEncoding(std::span<const std::byte> head) noexcept {
    if (head.size() >= 3 && byteAt(head, 0) == 0xEF && byteAt(head, 1) == 0xBB && byteAt(head, 2) == 0xBF) {
        return Enc::Utf8Bom;
    }
    if (head.size() >= 2 && byteAt(head, 0) == 0xFF && byteAt(head, 1) == 0xFE) {
        return Enc::Utf16Le;
    }
    if (head.size() >= 2 && byteAt(head, 0) == 0xFE && byteAt(head, 1) == 0xFF) {
        return Enc::Utf16Be;
    }
    // Valid UTF-8, tolerating a sequence the head cuts off at its end.
    std::size_t i = 0;
    while (i < head.size()) {
        const std::uint8_t b = byteAt(head, i);
        if (b < 0x80) {
            ++i;
            continue;
        }
        const int n = seqLength(b);
        if (n == 0) {
            return Enc::Cp1252;
        }
        if (i + static_cast<std::size_t>(n) > head.size()) {
            // Cut by the end of the head: what is there must still be continuation bytes.
            for (std::size_t k = i + 1; k < head.size(); ++k) {
                if ((byteAt(head, k) & 0xC0) != 0x80) {
                    return Enc::Cp1252;
                }
            }
            break;
        }
        const std::string_view seq(reinterpret_cast<const char*>(head.data()) + i,
                                   static_cast<std::size_t>(n));
        if (!utf8::isValid(seq)) {
            return Enc::Cp1252;
        }
        i += static_cast<std::size_t>(n);
    }
    return Enc::Utf8;
}

std::string utf16ToUtf8(std::span<const std::byte> bytes, bool bigEndian) {
    std::u16string u;
    u.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto a = static_cast<char16_t>(byteAt(bytes, i));
        const auto b = static_cast<char16_t>(byteAt(bytes, i + 1));
        u.push_back(bigEndian ? static_cast<char16_t>((a << 8) | b) : static_cast<char16_t>((b << 8) | a));
    }
    std::string out = utf8::fromUtf16(u);
    if (bytes.size() % 2 != 0) {
        utf8::append(out, utf8::kReplacement);
    }
    return out;
}

std::string toUtf8(std::string_view s, Enc enc) {
    if (enc != Enc::Cp1252 && utf8::isValid(s)) {
        return std::string(s);
    }
    if (utf8::isAscii(s)) {
        return std::string(s);
    }
    return ld::cp1252ToUtf8(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()));
}

// ---- lines
// ---------------------------------------------------------------------------------------------------------

bool LineCursor::next(std::string_view& line) noexcept {
    if (pos_ >= text_.size()) {
        return false;
    }
    start_ = pos_;
    const char term = crOnly_ ? '\r' : '\n';
    const void* hit = std::memchr(text_.data() + pos_, term, text_.size() - pos_);
    std::size_t end = 0;
    if (hit == nullptr) {
        end = text_.size();
        pos_ = text_.size();
    } else {
        end = static_cast<std::size_t>(static_cast<const char*>(hit) - text_.data());
        pos_ = end + 1;
    }
    std::size_t stop = end;
    if (!crOnly_ && stop > start_ && text_[stop - 1] == '\r') {
        --stop;
    }
    line = text_.substr(start_, stop - start_);
    return true;
}

namespace {

// Plain routines for MappedFile::guarded(): they own no object with a destructor.
struct CopyJob {
    char* out = nullptr;
    const char* in = nullptr;
    std::size_t n = 0;
};

void copyRoutine(void* ctx) {
    const auto* job = static_cast<const CopyJob*>(ctx);
    std::memcpy(job->out, job->in, job->n);
}

struct FindJob {
    const char* in = nullptr;
    std::size_t n = 0;
    char term = '\n';
    std::size_t found = 0; // n when absent
};

void findRoutine(void* ctx) {
    auto* job = static_cast<FindJob*>(ctx);
    const void* hit = std::memchr(job->in, job->term, job->n);
    job->found = hit == nullptr ? job->n : static_cast<std::size_t>(static_cast<const char*>(hit) - job->in);
}

} // namespace

Result<void> guardedCopy(const MappedFile* file, std::string_view text, std::size_t offset,
                         std::span<char> out) {
    if (offset > text.size() || out.size() > text.size() - offset) {
        return fail(ErrorCode::Invalid, "guardedCopy: the range is outside the text");
    }
    CopyJob job{out.data(), text.data() + offset, out.size()};
    if (job.n == 0) {
        return ok();
    }
    if (file == nullptr) {
        copyRoutine(&job);
        return ok();
    }
    return file->guarded(&copyRoutine, &job);
}

GuardedLines::GuardedLines(std::string_view text, std::size_t begin, bool crOnly, const MappedFile* file,
                           std::size_t window, std::size_t maxLine)
    : text_(text), crOnly_(crOnly), file_(file), window_(std::max<std::size_t>(window, 1)),
      maxLine_(std::max<std::size_t>(maxLine, 1)), base_(std::min(begin, text.size())) {
    if (file_ == nullptr) {
        view_ = text_.substr(base_);
    }
}

bool GuardedLines::next(std::string_view& line) {
    if (error_) {
        return false;
    }
    const char term = crOnly_ ? '\r' : '\n';
    while (true) {
        const char* data = view_.data();
        const std::size_t avail = view_.size() - cur_;
        const void* hit = avail > 0 ? std::memchr(data + cur_, term, avail) : nullptr;
        const bool atEnd = base_ + view_.size() >= text_.size();
        if (hit != nullptr || atEnd) {
            if (hit == nullptr && avail == 0) {
                return false;
            }
            const std::size_t end = hit != nullptr
                                        ? static_cast<std::size_t>(static_cast<const char*>(hit) - data)
                                        : view_.size();
            std::size_t stop = end;
            if (!crOnly_ && stop > cur_ && data[stop - 1] == '\r') {
                --stop;
            }
            line = view_.substr(cur_, stop - cur_);
            lineStart_ = base_ + cur_;
            cur_ = hit != nullptr ? end + 1 : view_.size();
            return true;
        }
        if (avail >= maxLine_) {
            return cutLongLine(line);
        }
        if (!refill()) {
            return false;
        }
    }
}

bool GuardedLines::refill() {
    // The unread tail (a line the window cut) moves to the front; the next bytes follow it, at least as many
    // as the tail holds, so a long line doubles the window instead of creeping.
    const std::size_t tail = view_.size() - cur_;
    const std::size_t from = base_ + view_.size();
    const std::size_t n = std::min(std::max(window_, tail), text_.size() - from);
    if (tail > 0) {
        std::memmove(buf_.data(), view_.data() + cur_, tail);
    }
    if (buf_.size() < tail + n) {
        buf_.resize(tail + n);
    }
    if (Result<void> r = guardedCopy(file_, text_, from, std::span<char>(buf_.data() + tail, n)); !r) {
        error_ = r.error();
        return false;
    }
    base_ = from - tail;
    cur_ = 0;
    view_ = std::string_view(buf_.data(), tail + n);
    return true;
}

bool GuardedLines::cutLongLine(std::string_view& line) {
    // The first maxLine_ bytes of the line stand for it; its terminator is found through the guard.
    const char term = crOnly_ ? '\r' : '\n';
    std::size_t from = base_ + view_.size();
    std::size_t next = text_.size();
    while (from < text_.size()) {
        FindJob job{text_.data() + from, std::min(window_, text_.size() - from), term, 0};
        if (file_ != nullptr) {
            if (Result<void> r = file_->guarded(&findRoutine, &job); !r) {
                error_ = r.error();
                return false;
            }
        } else {
            findRoutine(&job);
        }
        if (job.found < job.n) {
            next = from + job.found + 1;
            break;
        }
        from += job.n;
    }
    line = view_.substr(cur_, maxLine_);
    lineStart_ = base_ + cur_;
    // The next line starts after the terminator: an empty window there (the line's bytes stay in buf_ until
    // the next call).
    view_ = std::string_view();
    base_ = next;
    cur_ = 0;
    return true;
}

bool isCrOnly(std::string_view text, std::size_t probe) noexcept {
    const std::string_view head = text.substr(0, std::min(probe, text.size()));
    return head.find('\n') == std::string_view::npos && head.find('\r') != std::string_view::npos;
}

bool isBlank(std::string_view line) noexcept {
    for (const char c : line) {
        if (c != ' ' && c != '\t' && c != '\r') {
            return false;
        }
    }
    return true;
}

std::string_view trimSpaces(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// ---- cells
// ---------------------------------------------------------------------------------------------------------

void splitCells(std::string_view line, const Dialect& d, std::vector<Cell>& out, std::size_t maxCells) {
    out.clear();
    const std::size_t n = line.size();
    const std::size_t cap = std::max<std::size_t>(maxCells, 1);
    std::size_t i = 0;
    while (true) {
        // Leading white space before a quote does not stop the quote from opening the cell.
        std::size_t j = i;
        while (j < n && (line[j] == ' ' || line[j] == '\t') && line[j] != d.delimiter) {
            ++j;
        }
        Cell c;
        if (d.quoted && j < n && line[j] == d.quote) {
            const std::size_t open = j + 1;
            std::size_t k = open;
            bool escaped = false;
            while (k < n) {
                if (line[k] == d.quote) {
                    if (k + 1 < n && line[k + 1] == d.quote) {
                        escaped = true;
                        k += 2;
                        continue;
                    }
                    break;
                }
                ++k;
            }
            c.text = line.substr(open, std::min(k, n) - open);
            c.quoted = true;
            c.escaped = escaped;
            // Skip to the delimiter (text after the closing quote is dropped).
            std::size_t e = k < n ? k + 1 : n;
            while (e < n && line[e] != d.delimiter) {
                ++e;
            }
            if (d.trim) {
                c.text = trimSpaces(c.text);
            }
            out.push_back(c);
            if (e >= n || out.size() >= cap) {
                break;
            }
            i = e + 1;
            continue;
        }
        std::size_t e = i;
        const void* hit = std::memchr(line.data() + i, d.delimiter, n - i);
        e = hit == nullptr ? n : static_cast<std::size_t>(static_cast<const char*>(hit) - line.data());
        c.text = line.substr(i, e - i);
        if (d.trim) {
            c.text = trimSpaces(c.text);
        }
        out.push_back(c);
        if (e >= n || out.size() >= cap) {
            break;
        }
        i = e + 1;
    }
}

bool endsInOpenQuote(std::string_view line, const Dialect& d) noexcept {
    if (!d.quoted) {
        return false;
    }
    // The quote rules of splitCells(): a quote opens a field only at its start (after spaces and tabs), a
    // doubled quote inside it is a quote, and text after the closing quote runs to the delimiter.
    const std::size_t n = line.size();
    std::size_t i = 0;
    while (true) {
        std::size_t j = i;
        while (j < n && (line[j] == ' ' || line[j] == '\t') && line[j] != d.delimiter) {
            ++j;
        }
        if (j < n && line[j] == d.quote) {
            std::size_t k = j + 1;
            bool closed = false;
            while (k < n) {
                if (line[k] == d.quote) {
                    if (k + 1 < n && line[k + 1] == d.quote) {
                        k += 2;
                        continue;
                    }
                    closed = true;
                    break;
                }
                ++k;
            }
            if (!closed) {
                return true;
            }
            i = k + 1;
        }
        const void* hit = i < n ? std::memchr(line.data() + i, d.delimiter, n - i) : nullptr;
        if (hit == nullptr) {
            return false;
        }
        i = static_cast<std::size_t>(static_cast<const char*>(hit) - line.data()) + 1;
    }
}

std::string cellText(const Cell& c, const Dialect& d) {
    if (!c.escaped) {
        return std::string(c.text);
    }
    std::string s;
    s.reserve(c.text.size());
    for (std::size_t i = 0; i < c.text.size(); ++i) {
        s.push_back(c.text[i]);
        if (c.text[i] == d.quote && i + 1 < c.text.size() && c.text[i + 1] == d.quote) {
            ++i;
        }
    }
    return s;
}

std::size_t countDelimiters(std::string_view line, char delimiter, bool quoted, char quote) noexcept {
    std::size_t count = 0;
    bool inQuote = false;
    for (const char c : line) {
        if (quoted && c == quote) {
            inQuote = !inQuote;
        } else if (c == delimiter && !inQuote) {
            ++count;
        }
    }
    return count;
}

// ---- numbers
// -------------------------------------------------------------------------------------------------------

std::optional<double> parseNumber(std::string_view s, char decimal) noexcept {
    s = trimSpaces(s);
    if (s.empty()) {
        return std::nullopt;
    }
    if (s.front() == '+') {
        s.remove_prefix(1);
        if (s.empty() || s.front() == '-' || s.front() == '+') {
            return std::nullopt;
        }
    }
    double v = 0.0;
    const auto format = static_cast<fast_float::chars_format>(fast_float::chars_format::general |
                                                              fast_float::chars_format::no_infnan);
    const fast_float::parse_options options(format, decimal);
    const auto r = fast_float::from_chars_advanced(s.data(), s.data() + s.size(), v, options);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    if (!std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

bool isIntegerText(std::string_view s) noexcept {
    s = trimSpaces(s);
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

int significantDigits(std::string_view s) noexcept {
    s = trimSpaces(s);
    int digits = 0;
    bool leading = true;
    for (const char c : s) {
        if (c == 'e' || c == 'E') {
            break;
        }
        if (c < '0' || c > '9') {
            continue;
        }
        if (leading && c == '0') {
            continue;
        }
        leading = false;
        ++digits;
    }
    // Trailing zeros of the fraction part are printed precision, not information; keep them (conservative).
    return digits;
}

std::optional<double> parseClock(std::string_view s, char decimal) noexcept {
    s = trimSpaces(s);
    const std::size_t c1 = s.find(':');
    if (c1 == std::string_view::npos || c1 == 0) {
        return std::nullopt;
    }
    const std::size_t c2 = s.find(':', c1 + 1);
    std::string_view hh;
    std::string_view mm;
    std::string_view ss;
    if (c2 == std::string_view::npos) {
        mm = s.substr(0, c1);
        ss = s.substr(c1 + 1);
    } else {
        hh = s.substr(0, c1);
        mm = s.substr(c1 + 1, c2 - c1 - 1);
        ss = s.substr(c2 + 1);
    }
    if ((!hh.empty() && !isIntegerText(hh)) || !isIntegerText(mm)) {
        return std::nullopt;
    }
    const auto h = hh.empty() ? std::optional<double>(0.0) : parseNumber(hh, decimal);
    const auto m = parseNumber(mm, decimal);
    const auto sec = parseNumber(ss, decimal);
    if (!h || !m || !sec || *h < 0 || *m < 0 || *m >= 60 || *sec < 0 || *sec >= 61) {
        return std::nullopt;
    }
    return *h * 3600.0 + *m * 60.0 + *sec;
}

std::optional<Rate> rateFromDecimalText(std::string_view s) noexcept {
    s = trimSpaces(s);
    std::uint64_t num = 0;
    std::uint64_t den = 1;
    bool point = false;
    bool digits = false;
    for (const char c : s) {
        if (c == '.' || c == ',') {
            if (point) {
                return std::nullopt;
            }
            point = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        digits = true;
        if (num > (std::numeric_limits<std::uint64_t>::max() - 9) / 10) {
            return std::nullopt;
        }
        num = num * 10 + static_cast<std::uint64_t>(c - '0');
        if (point) {
            if (den > std::numeric_limits<std::uint64_t>::max() / 10) {
                return std::nullopt;
            }
            den *= 10;
        }
    }
    if (!digits || num == 0) {
        return std::nullopt;
    }
    const std::uint64_t g = std::gcd(num, den);
    num /= g;
    den /= g;
    if (num > std::numeric_limits<std::uint32_t>::max() || den > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return Rate{static_cast<std::uint32_t>(num), static_cast<std::uint32_t>(den)};
}

std::optional<Rate> rationalRate(double hz) noexcept {
    if (!std::isfinite(hz) || hz <= 0.0 || hz > 4.0e9) {
        return std::nullopt;
    }
    for (std::uint32_t den = 1; den <= 1000; ++den) {
        const double n = std::round(hz * static_cast<double>(den));
        if (n < 1.0 || n > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            continue;
        }
        const double candidate = n / static_cast<double>(den);
        if (std::abs(candidate - hz) <= 0.001 * hz) {
            return Rate{static_cast<std::uint32_t>(n), den}.normalised();
        }
    }
    return std::nullopt;
}

std::vector<Rate> rationalRates(double hz) {
    std::vector<Rate> out;
    if (!std::isfinite(hz) || hz <= 0.0 || hz > 4.0e9) {
        return out;
    }
    for (std::uint32_t den = 1; den <= 1000; ++den) {
        const double n = std::round(hz * static_cast<double>(den));
        if (n < 1.0 || n > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            continue;
        }
        if (std::abs(n / static_cast<double>(den) - hz) > 0.001 * hz) {
            continue;
        }
        const Rate r = Rate{static_cast<std::uint32_t>(n), den}.normalised();
        if (r.den == den) { // a reduced fraction appears first at its own denominator
            out.push_back(r);
        }
    }
    return out;
}

// ---- regular expressions
// -------------------------------------------------------------------------------------------

std::optional<std::regex> compileRegex(std::string_view pattern, bool icase) noexcept {
    try {
        auto flags = std::regex::ECMAScript;
        if (icase) {
            flags |= std::regex::icase;
        }
        return std::regex(pattern.begin(), pattern.end(), flags);
    } catch (...) { // regex_error, bad_alloc
        return std::nullopt;
    }
}

namespace {

struct Quantifier {
    bool valid = false;   // '*', '+', '?' or a well-formed {n}, {n,}, {n,m}
    bool repeats = false; // more than once
    std::size_t end = 0;  // the index after it, a lazy '?' included
};

Quantifier quantifierAt(std::string_view p, std::size_t i) noexcept {
    Quantifier q;
    if (i >= p.size()) {
        return q;
    }
    const char c = p[i];
    q.end = i + 1;
    if (c == '*' || c == '+' || c == '?') {
        q.valid = true;
        q.repeats = c != '?';
    } else if (c == '{') {
        const std::size_t close = p.find('}', i);
        if (close == std::string_view::npos) {
            return q; // a literal '{'
        }
        const std::string_view body = p.substr(i + 1, close - i - 1);
        const auto digits = [](std::string_view d) {
            return !d.empty() && std::all_of(d.begin(), d.end(), [](char x) { return x >= '0' && x <= '9'; });
        };
        const std::size_t comma = body.find(',');
        if (comma == std::string_view::npos) {
            if (!digits(body)) {
                return q;
            }
            q.repeats = body.size() > 1 || body[0] > '1'; // {n}, n >= 2
        } else {
            const std::string_view lo = body.substr(0, comma);
            const std::string_view hi = body.substr(comma + 1);
            if (!digits(lo) || (!hi.empty() && !digits(hi))) {
                return q;
            }
            q.repeats = hi.empty() || hi.size() > 1 || hi[0] > '1'; // {n,}, or {n,m} with m >= 2
        }
        q.valid = true;
        q.end = close + 1;
    } else {
        return q;
    }
    if (q.end < p.size() && p[q.end] == '?') {
        ++q.end; // lazy
    }
    return q;
}

} // namespace

bool hasNestedQuantifier(std::string_view p) {
    // Per open group: whether an element inside it repeats, and whether each of its alternatives holds a
    // character that must appear exactly once (a separator such as "\." in "(\.[a-z]+)*", which keeps the
    // iterations from trading characters, so its repetition is no danger).
    struct Group {
        bool repeats = false;
        bool branchLiteral = false;
        bool allLiteral = true;
    };
    std::vector<Group> groups;
    std::size_t i = 0;
    while (i < p.size()) {
        const char c = p[i];
        if (c == '(') {
            groups.emplace_back();
            ++i;
            if (i < p.size() && p[i] == '?') { // (?: (?= (?!
                ++i;
                if (i < p.size() && (p[i] == ':' || p[i] == '=' || p[i] == '!')) {
                    ++i;
                }
            }
            continue;
        }
        if (c == '|') {
            if (!groups.empty()) {
                Group& g = groups.back();
                g.allLiteral = g.allLiteral && g.branchLiteral;
                g.branchLiteral = false;
            }
            ++i;
            continue;
        }
        if (c == '*' || c == '+' || c == '?') {
            ++i; // a quantifier with nothing to repeat: not a valid pattern, nothing to judge
            continue;
        }
        // One atom: the end of a group, a class, an escape or a character; then its quantifier.
        bool literal = false;
        bool innerRepeats = false;
        bool dangerous = false;
        if (c == ')') {
            ++i;
            if (groups.empty()) {
                continue;
            }
            Group g = groups.back();
            groups.pop_back();
            g.allLiteral = g.allLiteral && g.branchLiteral;
            innerRepeats = g.repeats;
            dangerous = g.repeats && !g.allLiteral;
        } else if (c == '[') {
            std::size_t j = i + 1;
            if (j < p.size() && p[j] == '^') {
                ++j;
            }
            if (j < p.size() && p[j] == ']') {
                ++j; // a ']' first in a class is literal
            }
            while (j < p.size() && p[j] != ']') {
                j += p[j] == '\\' ? 2 : 1;
            }
            i = j + 1;
        } else if (c == '\\') {
            const char e = i + 1 < p.size() ? p[i + 1] : '\0';
            const bool classEscape = (e >= '0' && e <= '9') || e == 'd' || e == 'D' || e == 'w' || e == 'W' ||
                                     e == 's' || e == 'S' || e == 'b' || e == 'B';
            literal = !classEscape;
            i += 2;
        } else {
            literal = c != '.' && c != '^' && c != '$';
            ++i;
        }
        const Quantifier q = quantifierAt(p, i);
        if (q.valid) {
            i = q.end;
        }
        if (q.repeats && dangerous) {
            return true;
        }
        if (!groups.empty()) {
            Group& g = groups.back();
            g.repeats = g.repeats || q.repeats || innerRepeats;
            g.branchLiteral = g.branchLiteral || (literal && !q.valid);
        }
    }
    return false;
}

std::shared_ptr<const std::regex> cachedRegex(std::string_view pattern) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const std::regex>, std::less<>> cache;
    constexpr std::size_t kMaxCached = 512;
    const std::lock_guard lock(mutex);
    if (const auto it = cache.find(pattern); it != cache.end()) {
        return it->second;
    }
    std::shared_ptr<const std::regex> compiled;
    if (auto re = compileRegex(pattern)) {
        compiled = std::make_shared<const std::regex>(std::move(*re));
    }
    if (cache.size() >= kMaxCached) {
        cache.clear(); // patterns come from the loaded dictionaries: a full cache is a rare reset
    }
    cache.emplace(std::string(pattern), compiled);
    return compiled;
}

bool regexSearch(const std::regex& re, std::string_view subject, std::match_results<const char*>* m) {
    subject = subject.substr(0, std::min(subject.size(), kMaxRegexSubject));
    try {
        if (m != nullptr) {
            return std::regex_search(subject.data(), subject.data() + subject.size(), *m, re);
        }
        return std::regex_search(subject.data(), subject.data() + subject.size(), re);
    } catch (...) { // error_complexity, error_stack
        return false;
    }
}

bool regexSearchLines(const std::regex& re, std::string_view subject, bool anchored) {
    subject = subject.substr(0, std::min(subject.size(), kMaxRegexSubject));
    if (regexSearch(re, subject)) {
        return true; // anywhere (MSVC's '^' already matches at every line start)
    }
    if (!anchored) {
        return false;
    }
    // A pattern that starts with '^': retry at each later line start, the match pinned to it (linear
    // overall).
    constexpr char kCr = 13;
    constexpr char kLf = 10;
    std::size_t start = 0;
    while (true) {
        std::size_t nl = start;
        while (nl < subject.size() && subject[nl] != kCr && subject[nl] != kLf) {
            ++nl;
        }
        if (nl >= subject.size()) {
            return false;
        }
        start = nl + 1;
        if (start < subject.size() && subject[nl] == kCr && subject[start] == kLf) {
            ++start;
        }
        const std::string_view rest = subject.substr(start);
        try {
            if (std::regex_search(rest.data(), rest.data() + rest.size(), re,
                                  std::regex_constants::match_continuous)) {
                return true;
            }
        } catch (...) { // error_complexity, error_stack: no match at this line start
            continue;
        }
    }
}

bool regexFullMatch(const std::regex& re, std::string_view subject) {
    if (subject.size() > kMaxRegexSubject) {
        return false;
    }
    try {
        return std::regex_match(subject.data(), subject.data() + subject.size(), re);
    } catch (...) {
        return false;
    }
}

} // namespace esses::formats::text
