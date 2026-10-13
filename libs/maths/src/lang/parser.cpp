// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Parser (03 sections 2.2-2.5, D-020): a Pratt parser over the Lexer's tokens with the binding powers of 03
// section 2.3, and print(), the canonical form (03 section 3.1).
//
// Every node is appended after its children, so Ast::nodes is in post-order (a child's index is always below
// its parent's): the type checker and compile() walk the array forwards without recursion. The depth of the
// tree and the parser's recursion are bounded (E-EXPR-TOO-DEEP), so hostile input cannot exhaust the stack.
//
// Error recovery: the parser reports a diagnostic and carries on with a placeholder, a recovered node
// (`Engine RPM` becomes the reference 'Engine RPM' after E-QUOTE-MISSING, `'X' km/h` the conversion
// 'X'[km/h] after E-UNIT-BRACKET) or by skipping to the next ')' or ',', so one pass reports every
// independent error. parse() fails when any Error was reported.

#include "codes.hpp"
#include "lang_util.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/hash.hpp>
#include <esses/maths/lexer.hpp>
#include <esses/maths/parser.hpp>

#include <fast_float/fast_float.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace esses::maths {

namespace {

using lang::join;
using lang::makeDiag;

// Bounds that keep every recursive walk of a parsed tree far from the stack limit (03 section 14.3: fuzzing
// must not crash). Real expressions stay below a depth of 40.
constexpr std::size_t kMaxDepth = 256;
constexpr int kMaxRecursion = 256;

// Binding powers of 03 section 2.3.
constexpr int kPowLeft = 100;
constexpr int kPowRight = 95;
constexpr int kUnaryRight = 90;
constexpr int kUnaryRightCompat = 105; // MathsCompat::unaryMinusAbovePower: unary binds above '^'

struct Infix {
    BinaryOp op;
    int left;
    int right;
};

bool infixOf(const Token& t, Infix& out) {
    if (t.kind == TokenKind::KeywordOp) {
        const std::string& w = t.text;
        if (w == "OR") {
            out = {BinaryOp::Or, 10, 11};
        } else if (w == "AND") {
            out = {BinaryOp::And, 20, 21};
        } else if (w == "EQ") {
            out = {BinaryOp::Eq, 50, 51};
        } else if (w == "NEQ") {
            out = {BinaryOp::Ne, 50, 51};
        } else if (w == "LT") {
            out = {BinaryOp::Lt, 60, 61};
        } else if (w == "LTE") {
            out = {BinaryOp::Le, 60, 61};
        } else if (w == "GT") {
            out = {BinaryOp::Gt, 60, 61};
        } else if (w == "GTE") {
            out = {BinaryOp::Ge, 60, 61};
        } else {
            return false; // NOT is prefix only
        }
        return true;
    }
    if (t.kind != TokenKind::Operator) {
        return false;
    }
    const std::string& s = t.text;
    if (s == "||") {
        out = {BinaryOp::Or, 10, 11};
    } else if (s == "&&") {
        out = {BinaryOp::And, 20, 21};
    } else if (s == "|") {
        out = {BinaryOp::BitOr, 30, 31};
    } else if (s == "&") {
        out = {BinaryOp::BitAnd, 40, 41};
    } else if (s == "==") {
        out = {BinaryOp::Eq, 50, 51};
    } else if (s == "!=") {
        out = {BinaryOp::Ne, 50, 51};
    } else if (s == "<") {
        out = {BinaryOp::Lt, 60, 61};
    } else if (s == "<=") {
        out = {BinaryOp::Le, 60, 61};
    } else if (s == ">") {
        out = {BinaryOp::Gt, 60, 61};
    } else if (s == ">=") {
        out = {BinaryOp::Ge, 60, 61};
    } else if (s == "+") {
        out = {BinaryOp::Add, 70, 71};
    } else if (s == "-") {
        out = {BinaryOp::Sub, 70, 71};
    } else if (s == "*") {
        out = {BinaryOp::Mul, 80, 81};
    } else if (s == "/") {
        out = {BinaryOp::Div, 80, 81};
    } else if (s == "^") {
        out = {BinaryOp::Pow, kPowLeft, kPowRight};
    } else {
        return false;
    }
    return true;
}

bool prefixOf(const Token& t, UnaryOp& out) {
    if (t.kind == TokenKind::KeywordOp && t.text == "NOT") {
        out = UnaryOp::Not;
        return true;
    }
    if (t.kind != TokenKind::Operator) {
        return false;
    }
    if (t.text == "-") {
        out = UnaryOp::Neg;
    } else if (t.text == "+") {
        out = UnaryOp::Plus;
    } else if (t.text == "!") {
        out = UnaryOp::Not;
    } else if (t.text == "~") {
        out = UnaryOp::BitNot;
    } else {
        return false;
    }
    return true;
}

// Tokens that can begin an operand.
bool startsOperand(const Token& t) {
    switch (t.kind) {
    case TokenKind::ChannelRef:
    case TokenKind::IntLit:
    case TokenKind::DecLit:
    case TokenKind::StrLit:
    case TokenKind::Ident:
    case TokenKind::KeywordConst:
    case TokenKind::LParen:
        return true;
    case TokenKind::KeywordOp:
        return t.text == "NOT";
    case TokenKind::Operator: {
        UnaryOp u{};
        return prefixOf(t, u);
    }
    default:
        return false;
    }
}

std::string describe(const Token& t) {
    switch (t.kind) {
    case TokenKind::End:
        return "the end of the expression";
    case TokenKind::ChannelRef:
        return "'" + t.text + "'";
    case TokenKind::UnitSuffix:
        return "[" + t.text + "]";
    case TokenKind::StrLit:
        return "\"" + t.text + "\"";
    case TokenKind::LParen:
        return "'('";
    case TokenKind::RParen:
        return "')'";
    case TokenKind::Comma:
        return "','";
    default:
        return "'" + t.text + "'";
    }
}

double numberValue(const std::string& literal) {
    const std::string text = !literal.empty() && literal.front() == '.' ? "0" + literal : literal;
    double v = 0.0;
    const auto r = fast_float::from_chars(text.data(), text.data() + text.size(), v);
    if (r.ec != std::errc()) {
        return std::nan("");
    }
    return v;
}

class ParserImpl {
public:
    ParserImpl(std::string_view src, const MathsCompat& compat, Diagnostics& diags)
        : compat_(compat), diags_(diags) {
        tokens_ = Lexer::tokenize(src, diags_);
        ast_.source = std::string(src);
    }

    Ast run() {
        if (peek().kind == TokenKind::End) {
            report(codes::kExprEmpty, peek().span, "the expression is empty",
                   "write an expression such as 'Speed'[km/h] * 2");
            ast_.root = placeholder(peek().span);
            return std::move(ast_);
        }
        std::uint32_t root = parseExpr(0, 0);
        // Whatever follows a complete expression is an error; keep parsing the rest for more diagnostics.
        while (!aborted_ && peek().kind != TokenKind::End) {
            const Token& t = peek();
            if (t.kind == TokenKind::RParen) {
                report(codes::kParenUnbalanced, t.span, "')' without a matching '('",
                       "remove it or add a '('");
                ++pos_;
                continue;
            }
            if (startsOperand(t)) {
                report(codes::kSyntax, t.span, "missing operator before " + describe(t),
                       "join the parts with an operator such as + or *");
                const std::size_t before = pos_;
                (void)parseExpr(0, 0);
                if (pos_ == before) {
                    ++pos_;
                }
            } else {
                report(codes::kSyntax, t.span, "unexpected " + describe(t));
                ++pos_;
            }
        }
        ast_.root = root;
        return std::move(ast_);
    }

private:
    // ---- tokens and nodes --------------------------------------------------------------------------------

    const Token& peek(std::size_t ahead = 0) const {
        const std::size_t i = std::min(pos_ + ahead, tokens_.size() - 1);
        return tokens_[i];
    }
    const Token& take() {
        const Token& t = tokens_[pos_];
        if (pos_ + 1 < tokens_.size()) {
            ++pos_;
        }
        return t;
    }

    void report(std::string_view code, Span span, std::string message, std::string hint = {}) {
        diags_.push_back(makeDiag(code, Diagnostic::Error, span, std::move(message), std::move(hint)));
    }

    std::uint32_t add(Node node) {
        std::size_t depth = 1;
        for (const std::uint32_t k : node.kids) {
            depth = std::max(depth, depth_[k] + 1);
        }
        if (depth > kMaxDepth) {
            tooDeep(node.span);
        }
        ast_.nodes.push_back(std::move(node));
        depth_.push_back(depth);
        paren_.push_back(false);
        return static_cast<std::uint32_t>(ast_.nodes.size() - 1);
    }

    void tooDeep(Span span) {
        if (!aborted_) {
            report(codes::kExprTooDeep, span, "the expression is nested too deeply",
                   "split it into several maths items");
            aborted_ = true;
        }
    }

    // Stands in for an operand that could not be parsed; the parse has failed anyway.
    std::uint32_t placeholder(Span span) {
        Node n;
        n.kind = NodeKind::IntLit;
        n.span = span;
        n.text = "0";
        return add(std::move(n));
    }

    // ---- expressions -------------------------------------------------------------------------------------

    std::uint32_t parseExpr(int minBp, int level) {
        if (level > kMaxRecursion) {
            tooDeep(peek().span);
        }
        if (aborted_) {
            return placeholder(peek().span);
        }
        std::uint32_t lhs = parsePrefix(level);
        for (;;) {
            if (aborted_) {
                return lhs;
            }
            if (peek().kind == TokenKind::UnitSuffix) {
                // ('A' + 'B')[m], f(x)[m]: a unit belongs directly after a channel name.
                skipStrayUnits();
                continue;
            }
            Infix in{};
            if (!infixOf(peek(), in) || in.left <= minBp) {
                return lhs;
            }
            const Token& opTok = take();
            const Span opSpan = opTok.span;
            if (in.op == BinaryOp::Pow && compat_.unaryMinusAbovePower && isBareUnary(lhs)) {
                noteCompat(ast_.nodes[lhs].span);
            }
            const std::uint32_t rhs =
                (!startsOperand(peek()) && peek().kind != TokenKind::UnitSuffix)
                    ? missingOperand(opTok, opSpan, "operator '", "' has no right operand")
                    : parseExpr(in.right, level + 1);
            lhs = addBinary(in.op, lhs, rhs);
        }
    }

    // The node builders and error reports live outside the recursive functions so that their frames stay
    // small (the recursion depth is bounded by kMaxRecursion).
    std::uint32_t missingOperand(const Token& opTok, Span opSpan, const char* before, const char* after) {
        report(codes::kSyntax, peek().kind == TokenKind::End ? opSpan : peek().span,
               before + opTok.text + after);
        return placeholder(peek().span);
    }

    std::uint32_t addBinary(BinaryOp op, std::uint32_t lhs, std::uint32_t rhs) {
        Node n;
        n.kind = NodeKind::Binary;
        n.bop = op;
        n.span = join(ast_.nodes[lhs].span, ast_.nodes[rhs].span);
        n.kids = {lhs, rhs};
        return add(std::move(n));
    }

    std::uint32_t addUnary(UnaryOp op, Span opSpan, std::uint32_t operand) {
        Node n;
        n.kind = NodeKind::Unary;
        n.uop = op;
        n.span = join(opSpan, ast_.nodes[operand].span);
        n.kids = {operand};
        return add(std::move(n));
    }

    std::uint32_t addLeaf(const Token& tok) {
        Node n;
        n.span = tok.span;
        n.text = tok.text;
        switch (tok.kind) {
        case TokenKind::IntLit:
        case TokenKind::DecLit:
            n.kind = tok.kind == TokenKind::IntLit ? NodeKind::IntLit : NodeKind::DecLit;
            n.number = numberValue(tok.text);
            if (!std::isfinite(n.number)) {
                report(codes::kNumberFormat, tok.span, "number '" + tok.text + "' is out of range");
                n.number = 0.0;
            }
            break;
        case TokenKind::StrLit:
            n.kind = NodeKind::StrLit;
            break;
        default:
            n.kind = NodeKind::Keyword;
            break;
        }
        return add(std::move(n));
    }

    bool isBareUnary(std::uint32_t i) const { return ast_.nodes[i].kind == NodeKind::Unary && !paren_[i]; }
    bool isBarePow(std::uint32_t i) const {
        return ast_.nodes[i].kind == NodeKind::Binary && ast_.nodes[i].bop == BinaryOp::Pow && !paren_[i];
    }

    // I-COMPAT-POW (03 section 2.3): once per expression whose reading depends on unaryMinusAbovePower.
    void noteCompat(Span span) {
        if (compatNoted_) {
            return;
        }
        compatNoted_ = true;
        diags_.push_back(makeDiag(
            codes::kCompatPow, Diagnostic::Info, span,
            compat_.unaryMinusAbovePower
                ? "a sign before a power applies to the base here: -2^2 is 4 (unaryMinusAbovePower is on)"
                : "a sign before a power applies to the whole power: -2^2 is -4",
            "write (-a)^b or -(a^b) to make the intent explicit; the project option "
            "maths.compat.unaryMinusAbovePower selects the reading"));
    }

    std::uint32_t parsePrefix(int level) {
        skipStrayUnits();
        const Token& t = peek();
        UnaryOp uop{};
        if (prefixOf(t, uop)) {
            const Token& opTok = take();
            const Span opSpan = opTok.span;
            const std::uint32_t operand =
                (!startsOperand(peek()) && peek().kind != TokenKind::UnitSuffix)
                    ? missingOperand(opTok, opSpan, "'", "' has no operand")
                    : parseExpr(compat_.unaryMinusAbovePower ? kUnaryRightCompat : kUnaryRight, level + 1);
            if (!compat_.unaryMinusAbovePower && isBarePow(operand)) {
                noteCompat(join(opSpan, ast_.nodes[operand].span));
            }
            return addUnary(uop, opSpan, operand);
        }

        switch (t.kind) {
        case TokenKind::IntLit:
        case TokenKind::DecLit:
        case TokenKind::StrLit:
        case TokenKind::KeywordConst:
            return addLeaf(take());
        case TokenKind::ChannelRef:
            return parseReference(take());
        case TokenKind::Ident:
            if (peek(1).kind == TokenKind::LParen) {
                return parseCall(level);
            }
            return parseBareWords();
        case TokenKind::LParen:
            return parseGroup(level);
        default:
            break;
        }

        return noOperand(t);
    }

    void skipStrayUnits() {
        while (peek().kind == TokenKind::UnitSuffix) {
            report(codes::kUnitBracket, peek().span,
                   "a unit [" + peek().text + "] must follow a channel name",
                   "write 'Name'[" + peek().text + "]");
            (void)take();
        }
    }

    // No operand here. An infix operator or a closing token is left for the caller.
    std::uint32_t noOperand(const Token& t) {
        Infix in{};
        if (infixOf(t, in)) {
            report(codes::kSyntax, t.span, "operator " + describe(t) + " has no left operand");
        } else if (t.kind == TokenKind::End) {
            report(codes::kSyntax, t.span, "the expression is incomplete");
        } else {
            report(codes::kSyntax, t.span, "expected a value before " + describe(t));
        }
        return placeholder(t.span);
    }

    // 'Name', 'Name'[unit], and the E-UNIT-BRACKET recovery for 'Name' km/h.
    std::uint32_t parseReference(const Token& ref) {
        Node n;
        n.kind = NodeKind::ChannelRef;
        n.span = ref.span;
        n.text = ref.text;
        if (ref.text.empty()) {
            report(codes::kNameEmpty, ref.span, "the channel name between the quotes is empty");
        }
        const std::uint32_t refIndex = add(std::move(n));
        if (peek().kind == TokenKind::UnitSuffix) {
            const Token& u = take();
            const std::uint32_t conv = convert(refIndex, u.text, u.span);
            if (peek().kind == TokenKind::UnitSuffix) {
                report(codes::kUnitBracket, peek().span, "a channel takes one unit only");
                (void)take();
            }
            return conv;
        }
        if (peek().kind == TokenKind::Ident && peek(1).kind != TokenKind::LParen) {
            // 'X' km/h: a unit-like run of words joined by / * ^ and digits.
            const Span first = peek().span;
            Span last = first;
            (void)take();
            for (;;) {
                const Token& a = peek();
                const Token& b = peek(1);
                const bool joiner =
                    a.kind == TokenKind::Operator && (a.text == "/" || a.text == "*" || a.text == "^");
                if (joiner && (b.kind == TokenKind::Ident || b.kind == TokenKind::IntLit) &&
                    a.span.begin == last.end && b.span.begin == a.span.end) {
                    last = b.span;
                    pos_ += 2;
                } else if (a.kind == TokenKind::Ident && peek(1).kind != TokenKind::LParen) {
                    last = a.span;
                    ++pos_;
                } else {
                    break;
                }
            }
            const std::string unit = ast_.source.substr(first.begin, last.end - first.begin);
            report(codes::kUnitBracket, join(first, last), "the unit '" + unit + "' must be in brackets",
                   "write '" + ast_.nodes[refIndex].text + "'[" + unit + "]");
            return convert(refIndex, unit, join(first, last));
        }
        return refIndex;
    }

    std::uint32_t convert(std::uint32_t ref, std::string unit, Span unitSpan) {
        Node c;
        c.kind = NodeKind::Convert;
        c.text = std::move(unit);
        c.span = join(ast_.nodes[ref].span, unitSpan);
        c.kids = {ref};
        return add(std::move(c));
    }

    // Engine RPM * 2: words that are not a function call are most likely a channel name without its quotes.
    std::uint32_t parseBareWords() {
        const Span first = peek().span;
        Span last = first;
        (void)take();
        while ((peek().kind == TokenKind::Ident && peek(1).kind != TokenKind::LParen) ||
               peek().kind == TokenKind::IntLit) {
            last = peek().span;
            ++pos_;
        }
        const std::string words = ast_.source.substr(first.begin, last.end - first.begin);
        report(codes::kQuoteMissing, join(first, last),
               "'" + words + "' is not a function; a channel name must be in single quotes",
               "write '" + words + "'");
        Token ref;
        ref.kind = TokenKind::ChannelRef;
        ref.span = join(first, last);
        ref.text = words;
        return parseReference(ref);
    }

    std::uint32_t parseGroup(int level) {
        const Token& open = take();
        const Span openSpan = open.span;
        if (peek().kind == TokenKind::RParen) {
            report(codes::kSyntax, join(openSpan, peek().span), "empty parentheses");
            (void)take();
            return placeholder(openSpan);
        }
        const std::uint32_t inner = parseExpr(0, level + 1);
        expectClose(openSpan, codes::kParenUnbalanced, "'(' has no matching ')'", "add the missing ')'");
        if (inner < paren_.size()) {
            paren_[inner] = true;
        }
        return inner;
    }

    // At a closing parenthesis: consume it, or report and skip to the matching one (or the end).
    void expectClose(Span openSpan, std::string_view codeAtEnd, const std::string& message,
                     const std::string& hint) {
        if (aborted_) {
            return;
        }
        if (peek().kind == TokenKind::RParen) {
            (void)take();
            return;
        }
        if (peek().kind == TokenKind::End) {
            report(codeAtEnd, openSpan, message, hint);
            return;
        }
        if (startsOperand(peek())) {
            report(codes::kSyntax, peek().span, "missing operator before " + describe(peek()),
                   "join the parts with an operator such as + or *");
        } else {
            report(codes::kSyntax, peek().span, "unexpected " + describe(peek()) + "; expected ')'");
        }
        int nesting = 0;
        while (peek().kind != TokenKind::End) {
            const TokenKind k = take().kind;
            if (k == TokenKind::LParen) {
                ++nesting;
            } else if (k == TokenKind::RParen) {
                if (nesting == 0) {
                    return;
                }
                --nesting;
            }
        }
        report(codeAtEnd, openSpan, message, hint);
    }

    std::uint32_t parseCall(int level) {
        const std::size_t nameToken = pos_;
        (void)take(); // the name
        (void)take(); // '('
        CallState call;
        if (peek().kind == TokenKind::RParen) {
            call.end = take().span;
            call.closed = true;
        }
        while (!call.closed && !aborted_ && peek().kind != TokenKind::End) {
            const bool missing = peek().kind == TokenKind::Comma || peek().kind == TokenKind::RParen;
            call.args.push_back(missing ? missingArgument() : parseExpr(0, level + 1));
            if (aborted_ || !afterArgument(call, nameToken)) {
                break;
            }
        }
        return finishCall(call, nameToken);
    }

    struct CallState {
        std::vector<std::uint32_t> args;
        Span end;
        bool closed = false;
    };

    std::uint32_t missingArgument() {
        report(codes::kSyntax, peek().span, "missing argument before " + describe(peek()));
        return placeholder(peek().span);
    }

    // After an argument: ',' continues, ')' closes, the end stops; anything else is reported (a missing comma
    // when it can start the next argument). False when the argument list ends here.
    bool afterArgument(CallState& call, std::size_t nameToken) {
        for (;;) {
            const Token& t = peek();
            if (t.kind == TokenKind::Comma) {
                (void)take();
                if (peek().kind == TokenKind::RParen) {
                    report(codes::kSyntax, peek().span, "missing argument after ','");
                    call.end = take().span;
                    call.closed = true;
                    return false;
                }
                return true;
            }
            if (t.kind == TokenKind::RParen) {
                call.end = take().span;
                call.closed = true;
                return false;
            }
            if (t.kind == TokenKind::End) {
                return false;
            }
            if (startsOperand(t)) {
                const Span prev = ast_.nodes[call.args.back()].span;
                report(codes::kCommaMissing, Span{prev.end, t.span.begin},
                       "missing ',' between the arguments of " + tokens_[nameToken].text + "()",
                       "separate arguments with ','");
                return true;
            }
            report(codes::kSyntax, t.span,
                   "unexpected " + describe(t) + " in the arguments of " + tokens_[nameToken].text + "()");
            (void)take();
        }
    }

    std::uint32_t finishCall(CallState& call, std::size_t nameToken) {
        const Token& name = tokens_[nameToken];
        if (!call.closed && !aborted_) {
            report(codes::kCallUnclosed, name.span, name.text + "( has no closing ')'",
                   "add ')' after the last argument");
        }
        Span end = call.end;
        if (!call.closed) {
            end = call.args.empty() ? name.span : ast_.nodes[call.args.back()].span;
        }
        Node n;
        n.kind = NodeKind::Call;
        n.span = join(name.span, end);
        n.kids = std::move(call.args);
        n.text = canonicalName(name.text, name.span);
        return add(std::move(n));
    }

    // Function names are case-insensitive; the canonical spelling is lower case (W-STYLE-CASE otherwise), and
    // MathsCompat::functionAliases renames alternative spellings before the registry lookup (D-080).
    std::string canonicalName(const std::string& spelled, Span span) {
        std::string lower = lang::toLowerAscii(spelled);
        const auto alias = compat_.functionAliases.find(caseFold(spelled));
        if (alias != compat_.functionAliases.end()) {
            return alias->second;
        }
        if (lower != spelled) {
            diags_.push_back(makeDiag(codes::kStyleCase, Diagnostic::Style, span,
                                      "function names are written in lower case: " + lower,
                                      "write " + lower + "(...)"));
        }
        return lower;
    }

    const MathsCompat& compat_;
    Diagnostics& diags_;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    Ast ast_;
    std::vector<std::size_t> depth_;
    std::vector<bool> paren_;
    bool aborted_ = false;
    bool compatNoted_ = false;
};

// ---- print
// ---------------------------------------------------------------------------------------------------

int levelOf(const Node& n) {
    switch (n.kind) {
    case NodeKind::Unary:
        return 9;
    case NodeKind::Binary:
        switch (n.bop) {
        case BinaryOp::Pow:
            return 10;
        case BinaryOp::Mul:
        case BinaryOp::Div:
            return 8;
        case BinaryOp::Add:
        case BinaryOp::Sub:
            return 7;
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
            return 6;
        case BinaryOp::Eq:
        case BinaryOp::Ne:
            return 5;
        case BinaryOp::BitAnd:
            return 4;
        case BinaryOp::BitOr:
            return 3;
        case BinaryOp::And:
            return 2;
        case BinaryOp::Or:
            return 1;
        }
        return 0;
    default:
        return 12;
    }
}

std::string_view binarySymbol(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add:
        return "+";
    case BinaryOp::Sub:
        return "-";
    case BinaryOp::Mul:
        return "*";
    case BinaryOp::Div:
        return "/";
    case BinaryOp::Pow:
        return "^";
    case BinaryOp::Lt:
        return "<";
    case BinaryOp::Le:
        return "<=";
    case BinaryOp::Gt:
        return ">";
    case BinaryOp::Ge:
        return ">=";
    case BinaryOp::Eq:
        return "==";
    case BinaryOp::Ne:
        return "!=";
    case BinaryOp::BitAnd:
        return "&";
    case BinaryOp::BitOr:
        return "|";
    case BinaryOp::And:
        return "&&";
    case BinaryOp::Or:
        return "||";
    }
    return "?";
}

std::string_view unarySymbol(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg:
        return "-";
    case UnaryOp::Plus:
        return "+";
    case UnaryOp::Not:
        return "!";
    case UnaryOp::BitNot:
        return "~";
    }
    return "?";
}

class Printer {
public:
    Printer(const Ast& ast, bool foldNames) : ast_(ast), fold_(foldNames) {}

    void node(std::uint32_t i, std::string& out) const {
        if (i >= ast_.nodes.size()) {
            return;
        }
        const Node& n = ast_.nodes[i];
        switch (n.kind) {
        case NodeKind::IntLit:
        case NodeKind::DecLit:
        case NodeKind::Keyword:
            out += n.text;
            return;
        case NodeKind::StrLit:
            out += '"';
            out += n.text;
            out += '"';
            return;
        case NodeKind::ChannelRef:
            out += '\'';
            out += fold_ ? caseFold(n.text) : n.text;
            out += '\'';
            return;
        case NodeKind::Convert:
            if (!n.kids.empty()) {
                node(n.kids[0], out);
            }
            out += '[';
            out += n.text;
            out += ']';
            return;
        case NodeKind::Call:
            out += n.text;
            out += '(';
            for (std::size_t k = 0; k < n.kids.size(); ++k) {
                if (k > 0) {
                    out += ", ";
                }
                node(n.kids[k], out);
            }
            out += ')';
            return;
        case NodeKind::Unary: {
            out += unarySymbol(n.uop);
            if (n.kids.empty()) {
                return;
            }
            const Node& kid = ast_.nodes[n.kids[0]];
            const bool paren = levelOf(kid) < 9 || levelOf(kid) == 10;
            std::string inner;
            child(n.kids[0], paren, inner);
            // `- -a` rather than `--a`, which reads like a decrement.
            if (!inner.empty() && (inner.front() == '-' || inner.front() == '+') && n.uop != UnaryOp::Not &&
                n.uop != UnaryOp::BitNot) {
                out += ' ';
            }
            out += inner;
            return;
        }
        case NodeKind::Binary: {
            if (n.kids.size() != 2) {
                return;
            }
            const int level = levelOf(n);
            const int left = levelOf(ast_.nodes[n.kids[0]]);
            const int right = levelOf(ast_.nodes[n.kids[1]]);
            if (n.bop == BinaryOp::Pow) {
                // Right-associative; a signed base is always parenthesised and a signed exponent never needs
                // it, so the text reads the same under both settings of unaryMinusAbovePower.
                child(n.kids[0], left <= 10, out);
                out += '^';
                child(n.kids[1], right < 9, out);
                return;
            }
            child(n.kids[0], left < level, out);
            out += ' ';
            out += binarySymbol(n.bop);
            out += ' ';
            child(n.kids[1], right <= level, out);
            return;
        }
        }
    }

private:
    void child(std::uint32_t i, bool paren, std::string& out) const {
        if (paren) {
            out += '(';
        }
        node(i, out);
        if (paren) {
            out += ')';
        }
    }

    const Ast& ast_;
    bool fold_;
};

std::string printImpl(const Ast& ast, bool foldNames) {
    std::string out;
    if (ast.nodes.empty()) {
        return out;
    }
    Printer(ast, foldNames).node(ast.root, out);
    return out;
}

} // namespace

Result<Ast> Parser::parse(std::string_view src, const MathsCompat& compat, Diagnostics& diags) {
    Diagnostics local;
    ParserImpl impl(src, compat, local);
    Ast ast = impl.run();
    std::stable_sort(local.begin(), local.end(),
                     [](const Diagnostic& a, const Diagnostic& b) { return a.span.begin < b.span.begin; });
    const auto firstError = std::find_if(local.begin(), local.end(),
                                         [](const Diagnostic& d) { return d.level == Diagnostic::Error; });
    if (firstError != local.end()) {
        ErrorContext ctx;
        ctx.span = firstError->span;
        ctx.detail = firstError->code;
        std::string message = firstError->code + ": " + firstError->message;
        diags.insert(diags.end(), std::make_move_iterator(local.begin()),
                     std::make_move_iterator(local.end()));
        return fail(ErrorCode::Parse, std::move(message), std::move(ctx));
    }
    diags.insert(diags.end(), std::make_move_iterator(local.begin()), std::make_move_iterator(local.end()));
    // 03 section 7.2: the definition hash ignores comments, white space and the case of names.
    ast.sourceHash = hash128(printImpl(ast, true));
    return ast;
}

std::string print(const Ast& ast) {
    return printImpl(ast, false);
}

} // namespace esses::maths
