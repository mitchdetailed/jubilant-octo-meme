// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ComponentClipboard (04 section 7.2 item 6; 06 section 4.3 items 3 and 10; D-027; 07 section 6.1; AC-M3-11):
// the clipboard form of components for CTRL+X/C/V. The MIME type application/x-esses-components+json (plus
// text/plain with the same JSON) holds {"schemaVersion": 1, "components": [ComponentSpec...], "sourceDomain":
// "time" | "distance"}. A pasted component keeps its whole body, domain-dependent settings and manual scales
// included, whatever the target worksheet's domain; placeForPaste() keeps the stored rectangles snapped to
// the target grid (1 % by default) and offsets them by one grid cell per successive paste into the same
// worksheet, with fresh component ids. The shell wraps cut/paste/delete in QUndoCommands (MainWindow::
// copyComponents/cutComponents/pasteComponents/deleteComponents). kMimeType and Payload are final (M3
// contract). As built (M3, D-200): a rectangle keeps its size in whole cells (at least one) and its snapped
// position, moved one cell right and down per paste and wrapped inside the worksheet; fromMimeData reads the
// MIME type first, then text/plain holding the same JSON; an entry ComponentSpec::parse refuses is skipped
// (an unknown type is kept and drawn as the placeholder) and a payload left with no component is not a paste.
#pragma once

#include <esses/project/component_spec.hpp>

#include <QList>
#include <QSize>
#include <QString>
#include <memory>
#include <optional>

class QMimeData;

namespace esses::widgets {

class ComponentClipboard {
public:
    static constexpr const char* kMimeType = "application/x-esses-components+json";

    struct Payload {
        int schemaVersion = 1;
        QList<project::ComponentSpec> components;
        QString sourceDomain = QStringLiteral("time"); // "time" | "distance"
    };

    static std::unique_ptr<QMimeData> toMimeData(const Payload& payload);
    // nullopt when the data holds neither the MIME type nor text/plain with a valid payload.
    static std::optional<Payload> fromMimeData(const QMimeData& data);
    // The components to insert: rectangles snapped to `grid` cells (grid = [cols, rows], 100 x 100 = 1 %) and
    // offset by `pasteCount` cells (wrapping inside the worksheet), new ids, bodies unchanged.
    static QList<project::ComponentSpec> placeForPaste(const Payload& payload, int pasteCount, QSize grid);
};

} // namespace esses::widgets
