// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The PropertiesDialog pages of the ten MVP component types (06 section 12 item 3, AC-M3-08): per type, the
// tabs as a layout plus a FieldBinder table of body pointers, built from field_editors.hpp. Every property of
// the type's body schema (schemas/components/<type>-1.schema.json) is bound by exactly one row or lies
// inside a Custom/BandAxis/ColourBands/UnitValue row's subtree; a tab with no body field is left out (06
// section 12 item 6). The header (caption, links, legend) is the "Component" group of the Display tab.
// Internal to src/properties_shell/.
#pragma once

#include <esses/widgets/properties_shell/field_editors.hpp>

#include <QString>
#include <vector>

namespace esses::widgets::pages {

struct Page {
    QString title;
    std::vector<FieldRow> rows;
};

// The tabs of `type` (empty for a type without a body schema: a placeholder or unknown type). `schema` is the
// body schema (nested forms validate against it); it must outlive the editors.
std::vector<Page> pagesFor(const QString& type, const SchemaTree* schema);

// The header rows (pointers into {"caption": {...}, "links": {...}, "legend": {...}}).
std::vector<FieldRow> headerRows();

// The index of the tab that lists the channels (Channels, Sections for the time report), 0 when none.
int channelsTab(const std::vector<Page>& pages);

} // namespace esses::widgets::pages
