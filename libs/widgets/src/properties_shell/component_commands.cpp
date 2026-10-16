// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The generic component rows (06 section 14.4, 04 section 7.2 item 4). Owner: src/properties_shell/.

#include <esses/widgets/properties_shell/component_commands.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

namespace esses::widgets {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("esses::widgets::ComponentCommands", text);
}

struct Cycle {
    const char* pointer;
    QStringList values; // the first is the default
};

// The time_distance graph's body members G, Y, E and SHIFT+E cycle (it answers them itself; this is the
// fallback). The outing graph draws none of them (OutingGeometry reads keyMode and traceStyle only, D-198
// (g)), so those rows are "Not available" there rather than inert undo steps (D-203 (h)).
bool isGraph(const QString& type) {
    return type == QLatin1String("time_distance");
}

std::optional<Cycle> cycleOf(const QString& type, const QString& command) {
    if (command == QLatin1String("component.cycle_layout")) {
        if (isGraph(type)) {
            return Cycle{"/display/overlapMode", {QStringLiteral("panels"), QStringLiteral("overlapped")}};
        }
        if (type == QLatin1String("scatter")) {
            return Cycle{"/layout",
                         {QStringLiteral("overlapped"), QStringLiteral("tiled"), QStringLiteral("average")}};
        }
        if (type == QLatin1String("histogram")) {
            return Cycle{"/layout", {QStringLiteral("overlapped"), QStringLiteral("tiled")}};
        }
        if (type == QLatin1String("channel_report")) {
            return Cycle{"/layout", {QStringLiteral("channelsInRows"), QStringLiteral("channelsInColumns")}};
        }
        return std::nullopt;
    }
    if (command == QLatin1String("component.cycle_style")) {
        if (isGraph(type)) {
            return Cycle{"/display/traceStyle", {QStringLiteral("line"), QStringLiteral("points")}};
        }
        if (type == QLatin1String("scatter")) {
            return Cycle{"/style",
                         {QStringLiteral("points"), QStringLiteral("pointsTrend"), QStringLiteral("trend")}};
        }
        if (type == QLatin1String("histogram")) {
            return Cycle{"/style", {QStringLiteral("bar"), QStringLiteral("line"), QStringLiteral("text")}};
        }
        return std::nullopt;
    }
    if (command == QLatin1String("cursor.style")) {
        if (isGraph(type)) {
            return Cycle{"/display/cursorStyle",
                         {QStringLiteral("line"), QStringLiteral("crosshair"), QStringLiteral("none")}};
        }
        if (type == QLatin1String("scatter")) {
            return Cycle{"/cursorStyle",
                         {QStringLiteral("cross"), QStringLiteral("ring"), QStringLiteral("none")}};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

struct Toggle {
    const char* pointer;
    bool defaultValue;
};

std::optional<Toggle> toggleOf(const QString& type, const QString& command) {
    if (command == QLatin1String("graph.status_errors") && isGraph(type)) {
        return Toggle{"/display/showStatusStrip", false};
    }
    if (command == QLatin1String("graph.device_events") && isGraph(type)) {
        return Toggle{"/display/showDeviceEvents", true};
    }
    if (command == QLatin1String("graph.variance") && type == QLatin1String("time_distance")) {
        return Toggle{"/display/variance/show", false};
    }
    if (command == QLatin1String("graph.measurements") && type == QLatin1String("time_distance")) {
        return Toggle{"/display/key/stats", false};
    }
    return std::nullopt;
}

const char* bandListOf(const QString& type) {
    if (type == QLatin1String("time_report")) {
        return "/bands";
    }
    if (type == QLatin1String("gauge_numeric") || type == QLatin1String("gauge_bar")) {
        return "/colourBands";
    }
    return nullptr;
}

// The components that draw a colour-channel legend (K). track_position draws none until track contours
// exist (v1.0), so K is "Not available" there (D-203 (h)).
bool hasColourLegend(const QString& type) {
    return type == QLatin1String("scatter") || type == QLatin1String("histogram");
}

} // namespace

bool hasGenericComponentEdit(const QString& type, const QString& commandId) {
    if (commandId == QLatin1String("component.legend")) {
        // L toggles a key the component draws: the graphs answer it themselves (the key column), and no
        // other component draws spec.legend.channels, so there is no generic edit (D-203 (h)).
        return false;
    }
    if (commandId == QLatin1String("component.colour_legend")) {
        return hasColourLegend(type);
    }
    if (commandId == QLatin1String("component.colour_band")) {
        return bandListOf(type) != nullptr;
    }
    return cycleOf(type, commandId).has_value() || toggleOf(type, commandId).has_value();
}

std::optional<project::ComponentSpec> genericComponentEdit(const project::ComponentSpec& spec,
                                                           const QString& commandId, const QVariantList& args,
                                                           QString* undoText) {
    const QString type = QString::fromStdString(spec.type);
    project::ComponentSpec next = spec;
    const auto text = [undoText](const char* t) {
        if (undoText != nullptr) {
            *undoText = tr(t);
        }
    };
    if (commandId == QLatin1String("component.legend")) {
        return std::nullopt; // see hasGenericComponentEdit
    }
    if (commandId == QLatin1String("component.colour_legend")) {
        if (!hasColourLegend(type)) {
            return std::nullopt;
        }
        next.legend.colour = !spec.legend.colour;
        text("Toggle colour legend");
        return next;
    }
    const QJsonValue body = spec.body.isObject() ? jsonptr::fromDoc(spec.body) : QJsonValue(QJsonObject());
    if (commandId == QLatin1String("component.colour_band")) {
        const char* list = bandListOf(type);
        const int n = args.isEmpty() ? 0 : args.front().toInt();
        if (list == nullptr || n < 0) {
            return std::nullopt;
        }
        const QJsonArray bands = jsonptr::get(body, QString::fromLatin1(list)).toArray();
        if (n >= bands.size()) {
            return std::nullopt; // bands beyond the list are no-ops (04 section 7.2 item 4)
        }
        const QString pointer = QStringLiteral("%1/%2/visible").arg(QLatin1String(list)).arg(n);
        const bool visible = jsonptr::get(body, pointer).toBool(true);
        next.body = jsonptr::toDoc(jsonptr::set(body, pointer, !visible));
        text("Toggle colour band");
        return next;
    }
    if (const std::optional<Cycle> c = cycleOf(type, commandId)) {
        const QString pointer = QString::fromLatin1(c->pointer);
        const QString current = jsonptr::get(body, pointer).toString(c->values.front());
        const qsizetype at = c->values.indexOf(current);
        const QString value = c->values[(at < 0 ? 0 : at + 1) % c->values.size()];
        next.body = jsonptr::toDoc(jsonptr::set(body, pointer, value));
        text(commandId == QLatin1String("component.cycle_layout")  ? "Change layout"
             : commandId == QLatin1String("component.cycle_style") ? "Change style"
                                                                   : "Change cursor style");
        return next;
    }
    if (const std::optional<Toggle> t = toggleOf(type, commandId)) {
        const QString pointer = QString::fromLatin1(t->pointer);
        const bool current = jsonptr::get(body, pointer).toBool(t->defaultValue);
        next.body = jsonptr::toDoc(jsonptr::set(body, pointer, !current));
        text(commandId == QLatin1String("graph.variance")        ? "Toggle variance panel"
             : commandId == QLatin1String("graph.measurements")  ? "Toggle measurements"
             : commandId == QLatin1String("graph.status_errors") ? "Toggle status strip"
                                                                 : "Toggle device events");
        return next;
    }
    return std::nullopt;
}

} // namespace esses::widgets
