// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportModel (04 sections 12 item 1 and 13; 01 section 4.2; M3; D-199): the QAbstractTableModel behind the
// channel_report and time_report QTableViews. refresh() turns the component's spec plus the current selection
// into analysis::ReportInputs on the GUI thread (the ranges in scope; each channel resolved once per source
// and layer like RenderService::prepareInputs: logged channels through the source's ChannelResolver, maths
// outputs through MathsService, requested at Background and Pending until evaluated), runs
// analysis::ReportEngine / TimeReportEngine on the TaskPool at Priority::Background (replace key
// report/<componentId>) and delivers the table on the GUI thread through the Dispatcher (posted with the
// Dispatcher as the context and a QPointer made at submit time). Calls in one event-loop turn coalesce into
// one run; a delivered table is always shown (a superseded one as an interim). Pending cells show "..." and
// fill in when the widget refreshes on Data. One analysis::StatCache per workspace is shared by every
// ReportModel (RenderService::statCache()).
//
// Scope. Channel reports: selectedRanges = every visible slot's range (Main, Overlay 0, overlays, with
// their roles); selectedOutings = the outing around each visible slot (once, the first slot's role);
// allOutings = every outing of every open source, Main unless it holds the Overlay 0 slot (Overlay0) or an
// overlay slot (Overlay). Time reports: the laps of the selected ranges (an outing slot gives its laps) or
// of the outings around them. zoomLinked clips every range to the worksheet zoom through that range's own
// DomainMapper (alignment offsets applied; distance mode needs the range's DistanceChain from
// setDistanceService, else the range is not clipped).
//
// Display. Channel reports, channelsInRows (the default): one row per engine column ((channel, stat), then
// its Main - Overlay 0 column when deltaVsOverlay0), one column per heading; channelsInColumns: one row per
// heading, one column per (channel, stat). Column 0 is the row label (the channel and statistic, or the
// heading path joined by " / " with " (Overlay 0)" / " (Overlay)" after an overlay's row) and is frozen by
// the widget. Values use the column's display unit and dps; Blank cells are empty, Pending "...", Missing
// "(missing)", Error "(error)". Time reports: column 0 the lap name, then the sections in track order and
// "Lap Time"; the Eclectic, Rolling minimum and Total rows follow the laps; times in the component's
// timeFormat ("project" = setProjectTimeFormat, m:ss.fff by default); a missing section is an em dash.
// Styling travels in the roles below as plain values, because esses_qtmodel may not use Qt Gui types beyond
// the undo framework (D-161: no QColor/QFont): the widget maps them to Qt::BackgroundRole / Qt::FontRole.
// toTsv/toCsv write the displayed grid with its header row; a field holding '"', ',', '%', a tab or a newline
// is quoted with '"' doubled (04 section 12 item 7).
#pragma once

#include <esses/analysis/report_engine.hpp>
#include <esses/analysis/report_spec.hpp>
#include <esses/analysis/stat_cache.hpp>
#include <esses/analysis/time_report_engine.hpp>
#include <esses/base/result.hpp>
#include <esses/plotcore/view_state.hpp>

#include <QAbstractTableModel>
#include <QString>
#include <QVariant>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace esses {
class TaskPool;
} // namespace esses

namespace esses::qt {

class DataSelectionModel;
class Dispatcher;
class DistanceService;
class MathsService;

class ReportModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Kind : std::uint8_t { Channel, Time };
    // Styling roles (D-161: plain values; the widget turns them into Qt::BackgroundRole / Qt::FontRole).
    enum Role : int {
        BackgroundRgbaRole = Qt::UserRole + 1, // quint32 0xRRGGBBAA: min/max highlight, time-report band
        ItalicRole,                            // bool: an untrusted row, a Missing or Error cell
        BoldRole,                              // bool: the fastest time of a time-report column
        CellStateRole,                         // int: analysis::ReportCellState of a value cell
        RowKindRole // int: 0 heading/lap row, 1 Eclectic, 2 Rolling minimum, 3 Total
    };
    // Highlight colours of a channel report (body.highlight; nullopt: off).
    struct Highlight {
        std::optional<std::uint32_t> min; // 0xRRGGBBAA
        std::optional<std::uint32_t> max;
    };

    ReportModel(TaskPool& pool, Dispatcher& dispatcher, DataSelectionModel& selection, MathsService& maths,
                std::shared_ptr<analysis::StatCache> cache, QObject* parent = nullptr);
    ~ReportModel() override;

    Kind kind() const noexcept { return kind_; }
    void setChannelReport(const analysis::ReportSpec& spec, const QString& componentId);
    void setTimeReport(const analysis::TimeReportSpec& spec, const QString& componentId);
    void setHighlight(const Highlight& highlight);
    // "project", "s.mmm" or "m:ss.mmm" (04 section 13 item 6); anything else reads as "project".
    void setTimeFormat(const QString& format);
    // The project's lap time format (project.json options.lapTimeFormat: "m:ss.fff", "ss.fff", "h:mm:ss.f").
    void setProjectTimeFormat(const QString& format);
    // The worksheet zoom for zoomLinked channel reports (nullopt: not linked).
    void setZoom(std::optional<plot::ViewState> view);
    // Distance-mode zoom clipping (the ranges' DistanceChains); not owned, may be null.
    void setDistanceService(DistanceService* distance);
    // Re-runs the engine (the widget calls it on Selection and Data changes); coalesced per event-loop turn.
    void refresh();
    int pendingCells() const;
    // Engine runs delivered so far (tests).
    int deliveries() const noexcept { return deliveries_; }
    // Whether a run is queued or in flight.
    bool busy() const;
    // The last engine failure ("" when the last run succeeded).
    QString lastError() const;
    QString toTsv() const;
    QString toCsv() const;
    // A display field quoted for `separator` (04 section 12 item 7).
    static QString quoteField(const QString& field, QChar separator);

    const std::optional<analysis::ReportTable>& channelTable() const;
    const std::optional<analysis::TimeReportTable>& timeTable() const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

Q_SIGNALS:
    void computed(); // a table was delivered (possibly with pending cells)

private:
    struct Impl;
    struct Run;     // what one engine run was given (report_model.cpp)
    struct Outcome; // what it delivered
    void run();
    void deliver(std::uint64_t serial, const std::shared_ptr<const Run>& given,
                 const std::shared_ptr<Outcome>& outcome);
    void rebuildGrid();

    std::unique_ptr<Impl> impl_;
    Kind kind_ = Kind::Channel;
    int deliveries_ = 0;
};

} // namespace esses::qt
