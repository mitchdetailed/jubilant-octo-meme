// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Pass 1 of the TextEngine (05 section 7.1): encoding, line index, preamble, header, columns, time and rate,
// row sets with their chunk starts, enumeration tables, lap sources, and the ProbeResult. Deterministic: the
// same bytes and profile give the same ProbeResult on every platform.
#include "text/text_layout.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/mapped_file.hpp>
#include <esses/model/marker.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <set>
#include <utility>

namespace esses::formats::text {
namespace {

using Level = ImportLogEntry::Level;

constexpr std::size_t kHeadBytes = std::size_t{1} << 20; // header search window
constexpr std::size_t kHeadLines = 4096;
constexpr std::size_t kClassifyRows = 256;
constexpr std::uint64_t kSampleEvery = 1000;
constexpr std::size_t kMaxEnumLabels = 65536;
constexpr std::size_t kCarryWindow = std::size_t{64} << 10; // computeCarryIns' first backward window
// The @auto header rule's "numeric" data row: 05 section 7.3 says 80 %; text columns (one in three) are
// common enough that half is used (docs/formats/text.md).
constexpr double kNumericRow = 0.5;

struct HeadLine {
    std::string_view text;
    std::uint64_t offset = 0;
    std::uint64_t line = 0; // 1-based
    bool blank = false;
};

struct SampleRow {
    std::uint64_t row = 0;
    std::string text; // a copy: pass 1 reads a file mapping through guarded windows (D-190)
};

// The sample rows kept for classification and the rate search hold at most this many bytes in all (a hostile
// file of huge rows samples fewer rows, never more memory).
constexpr std::size_t kSampleBudget = std::size_t{32} << 20;

// A time sample rule (02 section 3.3 rule 12, see text_layout.hpp): later times are kept, an equal time
// replaces the pending sample, an earlier one is dropped.
struct Machine {
    bool pending = false;
    Micros pendingTime = 0;
    std::uint64_t pendingOffset = 0;
    std::uint64_t pendingLine = 0;
    bool emittedAny = false;
    Micros lastEmitted = 0;
    std::uint64_t emitted = 0;
    std::vector<ChunkStart> chunks;
    Micros first = 0;
    Micros last = 0;
    std::uint64_t dropped = 0;
    std::uint64_t duplicates = 0;

    void emit() {
        if (emitted % store::kChunkSamples == 0) {
            ChunkStart c;
            c.offset = pendingOffset;
            c.line = pendingLine;
            c.prevTime = emittedAny ? lastEmitted : std::numeric_limits<Micros>::min();
            chunks.push_back(std::move(c));
        }
        if (!emittedAny) {
            first = pendingTime;
        }
        last = pendingTime;
        lastEmitted = pendingTime;
        emittedAny = true;
        ++emitted;
        pending = false;
    }
    void candidate(std::uint64_t offset, std::uint64_t line, Micros t) {
        if (!pending) {
            if (emittedAny && t <= lastEmitted) {
                ++dropped;
                return;
            }
        } else if (t > pendingTime) {
            emit();
        } else if (t == pendingTime) {
            ++duplicates;
        } else {
            ++dropped;
            return;
        }
        pending = true;
        pendingTime = t;
        pendingOffset = offset;
        pendingLine = line;
    }
    void finish() {
        if (pending) {
            emit();
        }
    }
};

std::string_view delimiterName(char d) {
    switch (d) {
    case '\t':
        return "tab";
    case ';':
        return "semicolon";
    case '|':
        return "pipe";
    case ' ':
        return "space";
    default:
        break;
    }
    return "comma";
}

bool isSentinel(std::string_view cell, const std::vector<std::string>& sentinels) {
    for (const std::string& s : sentinels) {
        if (cell == s) {
            return true;
        }
    }
    return false;
}

int fractionDigits(std::string_view s, char decimal) {
    s = trimSpaces(s);
    const std::size_t p = s.find(decimal);
    if (p == std::string_view::npos) {
        return 0;
    }
    int n = 0;
    for (std::size_t i = p + 1; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        ++n;
    }
    return n;
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// ^(time|t|elapsed.*|secs?|timestamp)$, case-insensitive (05 section 7.3, the @auto time column), without
// std::regex.
bool isAutoTimeName(std::string_view n) {
    const std::string l = lowerAscii(n);
    return l == "time" || l == "t" || l == "sec" || l == "secs" || l == "timestamp" ||
           l.starts_with("elapsed");
}

double numericFraction(const std::vector<Cell>& cells, char delimiter) {
    std::size_t n = 0;
    std::size_t numeric = 0;
    for (const Cell& c : cells) {
        if (c.text.empty()) {
            continue;
        }
        ++n;
        if (parseNumber(c.text, '.') || (delimiter != ',' && parseNumber(c.text, ','))) {
            ++numeric;
        }
    }
    return n == 0 ? 0.0 : static_cast<double>(numeric) / static_cast<double>(n);
}

bool hasNumericCell(const std::vector<Cell>& cells, char delimiter) {
    for (const Cell& c : cells) {
        if (!c.text.empty() && (parseNumber(c.text, '.') || (delimiter != ',' && parseNumber(c.text, ',')))) {
            return true;
        }
    }
    return false;
}

// The previous line ending before `end` (exclusive): its start offset; `end` itself excluded.
std::size_t prevLineStart(std::string_view text, std::size_t end, char term) {
    if (end == 0) {
        return 0;
    }
    std::size_t i = end - 1; // the terminator of the previous line (or its last byte)
    if (text[i] == term) {
        if (i == 0) {
            return 0;
        }
        --i;
    }
    while (i > 0 && text[i - 1] != term) {
        --i;
    }
    return i;
}

class Analyser {
public:
    Analyser(const TextProfile& p, std::span<const std::byte> bytes, const MappedFile* mapped,
             const CancelToken& cancel)
        : p_(p), bytes_(bytes), mapped_(mapped), cancel_(cancel), L_(std::make_shared<Layout>()) {}

    Result<AnalyseResult> run(std::string_view importerId, std::string_view importerVersion);

private:
    std::string utf8(std::string_view s) const { return toUtf8(s, L_->enc); }
    // The cells a row is split into after the header: the header's plus two, enough to tell a ragged row
    // (and a trailing delimiter's empty cell) without allocating a cell per delimiter of a hostile line.
    std::size_t rowCap() const noexcept { return L_->headerFields + 2; }
    // The mapping L_->text lies in (null when the text is a transcoded copy or the source is memory).
    const MappedFile* textMapping() const noexcept { return L_->owned.empty() ? mapped_ : nullptr; }
    std::string cellUtf8(const Cell& c) const { return utf8(cellText(c, L_->dialect)); }
    void info(std::string_view codeId, std::string message, std::optional<std::string> detail = {},
              std::optional<std::string> channel = {}) {
        ImportLogEntry::Context ctx;
        ctx.detail = std::move(detail);
        ctx.channel = std::move(channel);
        log_.add(Level::Info, std::string(codeId), std::move(message), std::move(ctx));
    }
    void warn(std::string_view codeId, std::string message, std::optional<std::string> detail = {},
              std::optional<std::string> channel = {}) {
        ImportLogEntry::Context ctx;
        ctx.detail = std::move(detail);
        ctx.channel = std::move(channel);
        log_.add(Level::Warning, std::string(codeId), std::move(message), std::move(ctx));
    }

    void decodeText();
    void readHeadLines();
    void chooseDelimiter();
    void checkQuotedNewlines();
    Result<void> findHeader();
    void readPreamble(std::size_t end);
    // The names row (and units row) of a file that has a header and no data rows, for the @auto rule.
    struct HeaderOnly {
        std::size_t names = 0;
        std::optional<std::size_t> units;
    };
    std::optional<HeaderOnly> headerOnlyRows();
    Result<void> buildColumns();
    void chooseTimeColumn();
    void scanRows();
    void chooseDecimal();
    void classifyColumns();
    void resolveTime();
    Result<void> fullScan();
    void computeCarryIns();
    std::optional<Micros> timeOf(std::string_view line);
    std::optional<double> rawTime(const Cell& cell) const;

    const TextProfile& p_;
    std::span<const std::byte> bytes_;
    const MappedFile* mapped_ = nullptr; // the mapping behind bytes_ (null for memory): reads are guarded
    std::optional<Error> readError_;     // a fault while reading the mapping (the file changed): fail(Io)
    std::deque<std::string> headStore_;  // the head lines' bytes (head_ views them)
    std::string headBytes_;              // the first 64 KiB of the file, copied through the guard
    const CancelToken& cancel_;
    std::shared_ptr<Layout> L_;
    ImportLog log_;
    std::vector<HeadLine> head_;
    std::uint64_t longLine_ = 0; // 1-based line number of a line longer than kHeadBytes that ended the head
    bool longFirstLine_ = false; // ... and nothing but blank lines came before it
    bool headComplete_ = false;  // the head holds every line of the text
    std::size_t namesLine_ = 0;  // index into head_
    std::optional<std::size_t> unitsLine_;
    std::size_t dataLineIx_ = 0;
    std::vector<model::Detail> details_;
    std::map<std::string, std::pair<std::string, std::string>, std::less<>> inputs_; // @input -> value, unit
    std::vector<SampleRow> samples_;
    std::vector<Cell> cells_;
    std::vector<std::string> names_;
    std::vector<std::string> units_;
    std::vector<std::uint32_t> fields_; // field of each kept name
    std::string timeName_;
    std::string timeVendorUnit_;
    std::int32_t lapField_ = -1;
    std::vector<char> carryBuf_; // the window computeCarryIns reads backwards
    std::vector<std::pair<std::uint32_t, std::regex>> beaconMatch_;
    bool beaconEvent_ = false;
    std::vector<MarkerInfo> markers_;
    std::optional<UtcInstant> utc_;
};

void Analyser::decodeText() {
    const std::string_view all(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
    headBytes_.resize(std::min<std::size_t>(bytes_.size(), 65536));
    if (Result<void> r = guardedCopy(mapped_, all, 0, headBytes_); !r) {
        readError_ = r.error();
        return;
    }
    const std::span<const std::byte> head(reinterpret_cast<const std::byte*>(headBytes_.data()),
                                          headBytes_.size());
    Enc enc = Enc::Utf8;
    switch (p_.encoding) {
    case TextProfile::Encoding::Auto:
        enc = detectEncoding(head);
        break;
    case TextProfile::Encoding::Utf8:
        enc = detectEncoding(head) == Enc::Utf8Bom ? Enc::Utf8Bom : Enc::Utf8;
        break;
    case TextProfile::Encoding::Utf16Le:
        enc = Enc::Utf16Le;
        break;
    case TextProfile::Encoding::Utf16Be:
        enc = Enc::Utf16Be;
        break;
    case TextProfile::Encoding::Cp1252:
        enc = Enc::Cp1252;
        break;
    }
    const auto byteAt = [&](std::size_t i) { return std::to_integer<std::uint8_t>(head[i]); };
    if (enc == Enc::Utf16Le || enc == Enc::Utf16Be) {
        const bool bom = head.size() >= 2 && ((byteAt(0) == 0xFF && byteAt(1) == 0xFE) ||
                                              (byteAt(0) == 0xFE && byteAt(1) == 0xFF));
        // Transcoded from a guarded copy of the file (a mapping is never read unguarded, D-190).
        std::vector<std::byte> copy;
        std::span<const std::byte> whole = bytes_;
        if (mapped_ != nullptr) {
            copy.resize(bytes_.size());
            if (Result<void> r = guardedCopy(
                    mapped_, all, 0, std::span<char>(reinterpret_cast<char*>(copy.data()), copy.size()));
                !r) {
                readError_ = r.error();
                return;
            }
            whole = copy;
        }
        std::span<const std::byte> body = whole.subspan(bom ? 2 : 0);
        // A truncated UTF-16 file: its lone last byte is no character, and as U+FFFD it would make a
        // non-blank last line (an extra data row).
        const bool odd = body.size() % 2 != 0;
        if (odd) {
            body = body.first(body.size() - 1);
        }
        L_->owned = utf16ToUtf8(body, enc == Enc::Utf16Be);
        L_->text = L_->owned;
        L_->textBase = bom ? 2 : 0;
        L_->enc = Enc::Utf8; // the transcoded text is UTF-8
        info(code::kEncoding, "UTF-16 text transcoded to UTF-8 at open", std::string(encName(enc)));
        if (odd) {
            warn(code::kEncoding, "the UTF-16 text has an odd number of bytes; the last byte is dropped",
                 "odd byte count");
        }
        return;
    }
    std::string_view text(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
    if (enc == Enc::Utf8Bom && text.size() >= 3) {
        text.remove_prefix(3);
        L_->textBase = 3;
    }
    L_->text = text;
    L_->enc = enc;
    if (enc != Enc::Utf8) {
        info(code::kEncoding, "text encoding", std::string(encName(enc)));
    }
}

void Analyser::readHeadLines() {
    // The line ends from the first 64 KiB of the text (the guarded head copy, or the transcoded text).
    const std::string_view probe =
        L_->owned.empty()
            ? std::string_view(headBytes_).substr(std::min<std::size_t>(L_->textBase, headBytes_.size()))
            : std::string_view(L_->text);
    L_->crOnly = isCrOnly(probe);
    GuardedLines cur(L_->text, 0, L_->crOnly, textMapping());
    std::string_view line;
    std::uint64_t n = 0;
    while (head_.size() < kHeadLines && cur.pos() < kHeadBytes && cur.next(line)) {
        if (line.size() > kHeadBytes) {
            // No header or data row is this long: the head ends before it (05 section 9.3, bounded work on
            // hostile input), and a file that starts with it is not a table.
            longLine_ = n + 1;
            longFirstLine_ =
                std::all_of(head_.begin(), head_.end(), [](const HeadLine& x) { return x.blank; });
            break;
        }
        HeadLine h;
        h.text = headStore_.emplace_back(line);
        h.offset = cur.lineStart();
        h.line = ++n;
        h.blank = isBlank(line);
        head_.push_back(h);
    }
    if (cur.error()) {
        readError_ = *cur.error();
    }
    headComplete_ = longLine_ == 0 && !readError_ && cur.pos() >= L_->text.size();
}

void Analyser::chooseDelimiter() {
    Dialect& d = L_->dialect;
    d.quoted = p_.quote.has_value();
    d.quote = p_.quote.value_or('"');
    d.trim = p_.trim;
    if (p_.delimiter != "auto") {
        d.delimiter = p_.delimiter[0];
        return;
    }
    // The last (up to) 50 non-blank lines of the head are data rows in every file worth opening.
    std::vector<std::string_view> sample;
    for (auto it = head_.rbegin(); it != head_.rend() && sample.size() < 50; ++it) {
        if (!it->blank) {
            sample.push_back(it->text);
        }
    }
    char chosen = 0;
    for (const char cand : {'\t', ';', ',', '|', ' '}) {
        if (sample.empty()) {
            break;
        }
        const std::size_t first = countDelimiters(sample.front(), cand, d.quoted, d.quote);
        bool constant = first > 0;
        for (const std::string_view s : sample) {
            if (!constant) {
                break;
            }
            constant = countDelimiters(s, cand, d.quoted, d.quote) == first;
        }
        if (constant) {
            chosen = cand;
            break;
        }
    }
    if (chosen == 0) {
        // No constant count: the candidate with the largest count on the first sampled line, else ','.
        std::size_t best = 0;
        chosen = ',';
        for (const char cand : {'\t', ';', ',', '|'}) {
            const std::size_t n =
                sample.empty() ? 0 : countDelimiters(sample.front(), cand, d.quoted, d.quote);
            if (n > best) {
                best = n;
                chosen = cand;
            }
        }
    }
    d.delimiter = chosen;
    info(code::kAutoDelimiter, "delimiter detected", std::string(delimiterName(chosen)));
}

void Analyser::checkQuotedNewlines() {
    // RFC 4180 lets a quoted field hold a line break; this engine reads one line per row, so such a field
    // splits its row (a names row loses its later columns, data rows turn ragged). Said once, for the head
    // (05 section 7.1 step 2 as built: quoted fields do not span lines; D-190).
    std::uint64_t first = 0;
    std::uint32_t count = 0;
    for (const HeadLine& h : head_) {
        if (!h.blank && endsInOpenQuote(h.text, L_->dialect)) {
            first = first == 0 ? h.line : first;
            ++count;
        }
    }
    if (count == 0) {
        return;
    }
    ImportLogEntry::Context ctx;
    ctx.line = first;
    ctx.repeats = count;
    ctx.detail = "line " + std::to_string(first);
    log_.add(Level::Warning, std::string(code::kQuotedNewline),
             "a quoted field holds a line break, which ends the row here; the fields after it shift",
             std::move(ctx));
}

void Analyser::readPreamble(std::size_t end) {
    std::vector<std::string> unmatched;
    const bool keyValue = !p_.preamble.keyValueColumns.empty();
    std::vector<std::optional<std::regex>> regexes;
    std::vector<bool> taken(p_.preamble.capture.size(), false);
    if (!keyValue) {
        for (const auto& [target, pattern] : p_.preamble.capture) {
            regexes.push_back(compileRegex(pattern));
        }
    }
    const auto store = [&](const std::string& target, std::string value, std::string unit) {
        if (value.empty()) {
            return; // an empty value is no detail (and no engine input)
        }
        if (target.starts_with('@')) {
            inputs_[target.substr(1)] = {std::move(value), std::move(unit)};
            return;
        }
        model::Detail d;
        d.id = target;
        d.prov = model::Provenance::Logged;
        const auto num = unit.empty() ? std::nullopt : parseNumber(value, '.');
        if (num) {
            model::Quantityd q;
            q.v = *num;
            q.symbol = unit;
            q.dps = fractionDigits(value, '.');
            d.value = q;
        } else {
            d.value = std::move(value);
        }
        details_.push_back(std::move(d));
    };
    for (std::size_t i = 0; i < end && i < head_.size(); ++i) {
        const HeadLine& h = head_[i];
        if (h.blank) {
            continue;
        }
        if (keyValue) {
            splitCells(h.text, L_->dialect, cells_, kMaxFields + 1);
            for (const auto& kv : p_.preamble.keyValueColumns) {
                const auto at = [&](int ix) -> std::string {
                    return ix >= 0 && static_cast<std::size_t>(ix) < cells_.size()
                               ? cellUtf8(cells_[static_cast<std::size_t>(ix)])
                               : std::string();
                };
                std::string key = at(kv[0]);
                if (key.empty()) {
                    continue;
                }
                std::string value = at(kv[1]);
                std::string unit = at(kv[2]);
                const auto it = std::find_if(p_.preamble.capture.begin(), p_.preamble.capture.end(),
                                             [&](const auto& c) { return c.first == key; });
                if (it != p_.preamble.capture.end()) {
                    store(it->second, std::move(value), std::move(unit));
                } else if (!p_.preamble.unmatchedTo.empty()) {
                    std::string line = key;
                    line.append(": ").append(value);
                    if (!unit.empty()) {
                        line.append(" ").append(unit);
                    }
                    unmatched.push_back(std::move(line));
                }
            }
            continue;
        }
        const std::string line = utf8(h.text);
        bool matched = false;
        for (std::size_t k = 0; k < p_.preamble.capture.size(); ++k) {
            if (taken[k] || !regexes[k]) {
                continue;
            }
            std::match_results<const char*> m;
            if (regexSearch(*regexes[k], line, &m)) {
                taken[k] = true;
                matched = true;
                std::string value = m.size() > 1 ? m[1].str() : m[0].str();
                store(p_.preamble.capture[k].first, std::move(value), {});
            }
        }
        if (!matched && !p_.preamble.unmatchedTo.empty()) {
            splitCells(h.text, L_->dialect, cells_, kMaxFields + 1);
            std::string joined;
            for (const Cell& c : cells_) {
                if (!c.text.empty()) {
                    joined += (joined.empty() ? "" : " ") + cellUtf8(c);
                }
            }
            if (!joined.empty()) {
                unmatched.push_back(std::move(joined));
            }
        }
    }
    if (!unmatched.empty()) {
        std::string text;
        for (const std::string& u : unmatched) {
            text += (text.empty() ? "" : "\n") + u;
        }
        model::Detail d;
        d.id = p_.preamble.unmatchedTo;
        d.value = std::move(text);
        details_.push_back(std::move(d));
    }
}

std::optional<Analyser::HeaderOnly> Analyser::headerOnlyRows() {
    // Only a file whose every line is in the head: the last non-blank line, at least two fields and less
    // than half numeric, is the names row; when it is units-like (no number, every cell at most 12
    // characters) under a line of as many non-numeric fields and the profile looks for a units row, the line
    // above it is the names row and it the units row. The names row must name the time column (the profile's,
    // or one the @auto rule takes by name), so a line of prose is no header; and no line of the file may look
    // like a data row of that header (as many fields, at least half numeric): then the @auto rule failed for
    // another reason.
    if (!headComplete_) {
        return std::nullopt;
    }
    std::vector<std::size_t> lines;
    for (std::size_t i = 0; i < head_.size(); ++i) {
        if (!head_[i].blank) {
            lines.push_back(i);
        }
    }
    if (lines.empty()) {
        return std::nullopt;
    }
    const char delimiter = L_->dialect.delimiter;
    const auto close = [](std::size_t n, std::size_t fields) { return n + 1 >= fields && n <= fields + 1; };
    splitCells(head_[lines.back()].text, L_->dialect, cells_, kMaxFields + 1);
    const std::size_t fields = cells_.size();
    if (fields < 2 || fields > kMaxFields || numericFraction(cells_, delimiter) >= 0.5) {
        return std::nullopt;
    }
    HeaderOnly out;
    out.names = lines.back();
    const bool unitsLike =
        !hasNumericCell(cells_, delimiter) &&
        std::all_of(cells_.begin(), cells_.end(), [](const Cell& c) { return c.text.size() <= 12; });
    if (unitsLike && lines.size() >= 2 && p_.header.unitsRow == "@auto") {
        splitCells(head_[lines[lines.size() - 2]].text, L_->dialect, cells_, kMaxFields + 1);
        if (cells_.size() >= 2 && close(cells_.size(), fields) && numericFraction(cells_, delimiter) < 0.5) {
            out.units = out.names;
            out.names = lines[lines.size() - 2];
        }
    }
    splitCells(head_[out.names].text, L_->dialect, cells_, kMaxFields + 1);
    const std::string& wanted = p_.time.column;
    const bool namesTime = std::any_of(cells_.begin(), cells_.end(), [&](const Cell& c) {
        const std::string name = cellUtf8(c);
        return wanted == "@auto" || wanted.empty() ? isAutoTimeName(name) : ciEquals(name, wanted);
    });
    if (!namesTime) {
        return std::nullopt;
    }
    for (const std::size_t i : lines) {
        splitCells(head_[i].text, L_->dialect, cells_, kMaxFields + 1);
        if (close(cells_.size(), fields) && numericFraction(cells_, delimiter) >= kNumericRow) {
            return std::nullopt;
        }
    }
    return out;
}

Result<void> Analyser::findHeader() {
    std::size_t preambleEnd = 0; // index of the first line after the preamble
    const bool autoMode = p_.preamble.untilRegex == "@auto";
    bool unitsDecided = false;
    if (autoMode) {
        // The names row is the first line with >= 2 fields followed by a line >= 80 % numeric (or by a units
        // row and then such a line); the preamble is everything before it (05 section 7.3, @auto rules).
        bool found = false;
        for (std::size_t i = 0; i < head_.size() && !found; ++i) {
            if (head_[i].blank) {
                continue;
            }
            splitCells(head_[i].text, L_->dialect, cells_, kMaxFields + 1);
            const std::size_t fields = cells_.size();
            if (fields < 2 || fields > kMaxFields || numericFraction(cells_, L_->dialect.delimiter) >= 0.5) {
                continue;
            }
            std::size_t j = i + 1;
            while (j < head_.size() && head_[j].blank) {
                ++j;
            }
            if (j >= head_.size()) {
                break;
            }
            splitCells(head_[j].text, L_->dialect, cells_, kMaxFields + 1);
            const auto close = [&](std::size_t n) { return n + 1 >= fields && n <= fields + 1; };
            if (numericFraction(cells_, L_->dialect.delimiter) >= kNumericRow && close(cells_.size())) {
                preambleEnd = i;
                namesLine_ = i;
                found = true;
                if (p_.header.unitsRow == "@auto") {
                    unitsDecided = true;
                    info(code::kAutoUnitsRow, "no units row", "no");
                }
                break;
            }
            const bool unitsLike =
                !hasNumericCell(cells_, L_->dialect.delimiter) && close(cells_.size()) &&
                std::all_of(cells_.begin(), cells_.end(), [](const Cell& c) { return c.text.size() <= 12; });
            if (!unitsLike) {
                continue;
            }
            std::size_t k = j + 1;
            while (k < head_.size() && head_[k].blank) {
                ++k;
            }
            if (k < head_.size()) {
                splitCells(head_[k].text, L_->dialect, cells_, kMaxFields + 1);
                if (numericFraction(cells_, L_->dialect.delimiter) >= kNumericRow && close(cells_.size())) {
                    preambleEnd = i;
                    namesLine_ = i;
                    found = true;
                    if (p_.header.unitsRow == "@auto") {
                        unitsLine_ = j;
                        unitsDecided = true;
                        info(code::kAutoUnitsRow, "units row found", "yes");
                    }
                }
            }
        }
        if (!found) {
            // A header without data rows (an empty export, a logger stopped at once) opens with no samples
            // (text.no_data_rows), as a fixed-row profile's does.
            const std::optional<HeaderOnly> headerOnly = headerOnlyRows();
            if (!headerOnly) {
                return fail(ErrorCode::Format, "no header row followed by numeric data was found");
            }
            preambleEnd = headerOnly->names;
            namesLine_ = headerOnly->names;
            if (p_.header.unitsRow == "@auto") {
                unitsDecided = true;
                unitsLine_ = headerOnly->units;
                info(code::kAutoUnitsRow, unitsLine_ ? "units row found" : "no units row",
                     unitsLine_ ? "yes" : "no");
            }
        }
        std::size_t lines = 0;
        for (std::size_t i = 0; i < preambleEnd; ++i) {
            lines += head_[i].blank ? 0 : 1;
        }
        info(code::kAutoPreamble, "preamble detected", std::to_string(lines));
    } else if (!p_.preamble.untilRegex.empty()) {
        const auto re = compileRegex(p_.preamble.untilRegex);
        std::size_t i = 0;
        for (; i < head_.size(); ++i) {
            if (re && regexSearch(*re, utf8(head_[i].text))) {
                break;
            }
        }
        if (i >= head_.size()) {
            return fail(ErrorCode::Format, "no line matches the preamble's untilRegex");
        }
        preambleEnd = i;
    } else if (p_.preamble.untilBlank) {
        std::size_t i = 0;
        while (i < head_.size() && !head_[i].blank) {
            ++i;
        }
        preambleEnd = i;
    } else {
        preambleEnd = std::min<std::size_t>(static_cast<std::size_t>(p_.preamble.lines), head_.size());
    }
    readPreamble(preambleEnd);

    // Header rows, counted among the non-blank lines after the preamble (skipBlank) or all of them.
    std::vector<std::size_t> rows;
    for (std::size_t i = preambleEnd; i < head_.size(); ++i) {
        if (!p_.header.skipBlank || !head_[i].blank) {
            rows.push_back(i);
        }
    }
    if (!autoMode) {
        if (static_cast<std::size_t>(p_.header.namesRow) >= rows.size()) {
            return fail(ErrorCode::Format, "the file has no names row");
        }
        namesLine_ = rows[static_cast<std::size_t>(p_.header.namesRow)];
    }
    const auto rowPos = [&](std::size_t line) {
        return static_cast<std::size_t>(std::find(rows.begin(), rows.end(), line) - rows.begin());
    };
    const std::size_t namesPos = rowPos(namesLine_);
    if (!unitsDecided) {
        if (p_.header.unitsRow == "@auto") {
            if (namesPos + 1 < rows.size()) {
                splitCells(head_[rows[namesPos + 1]].text, L_->dialect, cells_, kMaxFields + 1);
                const bool unitsLike = !hasNumericCell(cells_, L_->dialect.delimiter) &&
                                       std::all_of(cells_.begin(), cells_.end(),
                                                   [](const Cell& c) { return c.text.size() <= 12; });
                if (unitsLike) {
                    unitsLine_ = rows[namesPos + 1];
                }
            }
            info(code::kAutoUnitsRow, unitsLine_ ? "units row found" : "no units row",
                 unitsLine_ ? "yes" : "no");
        } else if (p_.header.unitsRow != "null" && !p_.header.unitsRow.empty()) {
            const auto u = static_cast<std::size_t>(std::stoi(p_.header.unitsRow));
            if (u < rows.size()) {
                unitsLine_ = rows[u];
            }
        }
    }
    std::size_t dataPos = std::max(namesPos, unitsLine_ ? rowPos(*unitsLine_) : namesPos) + 1;
    if (p_.header.dataRow != "auto" && !p_.header.dataRow.empty()) {
        dataPos = static_cast<std::size_t>(std::stoi(p_.header.dataRow));
    }
    // Data rows are always counted among non-blank lines from here on.
    while (dataPos < rows.size() && head_[rows[dataPos]].blank) {
        ++dataPos;
    }
    dataLineIx_ = dataPos < rows.size() ? rows[dataPos] : head_.size();
    return ok();
}

Result<void> Analyser::buildColumns() {
    splitCells(head_[namesLine_].text, L_->dialect, cells_, kMaxFields + 1);
    if (cells_.size() > kMaxFields) {
        ErrorContext ctx;
        ctx.detail = "line " + std::to_string(head_[namesLine_].line);
        return fail(ErrorCode::Format,
                    "the names row has more than " + std::to_string(kMaxFields) + " columns", std::move(ctx));
    }
    std::vector<Cell> names = cells_;
    std::vector<Cell> units;
    if (unitsLine_) {
        splitCells(head_[*unitsLine_].text, L_->dialect, units, kMaxFields + 1);
    }
    // A trailing delimiter leaves an empty last cell on every row.
    bool trailing = false;
    if (p_.trailingDelimiter) {
        trailing = *p_.trailingDelimiter && !names.empty() && names.back().text.empty();
    } else {
        trailing = names.size() > 1 && names.back().text.empty();
    }
    if (trailing) {
        names.pop_back();
    }
    L_->trailingDelimiter = trailing;
    L_->headerFields = names.size();
    std::optional<std::regex> unitRe;
    if (!p_.header.unitInName.empty()) {
        unitRe = compileRegex(p_.header.unitInName);
    }
    std::map<std::string, int, std::less<>> seen;
    for (std::size_t f = 0; f < names.size(); ++f) {
        std::string name = cellUtf8(names[f]);
        std::string unit = f < units.size() ? cellUtf8(units[f]) : std::string();
        if (unitRe && !name.empty()) {
            std::match_results<const char*> m;
            if (regexSearch(*unitRe, name, &m) && m.size() > 1 && m[1].matched) {
                if (unit.empty()) {
                    unit = std::string(trimSpaces(m[1].str()));
                }
                std::string stripped = name.substr(0, static_cast<std::size_t>(m.position(0))) +
                                       name.substr(static_cast<std::size_t>(m.position(0) + m.length(0)));
                name = std::string(trimSpaces(stripped));
            }
        }
        if (name.empty()) {
            if (p_.header.ignoreEmptyNames) {
                info(code::kEmptyName, "a column without a name is dropped",
                     "column " + std::to_string(f + 1));
                continue;
            }
            name = "Column " + std::to_string(f + 1);
        }
        const int count = ++seen[name];
        if (count > 1) {
            if (p_.duplicateNames == TextProfile::DuplicateNames::Error) {
                ErrorContext ctx;
                ctx.detail = name;
                return fail(ErrorCode::Format, "duplicate column name '" + name + "'", std::move(ctx));
            }
            std::string renamed;
            int k = count;
            do {
                renamed = name + " (" + std::to_string(k++) + ")";
            } while (seen.count(renamed) != 0);
            seen[renamed] = 1;
            info(code::kDuplicateName, "duplicate column name renamed", renamed, name);
            name = std::move(renamed);
        }
        names_.push_back(std::move(name));
        units_.push_back(std::move(unit));
        fields_.push_back(static_cast<std::uint32_t>(f));
    }
    return ok();
}

void Analyser::chooseTimeColumn() {
    const std::string& want = p_.time.column;
    std::optional<std::size_t> found;
    if (want == "@auto" || want.empty()) {
        for (std::size_t i = 0; i < names_.size() && !found; ++i) {
            if (isAutoTimeName(names_[i])) {
                found = i;
            }
        }
        if (!found) {
            // The first monotone numeric column over the first (up to) 20 data rows, each split once.
            std::vector<std::vector<Cell>> rows;
            for (std::size_t r = dataLineIx_; r < head_.size() && rows.size() < 20; ++r) {
                if (!head_[r].blank) {
                    rows.emplace_back();
                    splitCells(head_[r].text, L_->dialect, rows.back(), rowCap());
                }
            }
            for (std::size_t i = 0; i < names_.size() && !found && rows.size() >= 2; ++i) {
                double prev = -std::numeric_limits<double>::infinity();
                bool ok = true;
                bool increased = false;
                for (std::size_t r = 0; r < rows.size() && ok; ++r) {
                    const auto v = fields_[i] < rows[r].size() ? parseNumber(rows[r][fields_[i]].text, '.')
                                                               : std::nullopt;
                    ok = v.has_value() && *v >= prev;
                    increased = increased || (ok && r > 0 && *v > prev);
                    if (v) {
                        prev = *v;
                    }
                }
                if (ok && increased) {
                    found = i;
                }
            }
        }
        info(code::kAutoTimeColumn, found ? "time column detected" : "no time column detected",
             found ? names_[*found] : std::string("none"));
    } else {
        for (std::size_t i = 0; i < names_.size() && !found; ++i) {
            if (names_[i] == want) {
                found = i;
            }
        }
        for (std::size_t i = 0; i < names_.size() && !found; ++i) {
            if (ciEquals(names_[i], want)) {
                found = i;
            }
        }
    }
    if (found) {
        L_->timeField = static_cast<std::int32_t>(fields_[*found]);
        timeName_ = names_[*found];
        timeVendorUnit_ = units_[*found];
        names_.erase(names_.begin() + static_cast<std::ptrdiff_t>(*found));
        units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(*found));
        fields_.erase(fields_.begin() + static_cast<std::ptrdiff_t>(*found));
    }
}

void Analyser::scanRows() {
    // Fast pass: count the data rows, record the dense set's chunk starts and the sample rows (the first 256
    // for classification, every 1,000th and the last for the rate inference).
    if (dataLineIx_ >= head_.size()) {
        L_->dataStart = L_->text.size();
        L_->dataLine = head_.empty() ? 1 : head_.back().line + 1;
        L_->rowSets.emplace_back();
        warn(code::kNoDataRows, "the file has a header but no data rows");
        return;
    }
    L_->dataStart = head_[dataLineIx_].offset;
    L_->dataLine = head_[dataLineIx_].line;
    RowSet dense;
    GuardedLines cur(L_->text, static_cast<std::size_t>(L_->dataStart), L_->crOnly, textMapping());
    std::string_view line;
    std::uint64_t row = 0;
    std::uint64_t lineNo = L_->dataLine;
    std::uint64_t lastRow = 0;
    std::size_t lastOffset = 0;
    std::size_t lastLength = 0;
    std::size_t sampleBytes = 0;
    while (cur.next(line)) {
        const std::uint64_t thisLine = lineNo++;
        if (isBlank(line)) {
            continue;
        }
        if (row % store::kChunkSamples == 0) {
            if (cancel_.cancelled()) {
                return;
            }
            ChunkStart c;
            c.offset = cur.lineStart();
            c.line = thisLine;
            dense.chunks.push_back(std::move(c));
        }
        if ((row < kClassifyRows || row % kSampleEvery == 0) && sampleBytes + line.size() <= kSampleBudget) {
            samples_.push_back(SampleRow{row, std::string(line)});
            sampleBytes += line.size();
        }
        lastRow = row;
        lastOffset = cur.lineStart();
        lastLength = line.size();
        ++row;
    }
    if (cur.error()) {
        readError_ = *cur.error();
        return;
    }
    if (row > 0 && (samples_.empty() || samples_.back().row != lastRow) && lastLength <= kSampleBudget) {
        // The last row, read again (the cursor's view of it is gone).
        SampleRow tail{lastRow, std::string(lastLength, '\0')};
        if (Result<void> r = guardedCopy(textMapping(), L_->text, lastOffset, tail.text); !r) {
            readError_ = r.error();
            return;
        }
        samples_.push_back(std::move(tail));
    }
    dense.samples = row;
    L_->rows = row;
    L_->rowSets.push_back(std::move(dense));
    if (row == 0) {
        warn(code::kNoDataRows, "the file has a header but no data rows");
    }
}

void Analyser::chooseDecimal() {
    Dialect& d = L_->dialect;
    if (p_.decimal != "auto") {
        d.decimal = p_.decimal[0];
        return;
    }
    d.decimal = '.';
    if (d.delimiter != ',') {
        // Among the number-like tokens with a separator, ',' must be the decimal point of >= 90 % (integers
        // say nothing either way; 05 section 7.2 counts every numeric token, see docs/formats/text.md).
        std::size_t dot = 0;
        std::size_t comma = 0;
        for (std::size_t i = 0; i < samples_.size() && i < 50; ++i) {
            splitCells(samples_[i].text, d, cells_, rowCap());
            for (const Cell& c : cells_) {
                const std::string_view t = trimSpaces(c.text);
                const auto commas = std::count(t.begin(), t.end(), ',');
                const auto dots = std::count(t.begin(), t.end(), '.');
                if (commas == 1 && dots == 0 && parseNumber(t, ',')) {
                    ++comma;
                } else if (dots == 1 && commas == 0 && parseNumber(t, '.')) {
                    ++dot;
                }
            }
        }
        if (comma > 0 && comma * 10 >= (comma + dot) * 9) {
            d.decimal = ',';
        }
    }
    info(code::kAutoDecimal, "decimal separator detected", std::string(1, d.decimal));
}

void Analyser::classifyColumns() {
    const Dialect& d = L_->dialect;
    // One split per sampled row, every column counted in the same pass (linear in the sampled text).
    struct Counts {
        std::size_t n = 0;
        std::size_t numeric = 0;
        std::size_t integer = 0;
        int maxSig = 0;
        int maxFrac = 0;
        double maxAbs = 0.0;
    };
    std::vector<Counts> counts(names_.size());
    std::vector<const std::vector<std::string>*> sentinelsOf(names_.size(), &p_.sentinels);
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (const auto it = p_.columns.find(names_[i]);
            it != p_.columns.end() && !it->second.sentinels.empty()) {
            sentinelsOf[i] = &it->second.sentinels;
        }
    }
    for (const SampleRow& s : samples_) {
        splitCells(s.text, d, cells_, rowCap());
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (fields_[i] >= cells_.size()) {
                continue;
            }
            const std::string_view t = cells_[fields_[i]].text;
            if (t.empty() || isSentinel(t, *sentinelsOf[i])) {
                continue;
            }
            Counts& k = counts[i];
            ++k.n;
            if (const auto v = parseNumber(t, d.decimal)) {
                ++k.numeric;
                k.integer += isIntegerText(t) ? 1 : 0;
                k.maxSig = std::max(k.maxSig, significantDigits(t));
                k.maxFrac = std::max(k.maxFrac, fractionDigits(t, d.decimal));
                k.maxAbs = std::max(k.maxAbs, std::abs(*v));
            }
        }
    }
    for (std::size_t i = 0; i < names_.size(); ++i) {
        ColumnPlan c;
        c.field = fields_[i];
        c.loggedName = names_[i];
        c.vendorUnit = units_[i];
        c.sentinels = p_.sentinels;
        const TextProfile::Column* over = nullptr;
        if (const auto it = p_.columns.find(names_[i]); it != p_.columns.end()) {
            over = &it->second;
        }
        if (over != nullptr && !over->sentinels.empty()) {
            c.sentinels = over->sentinels;
        }
        const auto [n, numeric, integer, maxSig, maxFrac, maxAbs] = counts[i];
        if (n > 0 && numeric * 2 < n) {
            c.kind = store::SampleKind::Enum;
            c.dtype = store::Dtype::I32;
        } else if (n > 0 && numeric == n && integer == n) {
            c.kind = store::SampleKind::Integer;
            c.dtype = maxAbs <= 16777216.0 ? store::Dtype::F32 : store::Dtype::F64;
            c.dps = 0;
        } else {
            c.kind = store::SampleKind::Real;
            c.dtype = maxSig > 7 ? store::Dtype::F64 : store::Dtype::F32;
            c.dps = std::min(maxFrac, 6);
        }
        if (over != nullptr) {
            if (over->kind == "Text" || over->kind == "Enum") {
                c.kind = store::SampleKind::Enum;
                c.dtype = store::Dtype::I32;
            } else if (over->kind == "Integer") {
                c.kind = store::SampleKind::Integer;
            } else if (over->kind == "Real") {
                c.kind = store::SampleKind::Real;
            }
            if (!over->dtype.empty() && c.kind != store::SampleKind::Enum) {
                c.dtype = over->dtype == "F64"   ? store::Dtype::F64
                          : over->dtype == "I32" ? store::Dtype::I32
                                                 : store::Dtype::F32;
            }
            if (over->dps) {
                c.dps = *over->dps;
            }
            if (!over->unit.empty() && c.vendorUnit.empty()) {
                c.vendorUnit = over->unit;
            }
            if (over->interp == "Linear") {
                c.interp = store::Interp::Linear;
            } else if (over->interp == "Previous") {
                c.interp = store::Interp::Previous;
            } else if (over->interp == "Closest") {
                c.interp = store::Interp::Closest;
            } else if (over->interp == "Next") {
                c.interp = store::Interp::Next;
            }
            Dictionary::Entry e;
            e.match.name = c.loggedName;
            if (!over->name.empty()) {
                e.name = over->name;
            }
            if (!over->quantity.empty()) {
                e.quantity = over->quantity;
            }
            if (!over->unit.empty()) {
                e.unit = over->unit;
            }
            if (!over->interp.empty()) {
                e.interp = over->interp;
            }
            c.hint = std::move(e);
        }
        if (c.kind == store::SampleKind::Enum) {
            c.dps = 0;
            info(code::kTextColumn, "a text column becomes an enumeration", c.loggedName, c.loggedName);
        }
        c.hold = p_.sparse == TextProfile::Sparse::Hold;
        L_->columns.push_back(std::move(c));
    }
    L_->rowSets[0].members.resize(L_->columns.size());
    for (std::uint32_t i = 0; i < L_->columns.size(); ++i) {
        L_->rowSets[0].members[i] = i;
    }
}

std::optional<double> Analyser::rawTime(const Cell& cell) const {
    if (L_->timeUnit == TimeUnit::Clock) {
        return parseClock(cell.text, L_->dialect.decimal);
    }
    return parseNumber(cell.text, L_->dialect.decimal);
}

void Analyser::resolveTime() {
    Layout& lay = *L_;
    const Dialect& d = lay.dialect;
    // Unit.
    const std::string& unit = p_.time.unit;
    std::vector<std::pair<std::uint64_t, std::string_view>> texts; // sampled rows' time cells
    if (lay.timeField >= 0) {
        for (const SampleRow& s : samples_) {
            splitCells(s.text, d, cells_, rowCap());
            if (static_cast<std::size_t>(lay.timeField) < cells_.size()) {
                texts.emplace_back(s.row, cells_[static_cast<std::size_t>(lay.timeField)].text);
            }
        }
    }
    if (unit == "s") {
        lay.timeUnit = TimeUnit::Seconds;
    } else if (unit == "ms") {
        lay.timeUnit = TimeUnit::Millis;
    } else if (unit == "us") {
        lay.timeUnit = TimeUnit::Micros;
    } else if (unit == "hhmmss") {
        lay.timeUnit = TimeUnit::Clock;
    } else if (unit == "epoch_s") {
        lay.timeUnit = TimeUnit::EpochSeconds;
    } else if (unit == "epoch_ms") {
        lay.timeUnit = TimeUnit::EpochMillis;
    } else if (lay.timeField >= 0) { // @auto
        const std::string vu = lowerAscii(trimSpaces(timeVendorUnit_));
        std::optional<double> firstValue;
        bool allInt = true;
        bool clock = false;
        double maxValue = 0.0;
        for (const auto& [row, t] : texts) {
            if (t.find(':') != std::string_view::npos && parseClock(t, d.decimal)) {
                clock = true;
                break;
            }
            if (const auto v = parseNumber(t, d.decimal)) {
                if (!firstValue) {
                    firstValue = v;
                }
                allInt = allInt && isIntegerText(t);
                maxValue = std::max(maxValue, *v);
            }
        }
        // The column's own unit (units row or name) first, then magnitudes; epoch values are recognised in
        // either case (05 section 7.3 @auto rules).
        const bool epochMs = firstValue && *firstValue > 1e12;
        const bool epochS = firstValue && *firstValue > 1e9;
        if (vu == "s" || vu == "sec" || vu == "secs") {
            lay.timeUnit = epochS && !epochMs ? TimeUnit::EpochSeconds : TimeUnit::Seconds;
        } else if (vu == "ms") {
            lay.timeUnit = epochMs ? TimeUnit::EpochMillis : TimeUnit::Millis;
        } else if (vu == "us") {
            lay.timeUnit = TimeUnit::Micros;
        } else if (clock) {
            lay.timeUnit = TimeUnit::Clock;
        } else if (epochMs) {
            lay.timeUnit = TimeUnit::EpochMillis;
        } else if (epochS) {
            lay.timeUnit = TimeUnit::EpochSeconds;
        } else if (firstValue && allInt && maxValue > 1e4) {
            lay.timeUnit = TimeUnit::Millis;
        } else {
            lay.timeUnit = TimeUnit::Seconds;
        }
        static constexpr std::string_view kNames[] = {"s", "ms", "us", "hhmmss", "epoch_s", "epoch_ms"};
        info(code::kAutoTimeUnit, "time unit detected", std::string(kNames[static_cast<int>(lay.timeUnit)]));
    }

    // Parsed sample times (absolute microseconds).
    const double scale = lay.timeUnit == TimeUnit::Millis || lay.timeUnit == TimeUnit::EpochMillis ? 1e3
                         : lay.timeUnit == TimeUnit::Micros                                        ? 1.0
                                                                                                   : 1e6;
    std::vector<std::pair<std::uint64_t, Micros>> times;
    for (const auto& [row, t] : texts) {
        Cell c;
        c.text = t;
        if (const auto v = rawTime(c)) {
            const double us = std::round(*v * scale);
            if (std::abs(us) < 9.0e18) {
                times.emplace_back(row, static_cast<Micros>(us));
            }
        }
    }
    // Origin: "first" (default for wall-clock and epoch units) or "zero".
    const bool absolute = lay.timeUnit == TimeUnit::EpochSeconds || lay.timeUnit == TimeUnit::EpochMillis ||
                          lay.timeUnit == TimeUnit::Clock;
    const bool originFirst = p_.time.origin == "first" || (p_.time.origin.empty() && absolute);
    if (originFirst && !times.empty()) {
        lay.originUs = times.front().second;
    }
    if ((lay.timeUnit == TimeUnit::EpochSeconds || lay.timeUnit == TimeUnit::EpochMillis) && !times.empty()) {
        utc_ = UtcInstant{times.front().second};
    }
    for (auto& [row, t] : times) {
        t -= lay.originUs;
    }

    // Rate.
    std::optional<Rate> rate;
    bool fromProfile = false;
    if (p_.time.fixedRate) {
        rate = p_.time.fixedRate;
        fromProfile = true;
    } else if (p_.time.rate.starts_with("fromDetail:")) {
        std::string input = p_.time.rate.substr(std::string_view("fromDetail:").size());
        if (input.starts_with('@')) {
            input.erase(0, 1);
        }
        const auto it = inputs_.find(input);
        if (it != inputs_.end()) {
            rate = rateFromDecimalText(it->second.first);
        }
        if (rate) {
            fromProfile = true;
            info(code::kRateFromDetail, "sample rate from the file's details", rate->toString());
        } else {
            warn(code::kCaptureUnparsed,
                 "the rate the profile reads from the details is missing or unreadable", "@" + input);
        }
    }
    const double toleranceFactor = 0.6;
    // The uniformity check of one rate over the sampled rows: |t_i - (t0 + i / rate)| <= 0.6 / rate.
    const auto fits = [&](Rate r, Micros& t0Out) {
        if (times.empty()) {
            t0Out = 0;
            return true;
        }
        const auto& [r0, t0] = times.front();
        t0Out = t0 - r.timeAt(static_cast<SampleIdx>(r0), 0);
        const double tol = toleranceFactor * 1e6 * static_cast<double>(r.den) / static_cast<double>(r.num);
        for (const auto& [row, t] : times) {
            const Micros expected = r.timeAt(static_cast<SampleIdx>(row), t0Out);
            if (std::abs(static_cast<double>(t - expected)) > tol) {
                return false;
            }
        }
        return true;
    };
    bool inferred = false;
    if (!rate && times.size() >= 2) {
        const auto& [r0, t0] = times.front();
        const auto& [r1, t1] = times.back();
        if (r1 > r0 && t1 > t0) {
            // The candidates of 05 section 7.1 (smallest denominator <= 1,000 within 0.1 %), each checked:
            // the first that fits the sampled rows wins, so 20.48 Hz is not taken for 41/2.
            const double step = static_cast<double>(t1 - t0) / 1e6 / static_cast<double>(r1 - r0);
            const std::vector<Rate> candidates = rationalRates(1.0 / step);
            for (const Rate& c : candidates) {
                Micros t0Fit = 0;
                if (fits(c, t0Fit)) {
                    rate = c;
                    inferred = true;
                    break;
                }
            }
            if (!rate && !candidates.empty()) {
                rate = candidates.front(); // reported; the check below makes the set timestamped
            }
            if (inferred && rate) {
                info(code::kRateInferred, "sample rate inferred from the time column", rate->toString());
            }
        }
    }
    lay.uniform = true;
    if (!rate) {
        if (lay.timeField >= 0 && times.size() >= 2) {
            lay.uniform = false; // no rational rate near 1 / step: timestamped
        } else {
            rate = Rate{1, 1};
            if (lay.timeField < 0) {
                warn(code::kNoTimeColumn, "no time column and no rate: rows are taken one second apart");
            } else {
                warn(code::kRateDefault, "too few rows to infer a rate: rows are taken one second apart");
            }
        }
    }
    if (rate) {
        lay.rate = *rate;
    }
    // t0 and the uniformity check (|t_i - (t0 + i / rate)| <= 0.6 / rate over the sampled rows).
    if (lay.uniform && lay.timeField >= 0 && !times.empty()) {
        Micros t0Fit = 0;
        lay.uniform = fits(lay.rate, t0Fit);
        lay.t0Us = t0Fit;
        if (!lay.uniform) {
            if (fromProfile) {
                warn(code::kTimeRateMismatch,
                     "the time column does not follow the stated sample rate; the channels are timestamped",
                     lay.rate.toString());
            } else {
                info(code::kTimestamped, "the time column is not uniform; the channels are timestamped");
            }
        }
    } else if (!lay.uniform) {
        info(code::kTimestamped, "the time column is not uniform; the channels are timestamped");
    }
}

std::optional<Micros> rowTimeImpl(const Layout& lay, const std::vector<Cell>& cells) {
    if (lay.timeField < 0 || static_cast<std::size_t>(lay.timeField) >= cells.size()) {
        return std::nullopt;
    }
    const Cell& c = cells[static_cast<std::size_t>(lay.timeField)];
    const auto v = lay.timeUnit == TimeUnit::Clock ? parseClock(c.text, lay.dialect.decimal)
                                                   : parseNumber(c.text, lay.dialect.decimal);
    if (!v) {
        return std::nullopt;
    }
    const double scale = lay.timeUnit == TimeUnit::Millis || lay.timeUnit == TimeUnit::EpochMillis ? 1e3
                         : lay.timeUnit == TimeUnit::Micros                                        ? 1.0
                                                                                                   : 1e6;
    const double us = std::round(*v * scale);
    if (!(std::abs(us) < 9.0e18)) {
        return std::nullopt;
    }
    return static_cast<Micros>(us) - lay.originUs;
}

Result<void> Analyser::fullScan() {
    Layout& lay = *L_;
    const Dialect& d = lay.dialect;
    const bool sparseMode = p_.sparse == TextProfile::Sparse::Timestamped && lay.timeField >= 0;
    std::vector<std::uint32_t> textCols;
    for (std::uint32_t c = 0; c < lay.columns.size(); ++c) {
        if (lay.columns[c].kind == store::SampleKind::Enum) {
            textCols.push_back(c);
        }
    }
    const bool needTimes = !lay.uniform || sparseMode || lapField_ >= 0 || beaconEvent_;
    if (lay.uniform && textCols.empty() && !sparseMode && lapField_ < 0 && !beaconEvent_) {
        return ok();
    }
    Machine dense; // the dense set's time rule (and the template a sparse column starts from)
    std::vector<std::unique_ptr<Machine>> sparse(lay.columns.size());
    std::vector<std::uint32_t> sparseList;
    std::uint64_t invalidTime = 0;
    std::optional<double> lapValue;
    GuardedLines cur(lay.text, static_cast<std::size_t>(lay.dataStart), lay.crOnly, textMapping());
    std::string_view line;
    std::uint64_t lineNo = lay.dataLine;
    std::uint64_t row = 0;
    bool overflowLogged = false;
    while (cur.next(line)) {
        const std::uint64_t thisLine = lineNo++;
        if (isBlank(line)) {
            continue;
        }
        if ((row & 0xFFFF) == 0 && cancel_.cancelled()) {
            return fail(ErrorCode::Cancelled, "open cancelled");
        }
        const std::uint64_t offset = cur.lineStart();
        splitCells(line, d, cells_, rowCap());
        std::optional<Micros> t;
        if (needTimes) {
            t = rowTimeImpl(lay, cells_);
            if (!t) {
                ++invalidTime;
            }
        }
        for (const std::uint32_t c : textCols) {
            ColumnPlan& col = lay.columns[c];
            if (col.field >= cells_.size()) {
                continue;
            }
            const Cell& cell = cells_[col.field];
            if (cell.text.empty() || isSentinel(cell.text, col.sentinels)) {
                continue;
            }
            std::string label = cellUtf8(cell);
            if (col.enumIds.find(label) == col.enumIds.end()) {
                if (col.enumLabels.size() >= kMaxEnumLabels) {
                    if (!overflowLogged) {
                        overflowLogged = true;
                        warn(code::kEnumOverflow,
                             "a text column has more distinct values than an enumeration holds",
                             std::to_string(kMaxEnumLabels), col.loggedName);
                    }
                    continue;
                }
                col.enumIds.emplace(label, static_cast<std::int32_t>(col.enumLabels.size()));
                col.enumLabels.push_back(std::move(label));
            }
        }
        if (sparseMode && sparseList.size() < lay.columns.size()) {
            // A present, empty cell starts the column's own row set. Columns are in field order: a short
            // row stops the loop (linear in the row, not the header).
            for (std::uint32_t c = 0; c < lay.columns.size() && lay.columns[c].field < cells_.size(); ++c) {
                const std::uint32_t f = lay.columns[c].field;
                if (!sparse[c] && cells_[f].text.empty()) {
                    sparse[c] =
                        std::make_unique<Machine>(dense); // the column's samples so far = the dense ones
                    sparseList.push_back(c);
                }
            }
        }
        if (t) {
            dense.candidate(offset, thisLine, *t);
            // A sparse set drops the rows whose cell is empty; a missing cell (a ragged row) stays an
            // invalid sample (05 section 7.1 pass 2), as in the dense machine copied above and in
            // decodeChunk, so both passes count the same samples.
            for (const std::uint32_t c : sparseList) {
                const std::uint32_t f = lay.columns[c].field;
                if (f >= cells_.size() || !cells_[f].text.empty()) {
                    sparse[c]->candidate(offset, thisLine, *t);
                }
            }
        }
        if (lapField_ >= 0 && static_cast<std::size_t>(lapField_) < cells_.size()) {
            const auto v = parseNumber(cells_[static_cast<std::size_t>(lapField_)].text, d.decimal);
            if (v) {
                if (lapValue && *v != *lapValue) {
                    const std::optional<Micros> at =
                        lay.uniform
                            ? std::optional<Micros>(lay.rate.timeAt(static_cast<SampleIdx>(row), lay.t0Us))
                            : t;
                    if (at) {
                        MarkerInfo m;
                        m.cls = model::MarkerClass::BCN;
                        m.tUs = *at;
                        m.group = std::string(model::kGroupBeacons);
                        markers_.push_back(std::move(m));
                    }
                }
                lapValue = v;
            }
        }
        if (beaconEvent_) {
            bool all = !beaconMatch_.empty();
            for (const auto& [f, re] : beaconMatch_) {
                all = all && f < cells_.size() && regexSearch(re, cellUtf8(cells_[f]));
            }
            const std::optional<Micros> at =
                lay.uniform ? std::optional<Micros>(lay.rate.timeAt(static_cast<SampleIdx>(row), lay.t0Us))
                            : t;
            if (all && at) {
                MarkerInfo m;
                m.cls = model::MarkerClass::BCN;
                m.tUs = *at;
                m.group = std::string(model::kGroupBeacons);
                markers_.push_back(std::move(m));
            }
        }
        ++row;
    }
    if (cur.error()) {
        return Failure{*cur.error()};
    }
    dense.finish();
    for (const std::uint32_t c : sparseList) {
        sparse[c]->finish();
    }
    if (!lay.uniform) {
        RowSet& s = lay.rowSets[0];
        s.timestamped = true;
        s.chunks = std::move(dense.chunks);
        s.samples = dense.emitted;
        s.tFirst = dense.first;
        s.tLast = dense.last;
        if (dense.dropped > 0) {
            warn(code::kTimeNotMonotonic, "rows whose time goes backwards are dropped",
                 std::to_string(dense.dropped));
        }
        if (dense.duplicates > 0) {
            info(code::kTimeDuplicate, "rows with the time of the previous row replace it (last wins)",
                 std::to_string(dense.duplicates));
        }
        if (invalidTime > 0) {
            warn(code::kTimeInvalid, "rows without a valid time are skipped", std::to_string(invalidTime));
        }
    }
    std::sort(sparseList.begin(), sparseList.end());
    for (const std::uint32_t c : sparseList) {
        RowSet s;
        s.timestamped = true;
        s.sparseColumn = static_cast<std::int32_t>(c);
        s.members = {c};
        s.chunks = std::move(sparse[c]->chunks);
        s.samples = sparse[c]->emitted;
        s.tFirst = sparse[c]->first;
        s.tLast = sparse[c]->last;
        lay.columns[c].rowSet = static_cast<std::uint32_t>(lay.rowSets.size());
        info(code::kSparseColumn, "a column with empty cells keeps only its own samples (timestamped)",
             lay.columns[c].loggedName, lay.columns[c].loggedName);
        lay.rowSets.push_back(std::move(s));
    }
    if (!sparseList.empty()) {
        std::vector<std::uint32_t>& members = lay.rowSets[0].members;
        members.erase(std::remove_if(members.begin(), members.end(),
                                     [&](std::uint32_t c) { return lay.columns[c].rowSet != 0; }),
                      members.end());
    }
    return ok();
}

void Analyser::computeCarryIns() {
    // Hold columns (sparse "hold") live in the dense set. For every chunk k >= 1 the value carried in is the
    // last non-empty cell before the chunk's first row: found by scanning back to the previous chunk start;
    // a column still empty there inherits the previous chunk's carry-in (05 section 7.1 step 7).
    Layout& lay = *L_;
    RowSet& s = lay.rowSets[0];
    std::vector<std::uint32_t> hold;
    for (std::size_t i = 0; i < s.members.size(); ++i) {
        if (lay.columns[s.members[i]].hold) {
            hold.push_back(static_cast<std::uint32_t>(i));
        }
    }
    if (hold.empty()) {
        return;
    }
    const char term = lay.crOnly ? '\r' : '\n';
    const Dialect& d = lay.dialect;
    for (std::size_t k = 0; k < s.chunks.size(); ++k) {
        std::vector<double>& carry = s.chunks[k].carry;
        carry.assign(s.members.size(), std::numeric_limits<double>::quiet_NaN());
        if (k == 0) {
            continue;
        }
        const std::vector<double>& before = s.chunks[k - 1].carry;
        std::vector<bool> found(s.members.size(), false);
        std::size_t missing = hold.size();
        auto lineEnd = static_cast<std::size_t>(s.chunks[k].offset); // the end of the next line back
        const auto floor = static_cast<std::size_t>(s.chunks[k - 1].offset);
        // Backwards through guarded copies of the text before the chunk (D-190): a window [winLo, winEnd);
        // a line whose start the window does not show is read again from a window ending at it, a window
        // that shows no line start at all doubles (up to the longest line pass 2 reads).
        std::size_t window = kCarryWindow;
        std::size_t winLo = 0;
        std::size_t winEnd = 0;
        bool loaded = false;
        while (missing > 0 && lineEnd > floor) {
            if (!loaded) {
                winEnd = lineEnd;
                winLo = lineEnd - std::min(window, lineEnd - floor);
                carryBuf_.resize(winEnd - winLo);
                if (Result<void> r = guardedCopy(textMapping(), lay.text, winLo, carryBuf_); !r) {
                    readError_ = r.error();
                    return;
                }
                loaded = true;
            }
            const std::string_view w(carryBuf_.data(), carryBuf_.size());
            const std::size_t start = prevLineStart(w, lineEnd - winLo, term);
            if (start == 0 && winLo > floor) {
                if (lineEnd == winEnd) {
                    if (window >= 2 * GuardedLines::kMaxLine) {
                        break; // a line longer than pass 2 reads: the carry-in stays inherited
                    }
                    window *= 2;
                }
                loaded = false;
                continue;
            }
            std::string_view line = w.substr(start, lineEnd - winLo - start);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.remove_suffix(1);
            }
            lineEnd = winLo + start;
            if (isBlank(line)) {
                continue;
            }
            splitCells(line, d, cells_, rowCap());
            for (const std::uint32_t m : hold) {
                if (found[m]) {
                    continue;
                }
                const ColumnPlan& col = lay.columns[s.members[m]];
                if (col.field >= cells_.size() || cells_[col.field].text.empty()) {
                    continue;
                }
                found[m] = true;
                --missing;
                const std::string_view t = cells_[col.field].text;
                if (isSentinel(t, col.sentinels)) {
                    continue; // an invalid value is carried (NaN)
                }
                if (col.kind == store::SampleKind::Enum) {
                    const auto it = col.enumIds.find(cellUtf8(cells_[col.field]));
                    if (it != col.enumIds.end()) {
                        carry[m] = static_cast<double>(it->second);
                    }
                } else if (const auto v = parseNumber(t, d.decimal)) {
                    carry[m] = *v;
                }
            }
        }
        for (const std::uint32_t m : hold) {
            if (!found[m]) {
                carry[m] = before[m];
            }
        }
    }
}

Result<AnalyseResult> Analyser::run(std::string_view importerId, std::string_view importerVersion) {
    if (p_.layout == TextProfile::Layout::Blocks) {
        return fail(ErrorCode::Unsupported,
                    "the sectioned (\"blocks\") text layout of D-102 is not implemented in this release");
    }
    decodeText();
    if (!readError_) {
        readHeadLines();
    }
    if (readError_) {
        return Failure{*readError_};
    }
    if (longFirstLine_) {
        ErrorContext ctx;
        ctx.detail = "line " + std::to_string(longLine_);
        return fail(ErrorCode::Format,
                    "line " + std::to_string(longLine_) + " exceeds the maximum line length of " +
                        std::to_string(kHeadBytes) + " bytes",
                    std::move(ctx));
    }
    if (head_.empty()) {
        return fail(ErrorCode::Format, "the file is empty");
    }
    chooseDelimiter();
    checkQuotedNewlines();
    if (auto r = findHeader(); !r) {
        return Failure{r.error()};
    }
    if (auto r = buildColumns(); !r) {
        return Failure{r.error()};
    }
    chooseTimeColumn();
    scanRows();
    if (cancel_.cancelled()) {
        return fail(ErrorCode::Cancelled, "open cancelled");
    }
    if (readError_) {
        return Failure{*readError_};
    }
    chooseDecimal();
    classifyColumns();
    resolveTime();

    // Lap sources (05 section 7.1): evaluated here so probe_json carries the markers.
    const TextProfile::LapSource* src = &p_.lapSource;
    while (src != nullptr) {
        bool used = false;
        if (src->kind == "lap_counter_edge") {
            std::string colName = src->column;
            std::optional<std::regex> re;
            if (colName.starts_with("@auto:")) {
                re = compileRegex(colName.substr(6), true);
            }
            for (const ColumnPlan& c : L_->columns) {
                const bool hit = re ? regexFullMatch(*re, c.loggedName) : c.loggedName == colName;
                if (hit) {
                    lapField_ = static_cast<std::int32_t>(c.field);
                    info(code::kLapSource, "laps from the changes of a lap counter column",
                         "lap_counter_edge:" + c.loggedName);
                    used = true;
                    break;
                }
            }
        } else if (src->kind == "beacon_event") {
            for (const auto& [col, pattern] : src->match) {
                for (const ColumnPlan& c : L_->columns) {
                    if (c.loggedName == col) {
                        if (auto re = compileRegex(pattern)) {
                            beaconMatch_.emplace_back(c.field, std::move(*re));
                        }
                    }
                }
            }
            if (!beaconMatch_.empty() && beaconMatch_.size() == src->match.size()) {
                beaconEvent_ = true;
                used = true;
                info(code::kLapSource, "laps from beacon rows", "beacon_event");
            } else {
                beaconMatch_.clear();
            }
        } else if (src->kind == "lap_table") {
            const bool present = std::any_of(details_.begin(), details_.end(), [&](const model::Detail& dd) {
                return dd.id == src->detailsKey;
            });
            if (present || inputs_.count(src->detailsKey) != 0) {
                warn(code::kLapTableUnverified,
                     "lap tables in the file's details are not read until verified against real exports "
                     "(D-106)",
                     src->detailsKey);
            }
        } else if (src->kind == "none") {
            used = true;
        }
        if (used) {
            break;
        }
        if (src->fallback) {
            src = src->fallback.get();
            continue;
        }
        info(code::kLapSourceAbsent, "the profile's lap source is not in this file", src->kind);
        break;
    }

    if (auto r = fullScan(); !r) {
        return Failure{r.error()};
    }
    computeCarryIns();
    if (readError_) {
        return Failure{*readError_};
    }

    // ---- the probe -------------------------------------------------------------------------------------
    Layout& lay = *L_;
    AnalyseResult out;
    ProbeResult& probe = out.probe;
    probe.importerId = std::string(importerId);
    probe.importerVersion = std::string(importerVersion);
    std::vector<std::string> flags;
    if (lay.dialect.delimiter != ',') {
        flags.emplace_back(delimiterName(lay.dialect.delimiter));
    }
    if (lay.dialect.decimal == ',') {
        flags.emplace_back("decimal-comma");
    }
    if (!lay.uniform) {
        flags.emplace_back("timestamped");
    }
    for (const std::string& f : flags) {
        probe.dialect += (probe.dialect.empty() ? "" : ",") + f;
    }
    probe.details = details_;
    LayerInfo layer;
    layer.name = "Normal";
    layer.utc = utc_;
    Micros duration = 0;
    for (std::uint32_t c = 0; c < lay.columns.size(); ++c) {
        const ColumnPlan& col = lay.columns[c];
        const RowSet& set = lay.rowSets[col.rowSet];
        ChannelInfo ci;
        ci.index = c;
        ci.loggedName = col.loggedName;
        ci.vendorUnit = col.vendorUnit;
        ci.vendorDtype = "text";
        ci.dtype = col.dtype;
        ci.kind = col.kind;
        ci.sampleCount = set.samples;
        ci.dps = col.dps;
        if (set.timestamped) {
            ci.timing = Timestamped{};
            ci.t0Us = set.samples > 0 ? set.tFirst : 0;
            duration = std::max(duration, set.samples > 0 ? set.tLast : 0);
        } else {
            ci.timing = lay.rate;
            ci.t0Us = lay.t0Us;
            duration = std::max(duration, lay.rate.timeAt(static_cast<SampleIdx>(set.samples), lay.t0Us));
        }
        if (col.kind == store::SampleKind::Enum) {
            ci.enumTable = static_cast<std::uint32_t>(probe.enumTables.size());
            lay.columns[c].enumTable = ci.enumTable;
            EnumTableInfo t;
            t.id = *ci.enumTable;
            for (std::size_t i = 0; i < col.enumLabels.size(); ++i) {
                t.entries.push_back(model::EnumEntry{static_cast<std::int32_t>(i), col.enumLabels[i], 0});
            }
            probe.enumTables.push_back(std::move(t));
        }
        layer.channels.push_back(std::move(ci));
    }
    layer.durationUs = std::max<Micros>(duration, 0);
    // Markers in time order; ordinal = position among markers of the same (class, group, time).
    std::stable_sort(markers_.begin(), markers_.end(),
                     [](const MarkerInfo& a, const MarkerInfo& b) { return a.tUs < b.tUs; });
    for (std::size_t i = 0; i < markers_.size(); ++i) {
        if (i > 0 && markers_[i].tUs == markers_[i - 1].tUs && markers_[i].cls == markers_[i - 1].cls &&
            markers_[i].group == markers_[i - 1].group) {
            markers_[i].ordinal = markers_[i - 1].ordinal + 1;
        }
    }
    layer.markers = markers_;
    probe.layers.push_back(std::move(layer));
    lay.formatHeaderEnd = std::min<std::uint64_t>(lay.dataStart, kHeadBytes);
    out.log = log_.snapshot();
    probe.warnings = out.log;
    out.layout = L_;
    return out;
}

} // namespace

std::optional<Micros> rowTime(const Layout& layout, const std::vector<Cell>& cells) {
    return rowTimeImpl(layout, cells);
}

Result<AnalyseResult> analyse(const TextProfile& profile, std::span<const std::byte> bytes,
                              std::string_view importerId, std::string_view importerVersion,
                              const CancelToken& cancel, const MappedFile* mapped) {
    try {
        Analyser a(profile, bytes, mapped, cancel);
        return a.run(importerId, importerVersion);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory reading the text file");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("unexpected error reading the text file: ") + e.what());
    }
}

} // namespace esses::formats::text
