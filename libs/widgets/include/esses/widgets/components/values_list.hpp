// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ValuesList, JSON type "values_list" (04 section 15): a QTableView over its own qt::ValuesModel (shared with
// the Values window): rows = the body's channels; columns Name, Main, reference overlay, delta, unit; values
// at the cursor through the single conversion rule of 04 section 3.2 ("..." while pending; <= 2 ms for 250
// rows). Body: {channels[{channel, displayUnit, dps}], columns{datum, delta, inlineUnits}, showHeading,
// autoSizeColumns, widths{name, value, delta, datum}}; unknown members survive edits. Overlay policy
// MainAndReference. The ValuesModel follows the WorksheetViewModel and the selection itself (cursor, datum,
// selection, data), so onViewChanged has nothing to add. Fonts: the scheme roles valuesHeader/valuesItem.
// addChannel appends a row (specEditRequested). May slip to M2 (07 section 4.6; the Values window stays).
// M1 limits: no datum column (ValuesModel has none yet: columns.datum is kept but not shown), links are
// always followed, no active-row hotkeys.
#pragma once

#include <esses/widgets/components/component_widget.hpp>

class QTableView;

namespace esses::qt {
class ValuesModel;
} // namespace esses::qt

namespace esses::widgets {

class ValuesList final : public ComponentWidget {
    Q_OBJECT
public:
    static constexpr const char* kType = "values_list";

    ValuesList(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent = nullptr);
    ~ValuesList() override;

    const char* type() const override { return kType; }
    void applySpec(const project::ComponentSpec& spec) override;
    void onViewChanged(const plot::ViewState& view, plot::ChangeMask what) override;
    void onSchemeChanged(const project::Scheme& scheme) override;
    OverlayPolicy overlayPolicy() const override { return OverlayPolicy::MainAndReference; }
    QStringList channelNames() const override;
    bool acceptsChannelDrop() const override { return true; }
    void addChannel(const QString& name, bool newPanel) override;
    QImage renderToImage(qreal dpr) const override; // QWidget::render

    qt::ValuesModel& model() noexcept { return *model_; }
    QTableView& table() noexcept { return *table_; }

private:
    void applyBody();
    void applyFonts(const project::Scheme& scheme);

    qt::ValuesModel* model_ = nullptr; // owned by this widget (QObject parent)
    QTableView* table_ = nullptr;
};

} // namespace esses::widgets
