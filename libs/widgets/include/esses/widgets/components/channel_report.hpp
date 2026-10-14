// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelReport, JSON type "channel_report" (04 section 12; D-027; 07 section 6.1; AC-M3-03).
// A table with a frozen first column over a qt::ReportModel (analysis::ReportEngine through the workspace's
// StatCache, RenderService::statCache()): scope, ordered headings, statistics, the probe, the min/max
// highlight, delta vs Overlay 0, channelsInRows (default) or channelsInColumns; cells fill in incrementally
// ("..." while pending). The model re-runs on Selection and Data (a source of the selection), and on Zoom and
// Domain when report.options.zoomLinked. Export Data writes the table as CSV (exportData), copyTsv() puts it
// on the clipboard as TSV, Print to Clipboard paints the widget (QWidget::render). addChannel appends a
// channel with the avg, min and max statistics; the body's unknown members survive edits.
// Overlay policy AllOverlays (04 section 7.4); key context Component (06 section 14.2).
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

class ChannelReport final : public ComponentWidget {
    Q_OBJECT
public:
    static constexpr const char* kType = "channel_report";

    ChannelReport(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent = nullptr);
    ~ChannelReport() override;

    const char* type() const override { return kType; }
    void applySpec(const project::ComponentSpec& spec) override;
    void onViewChanged(const plot::ViewState& view, plot::ChangeMask what) override;
    void onSelectionChanged(const plot::SelectionSnapshot& selection) override;
    void onDataChanged(const model::SourceId& source, std::uint64_t version) override;
    void onSchemeChanged(const project::Scheme& scheme) override;
    OverlayPolicy overlayPolicy() const override;
    qt::CommandContext commandContext() const override;
    QStringList channelNames() const override;
    bool acceptsChannelDrop() const override { return true; }
    void addChannel(const QString& name, bool newPanel) override;
    QStringList contextCommands() const override;
    bool exportData(ExportSink& sink, const ExportOptions& options) const override;
    QImage renderToImage(qreal dpr) const override;

    qt::ReportModel& model() const noexcept { return *model_; }
    QTableView* tableView() const;  // the value columns
    QTableView* frozenView() const; // column 0
    QString toTsv() const;
    void copyTsv() const; // the whole table to the clipboard as TSV (04 section 12 item 7)

protected:
    void onFocusChanged(bool focused) override;

private:
    void applyBody();

    qt::ReportModel* model_ = nullptr;
    detail::ReportTable* table_ = nullptr;
    std::shared_ptr<const project::Scheme> scheme_;
    bool zoomLinked_ = false;
};

} // namespace esses::widgets
