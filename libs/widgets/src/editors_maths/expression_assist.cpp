// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "expression_assist.hpp"

#include <QByteArray>
#include <QStringList>
#include <algorithm>
#include <string>

namespace esses::widgets::assist {

namespace {

// Characters that end the search for a unit bracket: a unit symbol never contains them.
bool breaksUnit(QChar c) {
    return c == QLatin1Char('\'') || c == QLatin1Char('\n') || c == QLatin1Char('(') ||
           c == QLatin1Char(')') || c == QLatin1Char(',') || c == QLatin1Char('"');
}

} // namespace

std::uint32_t byteOffset(const QString& text, int pos) {
    const int clamped = std::clamp(pos, 0, static_cast<int>(text.size()));
    return static_cast<std::uint32_t>(text.left(clamped).toUtf8().size());
}

int charPosition(const QString& text, std::uint32_t byte) {
    const QByteArray utf8 = text.toUtf8();
    const auto end = std::min<qsizetype>(static_cast<qsizetype>(byte), utf8.size());
    qsizetype start = end;
    // Back up to the first byte of a code point (continuation bytes are 10xxxxxx).
    while (start > 0 && start < utf8.size() && (static_cast<unsigned char>(utf8[start]) & 0xC0U) == 0x80U) {
        --start;
    }
    return static_cast<int>(QString::fromUtf8(utf8.left(start)).size());
}

std::optional<UnitBracket> unitBracketAt(const QString& text, int position) {
    const int pos = std::clamp(position, 0, static_cast<int>(text.size()));
    int open = -1;
    for (int i = pos - 1; i >= 0; --i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('[')) {
            open = i;
            break;
        }
        if (c == QLatin1Char(']') || breaksUnit(c)) {
            return std::nullopt;
        }
    }
    if (open < 0) {
        return std::nullopt;
    }
    // 'Name'[ or 'Name' [ (blanks allowed between the reference and its unit, 03 section 2.1).
    int quoteEnd = open - 1;
    while (quoteEnd >= 0 &&
           (text.at(quoteEnd) == QLatin1Char(' ') || text.at(quoteEnd) == QLatin1Char('\t'))) {
        --quoteEnd;
    }
    if (quoteEnd <= 0 || text.at(quoteEnd) != QLatin1Char('\'')) {
        return std::nullopt;
    }
    const int quoteStart = static_cast<int>(text.lastIndexOf(QLatin1Char('\''), quoteEnd - 1));
    if (quoteStart < 0) {
        return std::nullopt;
    }
    // The quote at quoteStart opens a reference when an even number of quotes precede it.
    if (text.left(quoteStart).count(QLatin1Char('\'')) % 2 != 0) {
        return std::nullopt;
    }
    UnitBracket b;
    b.channel = text.mid(quoteStart + 1, quoteEnd - quoteStart - 1);
    b.open = open;
    b.typed = text.mid(open + 1, pos - open - 1);
    for (int i = pos; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char(']')) {
            b.close = i;
            break;
        }
        if (c == QLatin1Char('[') || breaksUnit(c)) {
            break;
        }
    }
    if (b.channel.isEmpty()) {
        return std::nullopt;
    }
    return b;
}

Edit insertUnit(const QString& text, const UnitBracket& bracket, const QString& symbol) {
    Edit e;
    const int from = bracket.open + 1;
    if (bracket.close >= 0) {
        e.text = text.left(from) + symbol + text.mid(bracket.close);
        e.caret = from + static_cast<int>(symbol.size()) + 1;
        return e;
    }
    // No closing bracket yet: replace what was typed after '[' (up to the next blank or operator) and close.
    int end = from + static_cast<int>(bracket.typed.size());
    e.text = text.left(from) + symbol + QLatin1Char(']') + text.mid(end);
    e.caret = from + static_cast<int>(symbol.size()) + 1;
    return e;
}

maths::Token tokenAt(const QString& text, int pos) {
    const std::string utf8 = text.toStdString();
    maths::Diagnostics ignored;
    const std::vector<maths::Token> tokens = maths::Lexer::tokenize(utf8, ignored);
    const std::uint32_t byte = byteOffset(text, pos);
    for (const maths::Token& t : tokens) {
        if (t.kind != maths::TokenKind::End && t.span.begin <= byte && byte < t.span.end) {
            return t;
        }
    }
    return {};
}

QString precedenceHelp(const maths::MathsCompat& compat) {
    QStringList lines;
    lines << QStringLiteral("Operator precedence, highest first:")
          << QStringLiteral("  f(...)  'Channel'[unit]")
          << (compat.unaryMinusAbovePower ? QStringLiteral("  unary - + ! ~ NOT")
                                          : QStringLiteral("  ^   (right to left: 2^3^2 = 2^9)"))
          << (compat.unaryMinusAbovePower ? QStringLiteral("  ^   (right to left: 2^3^2 = 2^9)")
                                          : QStringLiteral("  unary - + ! ~ NOT"))
          << QStringLiteral("  * /") << QStringLiteral("  + -")
          << QStringLiteral("  < <= > >=  LT LTE GT GTE") << QStringLiteral("  == !=  EQ NEQ")
          << QStringLiteral("  &") << QStringLiteral("  |") << QStringLiteral("  &&  AND")
          << QStringLiteral("  ||  OR") << QString();
    if (compat.unaryMinusAbovePower) {
        lines << QStringLiteral("unaryMinusAbovePower is on: a leading minus binds tighter than '^', so "
                                "-2^2 = 4.");
    } else {
        lines << QStringLiteral("unaryMinusAbovePower is off (the default): '^' binds tighter than a "
                                "leading minus, so -2^2 = -4.");
    }
    lines << QStringLiteral("The switch is the project option Maths compat: unary minus above power.");
    return lines.join(QLatin1Char('\n'));
}

QString functionHelp(const maths::FunctionManifest& manifest) {
    QStringList args;
    bool optional = false;
    for (const maths::ArgSpec& a : manifest.args) {
        QString name = QString::fromStdString(a.name);
        if (a.optional && !optional) {
            optional = true;
            name = QStringLiteral("[") + name;
        }
        args << name;
    }
    QString signature =
        QString::fromStdString(manifest.name) + QLatin1Char('(') + args.join(QStringLiteral(", "));
    if (optional) {
        signature += QLatin1Char(']');
    }
    signature += QLatin1Char(')');
    QStringList arities;
    for (const std::uint8_t n : manifest.arities) {
        arities << QString::number(n);
    }
    QString help = signature;
    if (!arities.isEmpty()) {
        help += QStringLiteral("\nArguments: ") + arities.join(QStringLiteral(" or "));
    }
    if (!manifest.description.empty()) {
        help += QLatin1Char('\n') + QString::fromStdString(manifest.description);
    }
    return help;
}

} // namespace esses::widgets::assist
