// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeReport, JSON type "time_report" (04 section 13; D-027; 07 section 6.1; AC-M3-03).
// A table with a frozen first column over a qt::ReportModel in time mode (analysis::TimeReportEngine): laps x
// sections of rowGroup with the lap time, then the Eclectic, Rolling minimum and Total rows; % bands tint
// the cells (toggleBand(n) / ALT+n flips bands[n].visible as one undoable spec edit; a band beyond the list
// is a no-op); the fastest time of a column is bold; untrusted laps are italic when included. The model
// re-runs on Selection and Data. Export Data writes CSV, copyTsv() puts TSV on the clipboard, Print to
// Clipboard paints the widget (QWidget::render). ALT+0..ALT+9: the shell offers component.colour_band [n] to
// the focused component first (handleCommand -> toggleBand; a band beyond the list is a handled no-op); where
// no command runs the key reaches this widget, which handles it itself. Overlay policy AllOverlays (04
// section 7.4); key context Component (06 section 14.2).
#pragma once

#include <esses/widgets/components/component_widget.hpp>

class QTableView;

namespace esses::qt {
class ReportModel;
} // namespace esses::qt

namespace esses::widgets {

namespace detail {
class ReportTable;
} // namespace detail

class TimeReport final : public ComponentWidget {
    Q_OBJECT
public:
    static constexpr const char* kType = "time_report";

    TimeReport(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent = nullptr);
    ~TimeReport() override;

    const char* type() const override { return kType; }
    void applySpec(const project::ComponentSpec& spec) override;
    void onSelectionChanged(const plot::SelectionSnapshot& selection) override;
    void onDataChanged(const model::SourceId& source, std::uint64_t version) override;
    void onChannelDataChanged(const maths::ChannelKey& key) override;
    void onSchemeChanged(const project::Scheme& scheme) override;
    OverlayPolicy overlayPolicy() const override;
    qt::CommandContext commandContext() const override;
    QStringList contextCommands() const override;
    bool exportData(ExportSink& sink, const ExportOptions& options) const override;
    QImage renderToImage(qreal dpr) const override;
    bool handleCommand(const QString& id, const QVariantList& args) override;

    // ALT+n (04 section 13 item 3): flips bands[n].visible through specEditRequested; false when there is no
    // band n.
    bool toggleBand(int n);
    qt::ReportModel& model() const noexcept { return *model_; }
    QTableView* tableView() const;
    QTableView* frozenView() const;
    QString toTsv() const;
    void copyTsv() const;

protected:
    void onFocusChanged(bool focused) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void applyBody();
    // Reruns the report now, or once when shown again if the component is hidden (a kept worksheet's
    // report does no work for nobody; D-203 (z)).
    void requestRefresh();

    qt::ReportModel* model_ = nullptr;
    detail::ReportTable* table_ = nullptr;
    std::shared_ptr<const project::Scheme> scheme_;
    bool refreshPending_ = false;
};

} // namespace esses::widgets
