// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The TextEngine's scanner (src/text/text_scan): cells, quotes, numbers with either decimal point, line ends,
// encodings, the rational rate search and the bounded regular expressions.
#include "text/text_scan.hpp"
#include "text_test_support.hpp"

#include <esses/base/mapped_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::formats::text;

namespace {

std::vector<std::string> cells(std::string_view line, Dialect d = {}) {
    std::vector<Cell> out;
    splitCells(line, d, out);
    std::vector<std::string> s;
    for (const Cell& c : out) {
        s.push_back(cellText(c, d));
    }
    return s;
}

std::vector<std::string> lines(std::string_view text) {
    LineCursor cur(text, 0, isCrOnly(text));
    std::vector<std::string> out;
    std::string_view l;
    while (cur.next(l)) {
        out.emplace_back(l);
    }
    return out;
}

} // namespace

TEST_CASE("text scan: cells, quotes and trimming", "[unit]") {
    CHECK(cells("a,b,c") == std::vector<std::string>{"a", "b", "c"});
    CHECK(cells("a,,c,") == std::vector<std::string>{"a", "", "c", ""});
    CHECK(cells("") == std::vector<std::string>{""});
    CHECK(cells(" 1.5 , 2 ") == std::vector<std::string>{"1.5", "2"});
    CHECK(cells(R"("x, y","say ""hi""",3)") == std::vector<std::string>{"x, y", R"(say "hi")", "3"});
    CHECK(cells(R"(  "quoted after spaces" ,b)") == std::vector<std::string>{"quoted after spaces", "b"});
    CHECK(cells(R"(a"b,c)") == std::vector<std::string>{"a\"b", "c"}); // a quote inside a cell is text
    CHECK(cells(R"("unterminated,x)") == std::vector<std::string>{"unterminated,x"});
    Dialect semi;
    semi.delimiter = ';';
    CHECK(cells("1,5;2,25;x", semi) == std::vector<std::string>{"1,5", "2,25", "x"});
    Dialect noQuote;
    noQuote.quoted = false;
    CHECK(cells(R"("a",b)", noQuote) == std::vector<std::string>{"\"a\"", "b"});
    Dialect keep;
    keep.trim = false;
    CHECK(cells(" a , b", keep) == std::vector<std::string>{" a ", " b"});
    CHECK(countDelimiters(R"(a,"b,c",d)", ',', true, '"') == 2);
    CHECK(countDelimiters(R"(a,"b,c",d)", ',', false, '"') == 3);
}

TEST_CASE("text scan: numbers with a point or a comma", "[unit]") {
    CHECK(parseNumber("1.5", '.') == 1.5);
    CHECK(parseNumber(".02", '.') == 0.02); // leading-dot numbers (05 section 7.3, EFI)
    CHECK(parseNumber("-3", '.') == -3.0);
    CHECK(parseNumber("+4.25", '.') == 4.25);
    CHECK(parseNumber("1e3", '.') == 1000.0);
    CHECK(parseNumber("  7  ", '.') == 7.0);
    CHECK(parseNumber("1,5", ',') == 1.5);
    CHECK_FALSE(parseNumber("1,5", '.').has_value());
    CHECK_FALSE(parseNumber("1.5", ',').has_value());
    CHECK_FALSE(parseNumber("", '.').has_value());
    CHECK_FALSE(parseNumber("1.2.3", '.').has_value());
    CHECK_FALSE(parseNumber("12abc", '.').has_value());
    CHECK_FALSE(parseNumber("inf", '.').has_value());
    CHECK_FALSE(parseNumber("nan", '.').has_value());
    CHECK_FALSE(parseNumber("+-1", '.').has_value());
    CHECK_FALSE(parseNumber("-###", '.').has_value());
    CHECK(isIntegerText("-12"));
    CHECK_FALSE(isIntegerText("1.0"));
    CHECK(significantDigits("0.001234") == 4);
    CHECK(significantDigits("52.12345678") == 10);
    CHECK(significantDigits("-1740") == 4);
    CHECK(parseClock("01:02:03.5", '.') == 3723.5);
    CHECK(parseClock("2:03.25", '.') == 123.25);
    CHECK_FALSE(parseClock("12:75:00", '.').has_value());
    CHECK_FALSE(parseClock("12.5", '.').has_value());
}

TEST_CASE("text scan: rates from decimal text and from a step", "[unit]") {
    CHECK(rateFromDecimalText("20.480") == Rate{512, 25});
    CHECK(rateFromDecimalText("50") == Rate{50, 1});
    CHECK(rateFromDecimalText("0.5") == Rate{1, 2});
    CHECK_FALSE(rateFromDecimalText("0").has_value());
    CHECK_FALSE(rateFromDecimalText("fast").has_value());
    CHECK(rationalRate(1.0 / 0.02) == Rate{50, 1});
    CHECK(rationalRate(1.0 / 0.001) == Rate{1000, 1});
    CHECK_FALSE(rationalRate(0.0).has_value());
    // 20.48 Hz: 41/2 is within 0.1 % too; the engine checks each candidate against the time column.
    const std::vector<Rate> c = rationalRates(20.48);
    REQUIRE(c.size() >= 2);
    CHECK(c.front() == Rate{41, 2});
    CHECK(std::find(c.begin(), c.end(), Rate{512, 25}) != c.end());
    for (std::size_t i = 1; i < c.size(); ++i) {
        CHECK(c[i - 1].den < c[i].den);
    }
}

TEST_CASE("text scan: line ends and blank lines", "[unit]") {
    CHECK(lines("a\nb\n") == std::vector<std::string>{"a", "b"});
    CHECK(lines("a\r\nb\r\n\r\nc") == std::vector<std::string>{"a", "b", "", "c"});
    CHECK(lines("a\rb\rc") == std::vector<std::string>{"a", "b", "c"}); // CR-only file
    CHECK(lines("") == std::vector<std::string>{});
    CHECK(isBlank(" \t\r"));
    CHECK_FALSE(isBlank(","));
}

TEST_CASE("text scan: encodings", "[unit]") {
    const auto bytes = [](std::initializer_list<unsigned char> b) {
        std::vector<std::byte> v;
        for (const unsigned char c : b) {
            v.push_back(static_cast<std::byte>(c));
        }
        return v;
    };
    CHECK(detectEncoding(bytes({0xEF, 0xBB, 0xBF, 'a'})) == Enc::Utf8Bom);
    CHECK(detectEncoding(bytes({0xFF, 0xFE, 'a', 0})) == Enc::Utf16Le);
    CHECK(detectEncoding(bytes({0xFE, 0xFF, 0, 'a'})) == Enc::Utf16Be);
    CHECK(detectEncoding(bytes({'a', 0xC3, 0xA9})) == Enc::Utf8);
    CHECK(detectEncoding(bytes({'a', 0xC3})) == Enc::Utf8);        // a sequence cut by the head
    CHECK(detectEncoding(bytes({'a', 0xE9, 'b'})) == Enc::Cp1252); // 'b' cannot continue the sequence
    CHECK(detectEncoding(bytes({'a', 0xE9, 'b', 'c', 'd'})) == Enc::Cp1252);
    CHECK(toUtf8("caf\xE9", Enc::Cp1252) == "caf\xC3\xA9");
    CHECK(toUtf8("caf\xC3\xA9", Enc::Utf8) == "caf\xC3\xA9");
    CHECK(toUtf8("caf\xE9", Enc::Utf8) == "caf\xC3\xA9"); // invalid UTF-8 falls back to cp1252
    const auto le = bytes({'T', 0, 'i', 0, 0xE9, 0, '\n', 0});
    CHECK(utf16ToUtf8(le, false) == "Ti\xC3\xA9\n");
    const auto be = bytes({0, 'A', 0, 'b'});
    CHECK(utf16ToUtf8(be, true) == "Ab");
}

TEST_CASE("text scan: regular expressions are bounded and never throw", "[unit]") {
    CHECK_FALSE(compileRegex("(unclosed").has_value());
    const auto re = compileRegex("^\"Format\",\"x\"");
    REQUIRE(re.has_value());
    CHECK(regexSearch(*re, "\"Format\",\"x\",,,\r\nmore"));
    // The sniff search is multi-line on every standard library: '^' anchors at each line start.
    CHECK(regexSearchLines(*re, "first\r\n\"Format\",\"x\"", true));
    CHECK(regexSearchLines(*re, "first\r\"Format\",\"x\"", true));
    CHECK_FALSE(regexSearchLines(*re, "first \"Format\",\"x\"", true));
    // Many short lines: linear (one pinned attempt per line start).
    std::string many;
    for (int i = 0; i < 4000; ++i) {
        many += "x\n";
    }
    CHECK_FALSE(regexSearchLines(*re, many, true));
    CHECK(regexSearchLines(*re, many.substr(0, 2000) + "\"Format\",\"x\"", true));
    const auto icase = compileRegex("^(lap|lap number)$", true);
    REQUIRE(icase.has_value());
    CHECK(regexFullMatch(*icase, "Lap Number"));
    // A subject longer than the bound is cut (search) or refused (full match), so a long line cannot exhaust
    // the stack of a recursive std::regex engine.
    const auto any = compileRegex("^a*b");
    REQUIRE(any.has_value());
    const std::string longLine(kMaxRegexSubject * 4, 'a');
    CHECK_FALSE(regexSearch(*any, longLine + "b"));
    CHECK_FALSE(regexFullMatch(*any, longLine + "b"));
}

TEST_CASE("text scan: a cell cap stops the split at the cells the caller can use", "[unit]") {
    const Dialect d;
    std::vector<Cell> out;
    splitCells("a,b,c,d,e", d, out, 3);
    REQUIRE(out.size() == 3);
    CHECK(out[2].text == "c");
    splitCells(R"("x,y","z",w,v)", d, out, 2);
    REQUIRE(out.size() == 2);
    CHECK(out[0].text == "x,y");
    CHECK(out[1].text == "z");
    splitCells("a,b", d, out, 0); // a line always yields one cell
    CHECK(out.size() == 1);
    // A hostile line of 4 MiB of delimiters yields the cap, not a cell per delimiter.
    const std::string hostile(std::size_t{4} << 20, ',');
    splitCells(hostile, d, out, 5);
    CHECK(out.size() == 5);
    splitCells(hostile, d, out, kMaxFields + 1);
    CHECK(out.size() == kMaxFields + 1);
}

TEST_CASE("text scan: nested quantifiers are found; patterns compile once", "[unit]") {
    for (const char* bad : {R"((a+)+)", R"((a*)*b)", R"((\w+\s?)+$)", R"(((a+))+)", R"((x|b+){2,})",
                            R"((\d+){3})", R"(^(?:[a-z]+)*$)"}) {
        INFO(bad);
        CHECK(hasNestedQuantifier(bad));
    }
    for (const char* good :
         {R"(^Time$)", R"((ab)*c)", R"([a-z]+(\.[a-z]+)*)", R"((a+)?)", R"((a+){1})", R"([(+]+)",
          R"(a{2,}b+)", R"([\(\[]([^\)\]]*)[\)\]]\s*(\*OBD)?$)", R"(^(lap|lap number|lap count|lapno)$)",
          R"(^[Ll][Aa][Pp]( ?([Nn][Uu][Mm][Bb][Ee][Rr]|[Cc][Oo][Uu][Nn][Tt]))?$)", R"(^\{(\w+)\}$)",
          R"(^"Format","[^"]*")", R"(a{x}+)"}) {
        INFO(good);
        CHECK_FALSE(hasNestedQuantifier(good));
    }
    const auto first = cachedRegex("^Speed( FL)?$");
    REQUIRE(first != nullptr);
    CHECK(cachedRegex("^Speed( FL)?$") == first); // compiled once
    CHECK(regexSearch(*first, "Speed FL"));
    CHECK(cachedRegex("(unclosed") == nullptr);
}

namespace {

// The lines of `text` as GuardedLines reads them from a file mapping with a tiny window and line limit. The
// file lives in a directory of its own (TempDir): processes running this test at once never share it.
std::vector<std::string> guardedLines(const std::string& text, std::size_t window, std::size_t maxLine,
                                      std::vector<std::size_t>* starts = nullptr) {
    const formats::texttest::TempDir dir("guarded-lines");
    const auto path = dir.path() / "lines.txt";
    formats::texttest::writeText(path, text);
    std::vector<std::string> out;
    {
        auto mf = MappedFile::open(path);
        REQUIRE(mf.ok());
        const std::string_view view(reinterpret_cast<const char*>(mf->bytes().data()), mf->size());
        GuardedLines cur(view, 0, isCrOnly(view), &*mf, window, maxLine);
        std::string_view l;
        while (cur.next(l)) {
            out.emplace_back(l);
            if (starts != nullptr) {
                starts->push_back(cur.lineStart());
            }
        }
        CHECK_FALSE(cur.error().has_value());
        CHECK(cur.pos() == view.size());
    } // the mapping is closed before the directory goes
    return out;
}

} // namespace

TEST_CASE("text scan: guarded lines over a mapping read what LineCursor reads, window by window", "[unit]") {
    const std::vector<std::string> texts = {
        "Time,A\r\n0,1\r\n\r\n1,2\r\n22,333\r\nlast without end",
        "a\nbb\nccc\ndddd\neeeee\nffffff\n",
        "cr\ronly\rfile\r",
        "\n\n\nx\n",
        "single",
    };
    for (const std::string& text : texts) {
        for (const std::size_t window :
             {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{64}}) {
            INFO(text << " / window " << window);
            std::vector<std::size_t> starts;
            const std::vector<std::string> got = guardedLines(text, window, 1000, &starts);
            std::vector<std::string> want;
            std::vector<std::size_t> wantStarts;
            LineCursor cur(text, 0, isCrOnly(text));
            std::string_view l;
            while (cur.next(l)) {
                want.emplace_back(l);
                wantStarts.push_back(cur.lineStart());
            }
            CHECK(got == want);
            CHECK(starts == wantStarts);
        }
    }
    // A line longer than the limit is cut to it; the next line follows as usual.
    const std::string longLine = "Time,A\n" + std::string(100, 'x') + "\n1,2\n";
    const std::vector<std::string> cut = guardedLines(longLine, 8, 30);
    REQUIRE(cut.size() == 3);
    CHECK(cut[1] == std::string(30, 'x'));
    CHECK(cut[2] == "1,2");
    // Without a mapping it is LineCursor in place (no cut).
    GuardedLines direct(longLine, 0, false, nullptr, 8, 30);
    std::string_view l;
    REQUIRE(direct.next(l));
    REQUIRE(direct.next(l));
    CHECK(l.size() == 100);
}

TEST_CASE("text scan: a line that ends inside a quoted field", "[unit]") {
    const Dialect d;
    CHECK(endsInOpenQuote(R"(Time,"Speed)", d));
    CHECK(endsInOpenQuote(R"("a","b ""quoted"" and open)", d));
    CHECK_FALSE(endsInOpenQuote(R"(Time,"Speed",RPM)", d));
    CHECK_FALSE(endsInOpenQuote(R"(a"b,c)", d)); // a quote inside a field is text
    CHECK_FALSE(endsInOpenQuote(R"("x" trailing,"y")", d));
    CHECK_FALSE(endsInOpenQuote(R"("say ""hi""")", d));
    Dialect noQuote;
    noQuote.quoted = false;
    CHECK_FALSE(endsInOpenQuote(R"(Time,"Speed)", noQuote));
}
