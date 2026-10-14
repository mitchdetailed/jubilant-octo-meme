// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ComponentClipboard (04 section 7.2 item 6, 06 section 4.3 item 10, AC-M3-11). Owner: src/properties_shell/.

#include <esses/base/uuid.hpp>
#include <esses/widgets/properties_shell/component_clipboard.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <algorithm>
#include <cmath>

namespace esses::widgets {

namespace {

QByteArray payloadJson(const ComponentClipboard::Payload& payload) {
    QJsonArray list;
    for (const project::ComponentSpec& c : payload.components) {
        const std::string text = c.toJson();
        if (!text.empty()) {
            list.append(QJsonDocument::fromJson(QByteArray::fromStdString(text)).object());
        }
    }
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), payload.schemaVersion);
    root.insert(QStringLiteral("components"), list);
    root.insert(QStringLiteral("sourceDomain"), payload.sourceDomain);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::optional<ComponentClipboard::Payload> parsePayload(const QByteArray& json) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = doc.object();
    const QJsonValue list = root.value(QStringLiteral("components"));
    if (!list.isArray() || root.value(QStringLiteral("schemaVersion")).toInt(0) < 1) {
        return std::nullopt;
    }
    ComponentClipboard::Payload p;
    p.schemaVersion = root.value(QStringLiteral("schemaVersion")).toInt();
    const QString domain = root.value(QStringLiteral("sourceDomain")).toString();
    p.sourceDomain = domain == QLatin1String("distance") ? domain : QStringLiteral("time");
    for (const QJsonValue& v : list.toArray()) {
        if (!v.isObject()) {
            continue;
        }
        const QByteArray text = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
        if (Result<project::ComponentSpec> c = project::ComponentSpec::parse(text.toStdString())) {
            p.components.push_back(std::move(c).value());
        }
    }
    if (p.components.isEmpty()) {
        return std::nullopt;
    }
    return p;
}

} // namespace

std::unique_ptr<QMimeData> ComponentClipboard::toMimeData(const Payload& payload) {
    auto data = std::make_unique<QMimeData>();
    const QByteArray json = payloadJson(payload);
    data->setData(QString::fromLatin1(kMimeType), json);
    data->setText(QString::fromUtf8(json)); // text/plain carries the same JSON (06 section 4.3 item 10)
    return data;
}

std::optional<ComponentClipboard::Payload> ComponentClipboard::fromMimeData(const QMimeData& data) {
    if (data.hasFormat(QString::fromLatin1(kMimeType))) {
        if (std::optional<Payload> p = parsePayload(data.data(QString::fromLatin1(kMimeType)))) {
            return p;
        }
    }
    if (data.hasText()) {
        return parsePayload(data.text().toUtf8());
    }
    return std::nullopt;
}

QList<project::ComponentSpec> ComponentClipboard::placeForPaste(const Payload& payload, int pasteCount,
                                                                QSize grid) {
    const int cols = std::max(1, grid.width());
    const int rows = std::max(1, grid.height());
    const int step = std::max(0, pasteCount);
    QList<project::ComponentSpec> out;
    for (project::ComponentSpec c : payload.components) {
        // Snap the stored rectangle to the target grid (whole cells, at least one cell wide and high).
        auto l = static_cast<int>(std::lround(c.rect.l * cols));
        auto t = static_cast<int>(std::lround(c.rect.t * rows));
        int w = std::max(1, static_cast<int>(std::lround((c.rect.r - c.rect.l) * cols)));
        int h = std::max(1, static_cast<int>(std::lround((c.rect.b - c.rect.t) * rows)));
        w = std::min(w, cols);
        h = std::min(h, rows);
        // One cell further right and down per successive paste, wrapping inside the worksheet.
        const int spanX = cols - w + 1;
        const int spanY = rows - h + 1;
        l = ((std::clamp(l, 0, cols - w) + step) % spanX + spanX) % spanX;
        t = ((std::clamp(t, 0, rows - h) + step) % spanY + spanY) % spanY;
        c.rect = project::Rect{static_cast<double>(l) / cols, static_cast<double>(t) / rows,
                               static_cast<double>(l + w) / cols, static_cast<double>(t + h) / rows};
        c.id = uuidRandom(); // a paste never duplicates an id
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace esses::widgets
