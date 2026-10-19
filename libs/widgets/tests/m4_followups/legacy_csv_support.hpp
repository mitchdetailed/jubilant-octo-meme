// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Reading a component's Export Data in the legacy CSV dialect (M4, D-211 (d)) by the layout 05 section 6
// "Writer" gives it, not by the writer's code: eleven metadata lines, two blank lines, the names row and the
// units row, two blank lines, then the data; CRLF line ends; every field double-quoted (an invalid sample an
// empty quoted field). Header-only, shared by the component and shell tests.
#pragma once

#include <QByteArray>
#include <QList>

namespace esses::widgets::legacycsv {

inline constexpr int kMetadataLines = 11;
inline constexpr int kNamesRow = kMetadataLines + 2; // after the two blank lines
inline constexpr int kUnitsRow = kNamesRow + 1;
inline constexpr int kFirstDataRow = kUnitsRow + 3; // after the two blank lines
inline constexpr int kLinesBeforeData = kFirstDataRow;

// The lines of a CRLF text (the last line end does not start another line).
inline QList<QByteArray> lines(const QByteArray& csv) {
    QList<QByteArray> out = csv.split('\n');
    if (!out.isEmpty() && out.back().isEmpty()) {
        out.removeLast();
    }
    for (QByteArray& l : out) {
        if (l.endsWith('\r')) {
            l.chop(1);
        }
    }
    return out;
}

// Every line but the last ends with CRLF, and no bare LF occurs.
inline bool crlfOnly(const QByteArray& csv) {
    for (qsizetype i = 0; i < csv.size(); ++i) {
        if (csv[i] == '\n' && (i == 0 || csv[i - 1] != '\r')) {
            return false;
        }
    }
    return csv.endsWith("\r\n");
}

// The fields of a line of quoted fields, unquoted ("" -> empty); an unquoted field is returned as it is
// (the metadata lines leave some fields empty without quotes).
inline QList<QByteArray> fields(const QByteArray& line) {
    QList<QByteArray> out;
    QByteArray cur;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cur += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                cur += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// Every field of the line is double-quoted.
inline bool allQuoted(const QByteArray& line) {
    for (const QByteArray& f : line.split(',')) {
        if (f.size() < 2 || !f.startsWith('"') || !f.endsWith('"')) {
            return false;
        }
    }
    return true;
}

} // namespace esses::widgets::legacycsv
