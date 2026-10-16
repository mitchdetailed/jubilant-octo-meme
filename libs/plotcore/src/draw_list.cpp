// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/plotcore/draw_list.hpp>
#include <esses/project/rgba.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <string_view>

namespace esses::plot {

namespace {

std::size_t pushVertexCommand(DrawList& list, CmdKind kind, const float* xy, std::size_t vertexCount,
                              Rgba colour, float widthPx, DashId dash, float pointSizePx) {
    DrawCmd cmd;
    cmd.kind = kind;
    cmd.vertexOffset = static_cast<std::uint32_t>(list.verts.size() / 2);
    cmd.vertexCount = static_cast<std::uint32_t>(vertexCount);
    cmd.colour = colour;
    cmd.widthPx = widthPx;
    cmd.dash = dash;
    cmd.pointSizePx = pointSizePx;
    if (vertexCount > 0) {
        list.verts.insert(list.verts.end(), xy, xy + 2 * vertexCount);
    }
    list.cmds.push_back(cmd);
    return list.cmds.size() - 1;
}

// ---- golden JSON helpers ----------------------------------------------------------------------------------

// At most two decimals: rounded to 0.01 (half away from zero), trailing zeros and the point dropped, -0 as 0.
void appendNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    const double scaled = std::round(v * 100.0);
    if (std::abs(scaled) >= 9.0e15) {
        std::array<char, 64> buf{};
        const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, 0);
        out.append(buf.data(), r.ptr);
        return;
    }
    auto hundredths = static_cast<long long>(scaled);
    if (hundredths == 0) {
        out += '0';
        return;
    }
    if (hundredths < 0) {
        out += '-';
        hundredths = -hundredths;
    }
    const long long whole = hundredths / 100;
    const long long frac = hundredths % 100;
    std::array<char, 32> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), whole);
    out.append(buf.data(), r.ptr);
    if (frac != 0) {
        out += '.';
        out += static_cast<char>('0' + frac / 10);
        if (frac % 10 != 0) {
            out += static_cast<char>('0' + frac % 10);
        }
    }
}

// DrawList geometry is float; widening to double is exact, so the text is that of the float's own value.
void appendFloat(std::string& out, float v) {
    appendNumber(out, static_cast<double>(v));
}

void appendString(std::string& out, std::string_view s) {
    static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    out += '"';
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (u < 0x20) {
                out += "\\u00";
                out += kHex[u >> 4];
                out += kHex[u & 0x0F];
            } else {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

void appendColour(std::string& out, Rgba c) {
    appendString(out, project::formatRgba(c));
}

void appendRect(std::string& out, const RectPx& r) {
    out += '[';
    appendFloat(out, r.x);
    out += ',';
    appendFloat(out, r.y);
    out += ',';
    appendFloat(out, r.w);
    out += ',';
    appendFloat(out, r.h);
    out += ']';
}

void appendVerts(std::string& out, const DrawList& list, const DrawCmd& cmd) {
    out += '[';
    const std::size_t begin = static_cast<std::size_t>(cmd.vertexOffset) * 2;
    const std::size_t end = begin + static_cast<std::size_t>(cmd.vertexCount) * 2;
    for (std::size_t i = begin; i < end && i < list.verts.size(); ++i) {
        if (i != begin) {
            out += ',';
        }
        appendFloat(out, list.verts[i]);
    }
    out += ']';
}

constexpr std::string_view dashName(DashId d) noexcept {
    switch (d) {
    case DashId::Solid:
        return "solid";
    case DashId::Dash:
        return "dash";
    case DashId::Dot:
        return "dot";
    case DashId::DashDot:
        return "dashDot";
    }
    return "solid";
}

constexpr std::string_view styleName(TextStyleId s) noexcept {
    switch (s) {
    case TextStyleId::Axis:
        return "axis";
    case TextStyleId::Caption:
        return "caption";
    case TextStyleId::Key:
        return "key";
    case TextStyleId::KeyValue:
        return "keyValue";
    case TextStyleId::Band:
        return "band";
    case TextStyleId::Measurement:
        return "measurement";
    case TextStyleId::Banner:
        return "banner";
    case TextStyleId::Gauge:
        return "gauge";
    case TextStyleId::GaugeUnits:
        return "gaugeUnits";
    case TextStyleId::Table:
        return "table";
    case TextStyleId::Equation:
        return "equation";
    case TextStyleId::ColourBand:
        return "colourBand";
    case TextStyleId::TrackLabel:
        return "trackLabel";
    }
    return "axis";
}

constexpr std::string_view anchorName(TextAnchor a) noexcept {
    switch (a) {
    case TextAnchor::TopLeft:
        return "topLeft";
    case TextAnchor::TopCentre:
        return "topCentre";
    case TextAnchor::TopRight:
        return "topRight";
    case TextAnchor::MiddleLeft:
        return "middleLeft";
    case TextAnchor::Centre:
        return "centre";
    case TextAnchor::MiddleRight:
        return "middleRight";
    case TextAnchor::BottomLeft:
        return "bottomLeft";
    case TextAnchor::BottomCentre:
        return "bottomCentre";
    case TextAnchor::BottomRight:
        return "bottomRight";
    }
    return "topLeft";
}

} // namespace

std::size_t DrawList::clear(Rgba colour) {
    DrawCmd cmd;
    cmd.kind = CmdKind::Clear;
    cmd.colour = colour;
    cmds.push_back(cmd);
    return cmds.size() - 1;
}

std::size_t DrawList::scissor(RectPx rect) {
    DrawCmd cmd;
    cmd.kind = CmdKind::Scissor;
    cmd.rect = rect;
    cmds.push_back(cmd);
    return cmds.size() - 1;
}

std::size_t DrawList::filledRect(RectPx rect, Rgba colour) {
    DrawCmd cmd;
    cmd.kind = CmdKind::FilledRect;
    cmd.rect = rect;
    cmd.colour = colour;
    cmds.push_back(cmd);
    return cmds.size() - 1;
}

std::size_t DrawList::polyline(const float* xy, std::size_t vertexCount, Rgba colour, float lineWidthPx,
                               DashId dash) {
    return pushVertexCommand(*this, CmdKind::Polyline, xy, vertexCount, colour, lineWidthPx, dash, 0.0F);
}

std::size_t DrawList::lines(const float* xy, std::size_t vertexCount, Rgba colour, float lineWidthPx,
                            DashId dash) {
    return pushVertexCommand(*this, CmdKind::Lines, xy, vertexCount - vertexCount % 2, colour, lineWidthPx,
                             dash, 0.0F);
}

std::size_t DrawList::points(const float* xy, std::size_t vertexCount, Rgba colour, float pointSizePx) {
    return pushVertexCommand(*this, CmdKind::Points, xy, vertexCount, colour, 1.0F, DashId::Solid,
                             pointSizePx);
}

std::size_t DrawList::rectOutline(RectPx rect, Rgba colour, float lineWidthPx) {
    const float h = lineWidthPx * 0.5F;
    const float x0 = rect.x + h;
    const float y0 = rect.y + h;
    const float x1 = rect.x + rect.w - h;
    const float y1 = rect.y + rect.h - h;
    const std::array<float, 10> xy{x0, y0, x1, y0, x1, y1, x0, y1, x0, y0};
    return polyline(xy.data(), 5, colour, lineWidthPx);
}

std::size_t DrawList::textRun(std::string_view utf8, float x, float y, TextStyleId style, TextAnchor anchor,
                              Rgba colour, float maxWidthPx, std::uint8_t quarterTurns, Rgba background) {
    TextRun run;
    run.byteOffset = static_cast<std::uint32_t>(text.size());
    run.byteLen = static_cast<std::uint32_t>(utf8.size());
    run.x = x;
    run.y = y;
    run.style = style;
    run.anchor = anchor;
    run.quarterTurns = quarterTurns;
    run.colour = colour;
    run.maxWidthPx = maxWidthPx;
    run.background = background;
    text.append(utf8);
    texts.push_back(run);
    DrawCmd cmd;
    cmd.kind = CmdKind::TextRun;
    cmd.colour = colour;
    cmd.textIndex = static_cast<std::uint32_t>(texts.size() - 1);
    cmds.push_back(cmd);
    return cmds.size() - 1;
}

void DrawList::append(const DrawList& other, float dxPx, float dyPx) {
    const auto vertexBase = static_cast<std::uint32_t>(verts.size() / 2);
    const auto textBase = static_cast<std::uint32_t>(texts.size());
    const auto byteBase = static_cast<std::uint32_t>(text.size());
    for (std::size_t i = 0; i + 1 < other.verts.size(); i += 2) {
        verts.push_back(other.verts[i] + dxPx);
        verts.push_back(other.verts[i + 1] + dyPx);
    }
    text += other.text;
    for (TextRun run : other.texts) {
        run.byteOffset += byteBase;
        run.x += dxPx;
        run.y += dyPx;
        texts.push_back(run);
    }
    for (DrawCmd cmd : other.cmds) {
        switch (cmd.kind) {
        case CmdKind::Polyline:
        case CmdKind::Points:
        case CmdKind::Lines:
        case CmdKind::Quads:
            cmd.vertexOffset += vertexBase;
            break;
        case CmdKind::Scissor:
        case CmdKind::FilledRect:
            if (cmd.rect.w > 0.0F) {
                cmd.rect.x += dxPx;
                cmd.rect.y += dyPx;
            }
            break;
        case CmdKind::TextRun:
            cmd.textIndex += textBase;
            break;
        case CmdKind::Clear:
            break;
        }
        cmds.push_back(cmd);
    }
}

std::string DrawList::toJson() const {
    std::string out;
    out.reserve(256 + verts.size() * 7 + text.size() * 2 + cmds.size() * 64);
    out += "{\"w\":";
    appendNumber(out, widthPx);
    out += ",\"h\":";
    appendNumber(out, heightPx);
    out += ",\"dpr\":";
    appendFloat(out, dpr);
    out += ",\"cmds\":[";
    bool first = true;
    for (const DrawCmd& cmd : cmds) {
        out += first ? "\n" : ",\n";
        first = false;
        switch (cmd.kind) {
        case CmdKind::Clear:
            out += R"({"k":"clear","c":)";
            appendColour(out, cmd.colour);
            out += '}';
            break;
        case CmdKind::Scissor:
            out += R"({"k":"scissor","r":)";
            appendRect(out, cmd.rect);
            out += '}';
            break;
        case CmdKind::FilledRect:
            out += R"({"k":"rect","c":)";
            appendColour(out, cmd.colour);
            out += ",\"r\":";
            appendRect(out, cmd.rect);
            out += '}';
            break;
        case CmdKind::Polyline:
        case CmdKind::Lines:
        case CmdKind::Quads:
            out += cmd.kind == CmdKind::Polyline
                       ? R"({"k":"polyline")"
                       : (cmd.kind == CmdKind::Lines ? R"({"k":"lines")" : R"({"k":"quads")");
            out += ",\"c\":";
            appendColour(out, cmd.colour);
            out += ",\"w\":";
            appendFloat(out, cmd.widthPx);
            out += ",\"d\":";
            appendString(out, dashName(cmd.dash));
            out += ",\"v\":";
            appendVerts(out, *this, cmd);
            out += '}';
            break;
        case CmdKind::Points:
            out += R"({"k":"points","c":)";
            appendColour(out, cmd.colour);
            out += ",\"s\":";
            appendFloat(out, cmd.pointSizePx);
            out += ",\"v\":";
            appendVerts(out, *this, cmd);
            out += '}';
            break;
        case CmdKind::TextRun: {
            out += R"({"k":"text")";
            if (cmd.textIndex < texts.size()) {
                const TextRun& run = texts[cmd.textIndex];
                out += ",\"t\":";
                appendString(out, textOf(run));
                out += ",\"x\":";
                appendFloat(out, run.x);
                out += ",\"y\":";
                appendFloat(out, run.y);
                out += ",\"style\":";
                appendString(out, styleName(run.style));
                out += ",\"anchor\":";
                appendString(out, anchorName(run.anchor));
                out += ",\"c\":";
                appendColour(out, run.colour);
                if (run.maxWidthPx > 0.0F) {
                    out += ",\"maxw\":";
                    appendFloat(out, run.maxWidthPx);
                }
                if (run.quarterTurns != 0) {
                    out += ",\"rot\":";
                    appendNumber(out, run.quarterTurns);
                }
                if (run.background != 0) {
                    out += ",\"bg\":";
                    appendColour(out, run.background);
                }
                if (run.scale != 1.0F) {
                    out += ",\"scale\":";
                    appendFloat(out, run.scale);
                }
            }
            out += '}';
            break;
        }
        }
    }
    out += "\n]}\n";
    return out;
}

} // namespace esses::plot
