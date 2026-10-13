// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Lexer and Parser (03 sections 2.1-2.5, 3.1; D-020): token kinds, the precedence table of 03 section 2.3
// under both settings of MathsCompat::unaryMinusAbovePower, word operators, literals, comments, the
// canonical form (print), the definition hash, every syntax diagnostic code and error recovery.

#include "lang/codes.hpp"
#include "lang_test_support.hpp"

#include <esses/maths/lexer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace esses;
using namespace esses::maths;
using namespace esses::maths::test;

namespace {

MathsCompat compatOn() {
    MathsCompat c;
    c.unaryMinusAbovePower = true;
    return c;
}

std::vector<TokenKind> kindsOf(std::string_view src, Diagnostics& diags) {
    std::vector<TokenKind> out;
    for (const Token& t : Lexer::tokenize(src, diags)) {
        out.push_back(t.kind);
    }
    return out;
}

// Parses `src`, expecting failure; returns the diagnostics.
Diagnostics parseFails(std::string_view src, const MathsCompat& compat = {}) {
    Diagnostics diags;
    Result<Ast> ast = Parser::parse(src, compat, diags);
    INFO("source: " << src);
    CHECK_FALSE(ast.ok());
    if (!ast.ok()) {
        CHECK(ast.error().code == ErrorCode::Parse);
        CHECK(ast.error().context.span.has_value());
    }
    CHECK(hasError(diags));
    return diags;
}

} // namespace

TEST_CASE("lexer: every token kind with its text and byte span", "[unit]") {
    Diagnostics diags;
    const std::string src = "'Engine RPM' [rpm] 12 1.5 \"Outings:Laps\" mylib.rms AND rise ( ) , <=";
    const std::vector<Token> t = Lexer::tokenize(src, diags);
    CHECK(diags.empty());
    REQUIRE(t.size() == 13);
    CHECK(t[0].kind == TokenKind::ChannelRef);
    CHECK(t[0].text == "Engine RPM");
    CHECK(t[0].span.begin == 0);
    CHECK(t[0].span.end == 12);
    CHECK(t[1].kind == TokenKind::UnitSuffix);
    CHECK(t[1].text == "rpm");
    CHECK(t[2].kind == TokenKind::IntLit);
    CHECK(t[3].kind == TokenKind::DecLit);
    CHECK(t[4].kind == TokenKind::StrLit);
    CHECK(t[4].text == "Outings:Laps");
    CHECK(t[5].kind == TokenKind::Ident);
    CHECK(t[5].text == "mylib.rms");
    CHECK(t[6].kind == TokenKind::KeywordOp);
    CHECK(t[7].kind == TokenKind::KeywordConst);
    CHECK(t[8].kind == TokenKind::LParen);
    CHECK(t[9].kind == TokenKind::RParen);
    CHECK(t[10].kind == TokenKind::Comma);
    CHECK(t[11].kind == TokenKind::Operator);
    CHECK(t[11].text == "<=");
    CHECK(t[12].kind == TokenKind::End);
    CHECK(t[12].span.begin == src.size());
    CHECK(t.back().kind == TokenKind::End);
}

TEST_CASE("lexer: integer and decimal literals are distinct (1e3 and 1. are decimal)", "[unit]") {
    const std::vector<std::pair<std::string, TokenKind>> cases = {
        {"12", TokenKind::IntLit},   {"0", TokenKind::IntLit},      {"1.5", TokenKind::DecLit},
        {".5", TokenKind::DecLit},   {"1.", TokenKind::DecLit},     {"1e3", TokenKind::DecLit},
        {"1E-3", TokenKind::DecLit}, {"2.5e+2", TokenKind::DecLit}, {"007", TokenKind::IntLit}};
    for (const auto& [src, kind] : cases) {
        INFO(src);
        Diagnostics diags;
        const std::vector<Token> t = Lexer::tokenize(src, diags);
        CHECK(diags.empty());
        REQUIRE(t.size() == 2);
        CHECK(t[0].kind == kind);
        CHECK(t[0].text == src);
    }
    CHECK(parseOk(".5").nodes[0].number == 0.5);
    CHECK(parseOk("1e3").nodes[0].number == 1000.0);
    CHECK(parseOk("2.5e-2").nodes[0].number == 0.025);
}

TEST_CASE("lexer: keyword operators in any case, keyword constants, dotted names are never keywords",
          "[unit]") {
    Diagnostics diags;
    const std::vector<Token> t = Lexer::tokenize("and Or nOt lt LTE gt gte eq neq RISE Fall a.and", diags);
    REQUIRE(t.size() == 13);
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(t[i].kind == TokenKind::KeywordOp);
    }
    CHECK(t[0].text == "AND");
    CHECK(t[2].text == "NOT");
    CHECK(t[9].kind == TokenKind::KeywordConst);
    CHECK(t[9].text == "rise");
    CHECK(t[10].text == "fall");
    CHECK(t[11].kind == TokenKind::Ident);
}

TEST_CASE("lexer: comments are dropped and positions are kept", "[unit]") {
    Diagnostics diags;
    const std::string src = "1 /* a\nb */ + // note\n 'X'";
    const std::vector<Token> t = Lexer::tokenize(src, diags);
    CHECK(diags.empty());
    REQUIRE(t.size() == 4);
    CHECK(t[1].text == "+");
    CHECK(t[1].span.begin == src.find('+'));
    CHECK(t[2].span.begin == src.find('\''));
    CHECK(kindsOf("// only a comment", diags) == std::vector<TokenKind>{TokenKind::End});
}

TEST_CASE("lexer: lexical errors are diagnostics and the lexer carries on", "[unit]") {
    struct Case {
        std::string src;
        std::string_view code;
    };
    const std::vector<Case> cases = {
        {"'abc", codes::kQuoteMissing},         {"\"abc", codes::kQuoteMissing},
        {"1 /* open", codes::kCommentUnclosed}, {"'X'[km/h", codes::kUnitBracket},
        {"1 ]", codes::kUnitBracket},           {"1 = 2", codes::kUnexpectedChar},
        {"1 # 2", codes::kUnexpectedChar},      {"1 ; 2", codes::kUnexpectedChar},
        {"2x", codes::kNumberFormat},           {"1e", codes::kNumberFormat},
        {"1.5.2", codes::kNumberFormat},        {"\xE2\x80\x98X\xE2\x80\x99", codes::kUnexpectedChar}};
    for (const Case& c : cases) {
        INFO(c.src);
        Diagnostics diags;
        const std::vector<Token> t = Lexer::tokenize(c.src, diags);
        CHECK(t.back().kind == TokenKind::End);
        CHECK(hasCode(diags, c.code));
    }
    Diagnostics smart;
    (void)Lexer::tokenize("\xE2\x80\x98X\xE2\x80\x99", smart);
    REQUIRE_FALSE(smart.empty());
    CHECK(smart[0].hint.find("straight quote") != std::string::npos);
    CHECK(smart[0].span.end - smart[0].span.begin == 3); // one diagnostic per UTF-8 character
}

TEST_CASE("parser: binding powers of 03 section 2.3 (default: -2^2 = -4)", "[unit]") {
    const std::vector<std::pair<std::string, std::string>> table = {
        {"-2^2", "(neg (^ 2 2))"},
        {"2^3^2", "(^ 2 (^ 3 2))"},
        {"2^-3", "(^ 2 (neg 3))"},
        {"2^-3^2", "(^ 2 (neg (^ 3 2)))"},
        {"1 + 2 * 3", "(+ 1 (* 2 3))"},
        {"1 - 2 - 3", "(- (- 1 2) 3)"},
        {"8 / 4 / 2", "(/ (/ 8 4) 2)"},
        {"1 / 2 * 3", "(* (/ 1 2) 3)"},
        {"-a * b", "(* (neg 'a') 'b')"},
        {"a & b == c", "(& 'a' (== 'b' 'c'))"},
        {"a < b == c > d", "(== (< 'a' 'b') (> 'c' 'd'))"},
        {"a | b & c", "(| 'a' (& 'b' 'c'))"},
        {"a && b | c", "(&& 'a' (| 'b' 'c'))"},
        {"a || b && c", "(|| 'a' (&& 'b' 'c'))"},
        {"a + b < c * d", "(< (+ 'a' 'b') (* 'c' 'd'))"},
        {"!a == b", "(== (not 'a') 'b')"},
        {"~a & b", "(& (bnot 'a') 'b')"},
        {"+-2", "(plus (neg 2))"},
        {"(1 + 2) * 3", "(* (+ 1 2) 3)"},
        {"-(2^2)", "(neg (^ 2 2))"},
        {"(-2)^2", "(^ (neg 2) 2)"},
        {"'G Force Lat'[m/s/s] / ('Corr Speed'[m/s]^2)", "(/ 'G Force Lat'[m/s/s] (^ 'Corr Speed'[m/s] 2))"},
    };
    for (const auto& [src, want] : table) {
        INFO(src);
        // Bare words a..d are not channel references; quote them for the parser.
        std::string quoted;
        for (const char c : src) {
            if (c >= 'a' && c <= 'd' && src.find('\'') == std::string::npos) {
                quoted += '\'';
                quoted += c;
                quoted += '\'';
            } else {
                quoted += c;
            }
        }
        CHECK(sexprOf(quoted) == want);
    }
}

TEST_CASE("parser: unaryMinusAbovePower swaps the unary and power levels (-2^2 = 4)", "[unit]") {
    const MathsCompat on = compatOn();
    CHECK(sexprOf("-2^2", on) == "(^ (neg 2) 2)");
    CHECK(sexprOf("2^-3^2", on) == "(^ 2 (^ (neg 3) 2))");
    CHECK(sexprOf("2^3^2", on) == "(^ 2 (^ 3 2))");
    CHECK(sexprOf("-2 * 3", on) == "(* (neg 2) 3)");
    CHECK(sexprOf("-(2^2)", on) == "(neg (^ 2 2))");
    CHECK(sexprOf("'G'[m] / ('S'[m/s]^2)", on) == "(/ 'G'[m] (^ 'S'[m/s] 2))");
}

TEST_CASE("parser: I-COMPAT-POW once per affected expression, under both settings", "[unit]") {
    for (const bool on : {false, true}) {
        MathsCompat compat;
        compat.unaryMinusAbovePower = on;
        Diagnostics diags;
        REQUIRE(Parser::parse("-2^2 + -'x'^3", compat, diags).ok());
        CHECK(countCode(diags, codes::kCompatPow) == 1);
        const Diagnostic* d = findCode(diags, codes::kCompatPow);
        REQUIRE(d != nullptr);
        CHECK(d->level == Diagnostic::Info);
        CHECK(d->span.begin == 0);
        Diagnostics none;
        REQUIRE(Parser::parse("-(2^2) + (-2)^2 + 2^-3 + -2 * 3", compat, none).ok());
        CHECK_FALSE(hasCode(none, codes::kCompatPow));
    }
}

TEST_CASE("parser: word operators are the symbol operators", "[unit]") {
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"'a' AND 'b'", "'a' && 'b'"}, {"'a' or 'b'", "'a' || 'b'"},
        {"NOT 'a'", "!'a'"},           {"'a' LT 'b'", "'a' < 'b'"},
        {"'a' lte 'b'", "'a' <= 'b'"}, {"'a' Gt 'b'", "'a' > 'b'"},
        {"'a' GTE 'b'", "'a' >= 'b'"}, {"'a' eq 'b'", "'a' == 'b'"},
        {"'a' NEQ 'b'", "'a' != 'b'"}, {"NOT 'a' AND 'b' OR 'c'", "!'a' && 'b' || 'c'"}};
    for (const auto& [words, symbols] : pairs) {
        INFO(words);
        CHECK(sexprOf(words) == sexprOf(symbols));
        CHECK(print(parseOk(words)) == print(parseOk(symbols)));
    }
}

TEST_CASE("parser: references, units, calls, strings, keywords and comments", "[unit]") {
    CHECK(sexprOf("'Wheel Speed FL'[km/h]") == "'Wheel Speed FL'[km/h]");
    CHECK(sexprOf("'Wheel Speed FL' [ km/h ]") == "'Wheel Speed FL'[km/h]");
    CHECK(sexprOf("'Gaussian Weight'[]") == "'Gaussian Weight'[]");
    CHECK(sexprOf("smooth('X'[], 4)") == "(smooth 'X'[] 4)");
    CHECK(sexprOf("rand_val()") == "(rand_val)");
    CHECK(sexprOf("range_is(\"Outings:Laps:Track Sections:Default:Corners\")") ==
          "(range_is \"Outings:Laps:Track Sections:Default:Corners\")");
    CHECK(sexprOf("edge_delay('T' > 90.0, 1.0, RISE)") == "(edge_delay (> 'T' 90.0) 1.0 #rise)");
    CHECK(sexprOf("'Driver' == \"Joe\"") == "(== 'Driver' \"Joe\")");
    CHECK(sexprOf("mylib.rolling_rms('X', 10)") == "(mylib.rolling_rms 'X' 10)");
    CHECK(sexprOf("1 /* one */ + /* two */ 2 // done") == "(+ 1 2)");
}

TEST_CASE("parser: a channel name keeps its spelling; numbers keep their text", "[unit]") {
    const Ast ast = parseOk("'Engine RPM' * 1.50");
    CHECK(ast.nodes[0].text == "Engine RPM");
    CHECK(ast.nodes[1].text == "1.50");
    CHECK(ast.nodes[1].kind == NodeKind::DecLit);
    // Children precede their parents (the checker and compile() rely on it).
    for (std::size_t i = 0; i < ast.nodes.size(); ++i) {
        for (const std::uint32_t k : ast.nodes[i].kids) {
            CHECK(k < i);
        }
    }
    CHECK(ast.root == ast.nodes.size() - 1);
}

TEST_CASE("parser: function names are case-insensitive with W-STYLE-CASE, aliases rename", "[unit]") {
    Diagnostics diags;
    Result<Ast> ast = Parser::parse("ABS(-1) + Smooth('X', 3)", {}, diags);
    REQUIRE(ast.ok());
    CHECK(countCode(diags, codes::kStyleCase) == 2);
    CHECK(findCode(diags, codes::kStyleCase)->level == Diagnostic::Style);
    CHECK(sexpr(*ast) == "(+ (abs (neg 1)) (smooth 'X' 3))");

    MathsCompat compat;
    compat.functionAliases["min_running"] = "running_min";
    Diagnostics d2;
    Result<Ast> renamed = Parser::parse("MIN_RUNNING('X')", compat, d2);
    REQUIRE(renamed.ok());
    CHECK(sexpr(*renamed) == "(running_min 'X')");
    CHECK_FALSE(hasCode(d2, codes::kStyleCase));
}

TEST_CASE("parser: every syntax diagnostic code of 03 section 2.5 and ours", "[unit]") {
    struct Case {
        std::string src;
        std::string_view code;
    };
    const std::vector<Case> cases = {
        {"Engine RPM * 2", codes::kQuoteMissing},
        {"abs(Throttle)", codes::kQuoteMissing},
        {"'X' km/h", codes::kUnitBracket},
        {"'X' m/s/s + 1", codes::kUnitBracket},
        {"[km/h] * 2", codes::kUnitBracket},
        {"('A' + 'B')[m]", codes::kUnitBracket},
        {"'A'[m][km]", codes::kUnitBracket},
        {"(1 + 2", codes::kParenUnbalanced},
        {"1 + 2)", codes::kParenUnbalanced},
        {"abs(1", codes::kCallUnclosed},
        {"smooth('X', 5", codes::kCallUnclosed},
        {"max(1 2)", codes::kCommaMissing},
        {"max('A' 'B')", codes::kCommaMissing},
        {"1 +", codes::kSyntax},
        {"* 2", codes::kSyntax},
        {"1 2", codes::kSyntax},
        {"()", codes::kSyntax},
        {"max(1,)", codes::kSyntax},
        {"max(,1)", codes::kSyntax},
        {"(1 2)", codes::kSyntax},
        {"", codes::kExprEmpty},
        {"  /* nothing */ // here", codes::kExprEmpty},
        {"1 = 2", codes::kUnexpectedChar},
        {"1 /* open", codes::kCommentUnclosed},
        {"2x + 1", codes::kNumberFormat},
        {"1e999", codes::kNumberFormat},
        {"''", codes::kNameEmpty},
    };
    for (const Case& c : cases) {
        INFO(c.src << " -> " << c.code);
        const Diagnostics diags = parseFails(c.src);
        CHECK(hasCode(diags, c.code));
        for (const Diagnostic& d : diags) {
            CHECK(d.span.begin <= d.span.end);
            CHECK(d.span.end <= c.src.size());
            CHECK_FALSE(d.message.empty());
        }
    }
}

TEST_CASE("parser: E-QUOTE-MISSING and E-UNIT-BRACKET point at the words and suggest the fix", "[unit]") {
    const Diagnostics q = parseFails("Engine RPM * 2");
    const Diagnostic* d = findCode(q, codes::kQuoteMissing);
    REQUIRE(d != nullptr);
    CHECK(d->span.begin == 0);
    CHECK(d->span.end == 10);
    CHECK(d->hint == "write 'Engine RPM'");
    const Diagnostics u = parseFails("'Speed' km/h * 2");
    const Diagnostic* b = findCode(u, codes::kUnitBracket);
    REQUIRE(b != nullptr);
    CHECK(b->hint == "write 'Speed'[km/h]");
    CHECK(u.size() == 1); // recovered as 'Speed'[km/h]: no follow-on errors
}

TEST_CASE("parser: error recovery reports independent errors in one pass", "[unit]") {
    const Diagnostics diags = parseFails("max(1 2) + Throttle * (3 + ");
    CHECK(hasCode(diags, codes::kCommaMissing));
    CHECK(hasCode(diags, codes::kQuoteMissing));
    CHECK((hasCode(diags, codes::kSyntax) || hasCode(diags, codes::kParenUnbalanced)));
    // Diagnostics come in source order.
    for (std::size_t i = 1; i < diags.size(); ++i) {
        CHECK(diags[i - 1].span.begin <= diags[i].span.begin);
    }
    const Diagnostics two = parseFails("abs(1 + ) + smooth('X' 3");
    CHECK(hasCode(two, codes::kSyntax));
    CHECK(hasCode(two, codes::kCommaMissing));
    CHECK(hasCode(two, codes::kCallUnclosed));
}

TEST_CASE("parser: hostile nesting is refused with E-EXPR-TOO-DEEP, never a crash", "[unit]") {
    const std::string parens = std::string(5000, '(') + "1" + std::string(5000, ')');
    CHECK(hasCode(parseFails(parens), codes::kExprTooDeep));
    const std::string signs = std::string(100000, '-') + "1";
    CHECK(hasCode(parseFails(signs), codes::kExprTooDeep));
    std::string chain = "1";
    for (int i = 0; i < 2000; ++i) {
        chain += "+1";
    }
    CHECK(hasCode(parseFails(chain), codes::kExprTooDeep));
    std::string calls;
    for (int i = 0; i < 3000; ++i) {
        calls += "abs(";
    }
    CHECK(hasCode(parseFails(calls), codes::kExprTooDeep));
    std::string units = "1";
    for (int i = 0; i < 50000; ++i) {
        units += "[m]";
    }
    CHECK(hasCode(parseFails(units), codes::kUnitBracket));
    // A realistic nesting depth parses.
    std::string nested = "'x'";
    for (int i = 0; i < 60; ++i) {
        nested = "choose('c' > " + std::to_string(i) + ", " + nested + ", 0)";
    }
    CHECK(parseOk(nested).nodes.size() > 60);
}

TEST_CASE("print: the canonical form is idempotent and re-parses to the same tree", "[unit]") {
    const std::vector<std::string> corpus = {
        "'Corr Speed'[m/s] / 'Vehicle Wheelbase'[m] * tan('Steered Angle'[rad])  // reference",
        "choose('Oversteer'[deg] > 2, 2, choose('Oversteer'[deg] < -2, 0, 1))",
        "smooth(choose(abs('Steered Angle'[rad]) > 0.05, 'Yaw'[rad/s], invalid()), 0.2)",
        "-2^2",
        "(-2)^2",
        "-(2^2)",
        "2^-3^2",
        "2^3^2",
        "- -'a'",
        "+-2",
        "!!'a'",
        "~~'a'",
        "'a' - ('b' - 'c')",
        "'a' / ('b' * 'c')",
        "('a' + 'b') * 'c'",
        "'a' & 'b' == 'c'",
        "('a' & 'b') == 'c'",
        "'a' AND NOT 'b' OR 'c' NEQ 1.50e3",
        "stat_start('Speed'[km/h], 'Throttle'[%] > 10, "
        "range_change(\"Outings:Laps:Track Sections:Default:Corners\"))",
        "edge_delay('Throttle Position'[%] > 90.0, 1.0, rise)",
        "'Driver' == \"Joe\"",
        "'X'[]",
        "rand_chan()",
        "integrate('Speed'[m/s], 'Speed'[km/h] > 10, range_change(\"Outings:Laps\"))",
        "bit_decode(int('Status'), 3, 2) | bit_and('A', 255)",
        "'a' < 'b' == 'c' >= 'd' <= 'e' > 'f'",
        "1 - -1",
        "2 * -'x'^2",
        "'a' || 'b' && 'c' | 'd' & 'e'",
        ".5 + 1. + 1e3 + 007"};
    for (const std::string& src : corpus) {
        INFO(src);
        for (const bool on : {false, true}) {
            MathsCompat compat;
            compat.unaryMinusAbovePower = on;
            const Ast first = parseOk(src, compat);
            const std::string printed = print(first);
            const Ast second = parseOk(printed, compat);
            CHECK(print(second) == printed);
            CHECK(sexpr(second) == sexpr(first));
            // The printed form reads the same under the other setting too (no compat-dependent text).
            MathsCompat other;
            other.unaryMinusAbovePower = !on;
            CHECK(sexpr(parseOk(printed, other)) == sexpr(first));
        }
    }
    CHECK(print(parseOk("1+2*3")) == "1 + 2 * 3");
    CHECK(print(parseOk("(1+2)*3")) == "(1 + 2) * 3");
    CHECK(print(parseOk("-2^2")) == "-(2^2)");
    CHECK(print(parseOk("-2^2", compatOn())) == "(-2)^2");
    CHECK(print(parseOk("'a' and not 'b'")) == "'a' && !'b'");
    CHECK(print(parseOk("ABS( 'x' [ km/h ] )")) == "abs('x'[km/h])");
    CHECK(print(parseOk("- -1")) == "- -1");
}

TEST_CASE("Ast::sourceHash ignores comments, white space and name case, nothing else", "[unit]") {
    const Hash128 h = parseOk("'Engine RPM'[rpm] * 2").sourceHash;
    CHECK(parseOk("  'engine rpm' [rpm]*2 // twice").sourceHash == h);
    CHECK(parseOk("/* c */ 'ENGINE RPM'[rpm]\n*\n2").sourceHash == h);
    CHECK(parseOk("'Engine RPM'[rpm] * 3").sourceHash != h);
    CHECK(parseOk("'Engine RPM'[rad/s] * 2").sourceHash != h);
    CHECK(parseOk("'Engine RPM'[rpm] * 2.0").sourceHash != h); // a decimal is not an integer (windows)
    CHECK(parseOk("'Engine RPM' * 2").sourceHash != h);
    CHECK(parseOk("ABS('x')").sourceHash == parseOk("abs('x')").sourceHash);
    CHECK(parseOk("'a' AND 'b'").sourceHash == parseOk("'a' && 'b'").sourceHash);
}
