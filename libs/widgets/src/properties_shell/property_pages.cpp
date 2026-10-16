// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The PropertiesDialog pages of the ten MVP component types (06 section 12 item 3, AC-M3-08). Owner:
// src/properties_shell/.

#include "property_pages.hpp"

#include <esses/plotcore/scatter_geometry.hpp>
#include <esses/project/profile.hpp>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <utility>

namespace esses::widgets::pages {

namespace {

using Make = std::function<FieldEditor*(QWidget*)>;
using K = FieldKind;

QString tr(const char* text) {
    return QCoreApplication::translate("esses::widgets::PropertiesDialog", text);
}

FieldRow leaf(const char* pointer, K kind, const char* label, QStringList enumValues = {}) {
    FieldRow r;
    r.pointer = QString::fromLatin1(pointer);
    r.kind = kind;
    r.label = tr(label);
    r.enumValues = std::move(enumValues);
    return r;
}

FieldRow sub(const char* pointer, K kind, const char* label, Make make) {
    FieldRow r;
    r.pointer = QString::fromLatin1(pointer);
    r.kind = kind;
    r.label = tr(label);
    r.make = std::move(make);
    return r;
}

FieldRow unitValue(const char* pointer, const char* label) {
    return sub(pointer, K::UnitValue, label, [](QWidget* parent) { return new UnitValueEdit(parent); });
}

// The report headings a picker suggests before the dialog knows the session's profile: the built-in
// circuit profile's (04 section 12 item 6). PropertiesDialog replaces them with the active profile's.
QStringList headingSuggestions() {
    QStringList out;
    for (const std::string& h : project::Profile::circuitDefault().reportHeadings) {
        out.push_back(QString::fromStdString(h));
    }
    return out;
}

// The range-group paths among them (a time report's rows, a graph's band group).
QStringList rangeGroupSuggestions() {
    QStringList out;
    for (const QString& h : headingSuggestions()) {
        if (h.startsWith(QLatin1String("Outings"))) {
            out.push_back(h);
        }
    }
    return out;
}

QJsonObject zeroValue() {
    return QJsonObject{{QStringLiteral("v"), 0}, {QStringLiteral("unit"), QString()}};
}

Make form(std::vector<FieldRow> rows, const SchemaTree* schema, QString prefix) {
    return [rows = std::move(rows), schema, prefix = std::move(prefix)](QWidget* parent) -> FieldEditor* {
        return new FieldForm(rows, schema, prefix, parent);
    };
}

Make list(Make item, QJsonValue newItem) {
    return [item = std::move(item), newItem = std::move(newItem)](QWidget* parent) -> FieldEditor* {
        return new RecordListEditor(item, newItem, {}, parent);
    };
}

Make scalarList(K kind, QStringList enumValues, QJsonValue newItem) {
    return list([kind, enumValues = std::move(enumValues)](
                    QWidget* parent) -> FieldEditor* { return new LeafEditor(kind, enumValues, parent); },
                std::move(newItem));
}

Make optional(const char* label, Make inner, QJsonValue defaultValue) {
    return [label, inner = std::move(inner),
            defaultValue = std::move(defaultValue)](QWidget* parent) -> FieldEditor* {
        return new OptionalEditor(tr(label), inner(parent), defaultValue, parent);
    };
}

Make fixedArray(K kind, int count, QStringList labels, QList<double> defaults = {}) {
    return [kind, count, labels = std::move(labels),
            defaults = std::move(defaults)](QWidget* parent) -> FieldEditor* {
        return new FixedArrayEdit(kind, count, labels, parent, defaults);
    };
}

// ---- shared definitions (04 section 7.3) ------------------------------------------------------------------

std::vector<FieldRow> bandAxisRows(const SchemaTree* s, const QString& prefix) {
    QJsonObject band{
        {QStringLiteral("min"), zeroValue()},
        {QStringLiteral("max"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("unit"), QString()}}}};
    return {leaf("/mode", K::Enum, "Bands"),
            leaf("/bandMode", K::Enum, "Even bands by"),
            unitValue("/dataMin", "Minimum"),
            unitValue("/dataMax", "Maximum"),
            unitValue("/bandWidth", "Band width"),
            leaf("/bandCount", K::Integer, "Band count"),
            leaf("/palette", K::Text, "Palette"),
            leaf("/colour1", K::Colour, "Colour 1"),
            leaf("/colour2", K::Colour, "Colour 2"),
            leaf("/reverse", K::Bool, "Reverse colours"),
            leaf("/centred", K::Bool, "Centre bands on values"),
            leaf("/lessMinBand", K::Bool, "Band below minimum"),
            leaf("/greaterMaxBand", K::Bool, "Band above maximum"),
            leaf("/continuous", K::Bool, "Continuous colours"),
            sub("/bands", K::Custom, "Arbitrary bands",
                list(form({unitValue("/min", "Minimum"), unitValue("/max", "Maximum"),
                           leaf("/colour", K::Colour, "Colour")},
                          s, prefix + QStringLiteral("/bands/0")),
                     band))};
}

Make bandAxis(const SchemaTree* s, const QString& prefix) {
    return form(bandAxisRows(s, prefix), s, prefix);
}

Make colourBands(const SchemaTree* s, const QString& prefix) {
    QJsonObject band{
        {QStringLiteral("min"), zeroValue()},
        {QStringLiteral("max"), QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("unit"), QString()}}},
        {QStringLiteral("visible"), true}};
    return list(
        form({unitValue("/min", "Minimum"), unitValue("/max", "Maximum"), leaf("/open", K::Enum, "Open end"),
              leaf("/colour", K::Colour, "Colour"), leaf("/visible", K::Bool, "Visible")},
             s, prefix + QStringLiteral("/0")),
        band);
}

void scaleRows(std::vector<FieldRow>& rows, const char* base, const char* label) {
    const QString b = QString::fromLatin1(base);
    FieldRow mode;
    mode.pointer = b + QStringLiteral("/mode");
    mode.kind = K::Enum;
    mode.label = tr(label) + tr(" scale");
    rows.push_back(mode);
    FieldRow min;
    min.pointer = b + QStringLiteral("/min");
    min.kind = K::UnitValue;
    min.label = tr(label) + tr(" minimum");
    min.make = [](QWidget* parent) { return new UnitValueEdit(parent); };
    rows.push_back(min);
    FieldRow max = min;
    max.pointer = b + QStringLiteral("/max");
    max.label = tr(label) + tr(" maximum");
    rows.push_back(max);
}

// The groups and traces of a time_distance or outing graph (04 sections 8.2-8.3, 8.14).
Make groupsEditor(const SchemaTree* s) {
    const auto line = [](const char* which, const char* label) {
        std::vector<FieldRow> rows;
        FieldRow on;
        on.pointer = QStringLiteral("/lines/%1/enabled").arg(QLatin1String(which));
        on.kind = K::Bool;
        on.label = tr(label);
        rows.push_back(on);
        FieldRow colour;
        colour.pointer = QStringLiteral("/lines/%1/colour").arg(QLatin1String(which));
        colour.kind = K::Colour;
        colour.label = tr(label) + tr(" colour");
        rows.push_back(colour);
        return rows;
    };
    std::vector<FieldRow> group{leaf("/name", K::Text, "Group name"),
                                leaf("/scaleMode", K::Enum, "Scale"),
                                unitValue("/scaleMin", "Scale minimum"),
                                unitValue("/scaleMax", "Scale maximum"),
                                sub("/edges", K::Custom, "Panel edges (top, bottom)",
                                    fixedArray(K::Number, 2, {tr("top"), tr("bottom")}, {0.0, 1.0})),
                                leaf("/lo", K::Number, "Overlapped low edge"),
                                leaf("/hi", K::Number, "Overlapped high edge")};
    for (const auto& [which, label] : {std::pair{"zero", "Zero line"}, std::pair{"min", "Minimum line"},
                                       std::pair{"max", "Maximum line"}}) {
        for (FieldRow& r : line(which, label)) {
            group.push_back(std::move(r));
        }
    }
    group.push_back(
        sub("/traces", K::Custom, "Traces",
            list(form({leaf("/channel", K::Channel, "Channel"), leaf("/visible", K::Bool, "Visible"),
                       leaf("/displayUnit", K::Text, "Display unit"), leaf("/dps", K::Integer, "Decimals")},
                      s, QStringLiteral("/groups/0/traces/0")),
                 QJsonObject{{QStringLiteral("channel"), QString()}, {QStringLiteral("visible"), true}})));
    return list(form(std::move(group), s, QStringLiteral("/groups/0")),
                QJsonObject{{QStringLiteral("name"), tr("Group")}, {QStringLiteral("traces"), QJsonArray()}});
}

std::vector<Page> graphPages(const SchemaTree* s, bool outing) {
    Page channels{tr("Channels"), {sub("/groups", K::Custom, "Groups", groupsEditor(s))}};
    Page display{
        tr("Display"),
        {leaf("/display/traceStyle", K::Enum, "Trace style"),
         leaf("/display/lineWidth", K::Number, "Line width"),
         leaf("/display/pointSize", K::Number, "Point size"), leaf("/display/showGrid", K::Bool, "Grid"),
         leaf("/display/showMarkers", K::Bool, "Markers"), leaf("/display/showBands", K::Bool, "Bands"),
         leaf("/display/bandMode", K::Enum, "Band mode"),
         leaf("/display/bandGroup", K::Text, "Band group", rangeGroupSuggestions()),
         leaf("/display/overlapMode", K::Enum, "Layout"),
         leaf("/display/cursorStyle", K::Enum, "Cursor style"),
         leaf("/display/cursorSnap", K::Bool, "Snap cursor to samples"),
         leaf("/display/key/mode", K::Enum, "Key"), leaf("/display/key/pos", K::Enum, "Key position"),
         leaf("/display/key/values", K::Bool, "Key values")}};
    if (!outing) {
        display.rows.push_back(leaf("/display/key/stats", K::Bool, "Measurements row"));
    }
    display.rows.push_back(leaf("/display/showStatusStrip", K::Bool, "Status and errors strip"));
    if (!outing) {
        display.rows.push_back(sub("/display/statusChannels", K::Custom, "Status channels",
                                   scalarList(K::Channel, {}, QString())));
    }
    display.rows.push_back(leaf("/display/showDeviceEvents", K::Bool, "Device events"));
    if (!outing) {
        display.rows.push_back(leaf("/display/variance/show", K::Bool, "Variance panel"));
        display.rows.push_back(leaf("/display/variance/heightFrac", K::Number, "Variance height"));
        display.rows.push_back(leaf("/display/variance/autoScale", K::Bool, "Variance auto scale"));
        display.rows.push_back(unitValue("/display/variance/min", "Variance minimum"));
        display.rows.push_back(unitValue("/display/variance/max", "Variance maximum"));
    }
    display.rows.push_back(leaf("/display/axis/showX", K::Bool, "X axis"));
    display.rows.push_back(leaf("/display/axis/showY", K::Bool, "Y axis"));
    display.rows.push_back(leaf("/display/axis/showXScrollbar", K::Bool, "X scroll bar"));
    display.rows.push_back(leaf("/display/axis/showYScrollbar", K::Bool, "Y scroll bar"));
    std::vector<Page> out{std::move(channels), std::move(display)};
    if (outing) {
        out.push_back(Page{tr("Data"), {leaf("/enableOverlays", K::Bool, "Draw overlays (time mode)")}});
    }
    return out;
}

std::vector<Page> scatterPages(const SchemaTree* s) {
    Page channels{
        tr("Channels"),
        {leaf("/xChannel", K::Channel, "X channel"),
         sub("/traces", K::Custom, "Y channels",
             list(form({leaf("/yChannel", K::Channel, "Y channel"), leaf("/visible", K::Bool, "Visible"),
                        leaf("/yDelayUs", K::Integer, "Y delay (us)")},
                       s, QStringLiteral("/traces/0")),
                  QJsonObject{{QStringLiteral("yChannel"), QString()}, {QStringLiteral("visible"), true}}))}};
    Page display{tr("Display"),
                 {leaf("/layout", K::Enum, "Layout"), leaf("/tileCols", K::Integer, "Tile columns"),
                  leaf("/style", K::Enum, "Style"), leaf("/pointSize", K::Number, "Point size"),
                  leaf("/trend/type", K::Enum, "Trend"), leaf("/trend/order", K::Integer, "Trend order"),
                  leaf("/trend/throughZero", K::Bool, "Trend through zero"),
                  leaf("/trend/showEquation", K::Bool, "Show equation"),
                  leaf("/trend/significantDigits", K::Integer, "Equation digits"),
                  leaf("/intervalAverage/enabled", K::Bool, "Interval average"),
                  unitValue("/intervalAverage/binWidth", "Interval width"),
                  leaf("/cursorStyle", K::Enum, "Cursor style"), leaf("/showGrid", K::Bool, "Grid")}};
    scaleRows(display.rows, "/scales/x", "X");
    scaleRows(display.rows, "/scales/y", "Y");
    scaleRows(display.rows, "/scales/z", "Colour");
    Page colour{tr("Colour Channel"),
                {leaf("/colourChannel/channel", K::Channel, "Colour channel"),
                 sub("/colourChannel/axis", K::BandAxis, "Colour bands",
                     bandAxis(s, QStringLiteral("/colourChannel/axis")))}};
    Page data{tr("Data"),
              {leaf("/gating/channel", K::Channel, "Gating channel"),
               leaf("/density/auto", K::Bool, "Automatic density"),
               leaf("/density/threshold", K::Integer, "Density threshold"),
               sub("/density/bins", K::Custom, "Density bins (x, y)",
                   fixedArray(K::Integer, 2, {QStringLiteral("x"), QStringLiteral("y")},
                              {static_cast<double>(plot::ScatterBody{}.densityBinsX),
                               static_cast<double>(plot::ScatterBody{}.densityBinsY)}))}};
    return {std::move(channels), std::move(display), std::move(colour), std::move(data)};
}

std::vector<Page> histogramPages(const SchemaTree* s) {
    Page channels{
        tr("Channels"),
        {sub("/traces", K::Custom, "Channels",
             list(form({leaf("/channel", K::Channel, "Channel"), leaf("/visible", K::Bool, "Visible")}, s,
                       QStringLiteral("/traces/0")),
                  QJsonObject{{QStringLiteral("channel"), QString()}, {QStringLiteral("visible"), true}}))}};
    Page display{tr("Display"),
                 {leaf("/style", K::Enum, "Style"), leaf("/layout", K::Enum, "Layout"),
                  leaf("/tileCols", K::Integer, "Tile columns"),
                  leaf("/binLabelStep", K::Integer, "Label every n bins"),
                  leaf("/condensedAxes", K::Bool, "Condensed axes"),
                  leaf("/stackLines", K::Bool, "Stack lines"), leaf("/yMode", K::Enum, "Y axis")}};
    scaleRows(display.rows, "/yScale", "Y");
    QJsonObject defaultAxis{{QStringLiteral("mode"), QStringLiteral("even")},
                            {QStringLiteral("bandMode"), QStringLiteral("count")},
                            {QStringLiteral("bandCount"), 10}};
    Page colour{
        tr("Colour Channel"),
        {leaf("/colourChannel/channel", K::Channel, "Colour channel"),
         sub("/colourChannel/axis", K::BandAxis, "Colour bands",
             optional("Colour by bands", bandAxis(s, QStringLiteral("/colourChannel/axis")), defaultAxis))}};
    Page data{tr("Data"),
              {sub("/bins", K::BandAxis, "Bins", bandAxis(s, QStringLiteral("/bins"))),
               leaf("/excludeUntrusted", K::Bool, "Exclude untrusted laps"),
               leaf("/gating/channel", K::Channel, "Gating channel")}};
    return {std::move(channels), std::move(display), std::move(colour), std::move(data)};
}

std::vector<Page> channelReportPages(const SchemaTree* s) {
    Page channels{
        tr("Channels"),
        {sub(
            "/report/channels", K::Custom, "Channels",
            list(form({leaf("/name", K::Channel, "Channel"),
                       sub("/stats", K::Custom, "Statistics",
                           scalarList(K::Enum,
                                      SchemaTree::enumValues(
                                          s != nullptr ? s->node(QStringLiteral("/report/channels/0/stats/0"))
                                                       : QJsonObject()),
                                      QStringLiteral("avg")))},
                      s, QStringLiteral("/report/channels/0")),
                 QJsonObject{{QStringLiteral("name"), QString()}}))}};
    Page report{tr("Report"),
                {leaf("/report/scope", K::Enum, "Scope"),
                 sub("/report/headers", K::Custom, "Headings",
                     scalarList(K::Text, headingSuggestions(), QStringLiteral("Outings:Laps"))),
                 leaf("/report/options/excludeOverlays", K::Bool, "Exclude overlays"),
                 leaf("/report/options/includeUntrusted", K::Bool, "Include untrusted laps"),
                 leaf("/report/options/interpolateBoundaries", K::Bool, "Interpolate at range edges"),
                 leaf("/report/options/probeSeconds", K::Number, "Probe (s)"),
                 leaf("/report/options/zoomLinked", K::Bool, "Follow the zoom"),
                 leaf("/deltaVsOverlay0", K::Bool, "Difference to Overlay 0"),
                 leaf("/timeFormat", K::Enum, "Time format"), leaf("/layout", K::Enum, "Layout"),
                 leaf("/highlight/min/on", K::Bool, "Highlight minimum"),
                 leaf("/highlight/min/colour", K::Colour, "Minimum colour"),
                 leaf("/highlight/max/on", K::Bool, "Highlight maximum"),
                 leaf("/highlight/max/colour", K::Colour, "Maximum colour")}};
    Page display{
        tr("Display"),
        {leaf("/splitter/mode", K::Enum, "Split"), leaf("/splitter/pos", K::Number, "Split position")}};
    return {std::move(channels), std::move(report), std::move(display)};
}

std::vector<Page> timeReportPages(const SchemaTree* s) {
    Page sections{tr("Sections"),
                  {leaf("/rowGroup", K::Text, "Rows (range group)", rangeGroupSuggestions()),
                   leaf("/scope", K::Enum, "Scope"),
                   leaf("/options/includeUntrusted", K::Bool, "Include untrusted laps"),
                   leaf("/summaries/eclectic", K::Bool, "Eclectic lap"),
                   leaf("/summaries/rollingMin", K::Bool, "Rolling minimum"),
                   leaf("/summaries/total", K::Bool, "Total"), leaf("/timeFormat", K::Enum, "Time format")}};
    Page bands{tr("Bands"),
               {sub("/bands", K::Custom, "Percentage bands",
                    list(form({leaf("/pct", K::Number, "Within %"), leaf("/colour", K::Colour, "Colour"),
                               leaf("/visible", K::Bool, "Visible")},
                              s, QStringLiteral("/bands/0")),
                         QJsonObject{{QStringLiteral("pct"), 1}, {QStringLiteral("visible"), true}}))}};
    return {std::move(sections), std::move(bands), Page{tr("Display"), {}}};
}

std::vector<Page> trackPositionPages() {
    return {Page{
        tr("Display"),
        {leaf("/showZoomSpan", K::Bool, "Zoom span"), leaf("/showSectionLabels", K::Bool, "Section labels"),
         leaf("/showSectionMarks", K::Bool, "Section marks"),
         leaf("/showSectionColours", K::Bool, "Section colours"),
         leaf("/sizes/cursor", K::Number, "Cursor size"), leaf("/sizes/detail", K::Number, "Detail size"),
         leaf("/sizes/material", K::Number, "Track width"), leaf("/scale/mode", K::Enum, "Scale"),
         leaf("/scale/zoom", K::Number, "Zoom"), leaf("/scale/panX", K::Number, "Pan x"),
         leaf("/scale/panY", K::Number, "Pan y"), leaf("/scale/rotation", K::Number, "Rotation (degrees)"),
         leaf("/background", K::Bool, "Draw as background")}}};
}

std::vector<Page> valuesListPages(const SchemaTree* s) {
    Page channels{
        tr("Channels"),
        {sub("/channels", K::Custom, "Channels",
             list(form({leaf("/channel", K::Channel, "Channel"),
                        leaf("/displayUnit", K::Text, "Display unit"), leaf("/dps", K::Integer, "Decimals")},
                       s, QStringLiteral("/channels/0")),
                  QJsonObject{{QStringLiteral("channel"), QString()}}))}};
    Page display{
        tr("Display"),
        {leaf("/columns/datum", K::Bool, "Datum column"), leaf("/columns/delta", K::Bool, "Delta column"),
         leaf("/columns/inlineUnits", K::Bool, "Units beside values"),
         leaf("/showHeading", K::Bool, "Heading"), leaf("/autoSizeColumns", K::Bool, "Size columns to fit"),
         leaf("/widths/name", K::Integer, "Name width"), leaf("/widths/value", K::Integer, "Value width"),
         leaf("/widths/delta", K::Integer, "Delta width"), leaf("/widths/datum", K::Integer, "Datum width")}};
    return {std::move(channels), std::move(display)};
}

std::vector<Page> gaugeNumericPages(const SchemaTree* s) {
    Page channels{tr("Channels"),
                  {leaf("/channel", K::Channel, "Channel"), leaf("/displayUnit", K::Text, "Display unit"),
                   leaf("/displayDps", K::Integer, "Decimals")}};
    Page display{
        tr("Display"),
        {leaf("/showUnits", K::Bool, "Show units"), leaf("/dynamicFont", K::Bool, "Size font to fit"),
         sub("/font", K::Custom, "Font",
             optional("Own font",
                      form({leaf("/family", K::Text, "Family"), leaf("/pointSize", K::Number, "Size (pt)"),
                            leaf("/bold", K::Bool, "Bold"), leaf("/italic", K::Bool, "Italic")},
                           s, QStringLiteral("/font")),
                      QJsonObject{{QStringLiteral("family"), QStringLiteral("Noto Sans")},
                                  {QStringLiteral("pointSize"), 24}})),
         leaf("/bandMode", K::Enum, "Colour bands colour the")}};
    Page bands{tr("Colour Bands"),
               {sub("/colourBands", K::ColourBands, "Colour bands",
                    colourBands(s, QStringLiteral("/colourBands")))}};
    return {std::move(channels), std::move(display), std::move(bands)};
}

std::vector<Page> gaugeBarPages(const SchemaTree* s) {
    Page channels{tr("Channels"),
                  {leaf("/channel", K::Channel, "Channel"), leaf("/displayUnit", K::Text, "Display unit")}};
    Page display{tr("Display"), {}};
    scaleRows(display.rows, "/scale", "Bar");
    for (FieldRow r :
         {leaf("/orientation", K::Enum, "Orientation"), leaf("/originStyle", K::Enum, "Bar starts at")}) {
        display.rows.push_back(std::move(r));
    }
    display.rows.push_back(unitValue("/customOrigin", "Custom start"));
    display.rows.push_back(
        sub("/barColour", K::Custom, "Bar colour",
            optional(
                "Own colour",
                [](QWidget* parent) -> FieldEditor* { return new LeafEditor(K::Colour, {}, parent); },
                QStringLiteral("#2060c0"))));
    display.rows.push_back(leaf("/scaleDps", K::Integer, "Scale decimals"));
    display.rows.push_back(leaf("/showScale", K::Bool, "Show scale"));
    display.rows.push_back(leaf("/bandLines", K::Enum, "Band lines"));
    display.rows.push_back(leaf("/bandMode", K::Enum, "Colour bands colour the"));
    Page bands{tr("Colour Bands"),
               {sub("/colourBands", K::ColourBands, "Colour bands",
                    colourBands(s, QStringLiteral("/colourBands")))}};
    return {std::move(channels), std::move(display), std::move(bands)};
}

} // namespace

std::vector<Page> pagesFor(const QString& type, const SchemaTree* schema) {
    if (type == QLatin1String("time_distance")) {
        return graphPages(schema, false);
    }
    if (type == QLatin1String("outing")) {
        return graphPages(schema, true);
    }
    if (type == QLatin1String("scatter")) {
        return scatterPages(schema);
    }
    if (type == QLatin1String("histogram")) {
        return histogramPages(schema);
    }
    if (type == QLatin1String("channel_report")) {
        return channelReportPages(schema);
    }
    if (type == QLatin1String("time_report")) {
        return timeReportPages(schema);
    }
    if (type == QLatin1String("track_position")) {
        return trackPositionPages();
    }
    if (type == QLatin1String("values_list")) {
        return valuesListPages(schema);
    }
    if (type == QLatin1String("gauge_numeric")) {
        return gaugeNumericPages(schema);
    }
    if (type == QLatin1String("gauge_bar")) {
        return gaugeBarPages(schema);
    }
    return {};
}

std::vector<FieldRow> headerRows() {
    return {leaf("/caption/show", K::Bool, "Show caption"),
            leaf("/caption/auto", K::Bool, "Automatic caption"),
            leaf("/caption/text", K::Text, "Caption"),
            leaf("/links/zoom", K::Bool, "Follow the zoom"),
            leaf("/links/cursor", K::Bool, "Follow the cursor"),
            leaf("/links/datum", K::Bool, "Follow the datum"),
            leaf("/legend/channels", K::Bool, "Channel legend"),
            leaf("/legend/colour", K::Bool, "Colour legend")};
}

int channelsTab(const std::vector<Page>& pages) {
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (pages[i].title == tr("Channels") || pages[i].title == tr("Sections")) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

} // namespace esses::widgets::pages
