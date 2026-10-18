// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers of the properties/shell Qt Tests (header only, so nothing is compiled into the other esses_widgets
// test executables): a MainWindow on a private configuration directory and the repository's data directory,
// a project folder written by ProjectSession::newProject, the component bodies of schemas/examples, and the
// generic leaf mutation of the AC-M3-08 round trip (every leaf editor kind gets a new value its schema node
// allows). Namespace esses::widgets::shelltest.
#pragma once

#include "components/component_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/uuid.hpp>
#include <esses/project/component_spec.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>
#include <esses/widgets/shell/main_window.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLocale>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace esses::widgets::shelltest {

inline std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdWString());
}

inline Paths pathsFor(const QString& configDir) {
    PathOverrides o;
    o.configDir = fsPath(configDir);
    o.dataDir = cmptest::repoRoot() / "data";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve: %s", p.error().toString().c_str());
    }
    return *p;
}

inline std::unique_ptr<qt::AppRuntime> runtime(unsigned workers = 2) {
    qt::AppRuntimeOptions o;
    o.workers = workers;
    o.cacheBudget = std::size_t{128} << 20;
    o.markGuiThreadNoBlock = true; // as esses (AC-M1-12)
    return std::make_unique<qt::AppRuntime>(o);
}

inline bool showAndActivate(QWidget& w) {
    w.show();
    if (!QTest::qWaitForWindowExposed(&w)) {
        return false;
    }
    w.activateWindow();
    return QTest::qWaitForWindowActive(&w);
}

// A new project (the shipped default workbook) in `folder`, closed again.
inline bool makeProject(const Paths& paths, const std::filesystem::path& folder, const std::string& name) {
    qt::ProjectSession maker(paths, nullptr);
    if (!maker.loadDefaults() || !maker.newProject(folder, name)) {
        return false;
    }
    maker.closeProject();
    return true;
}

// schemas/examples/components/<type>-1/valid/<which>.json as a JsonDoc.
inline JsonDoc exampleBody(const std::string& type, const std::string& which = "full") {
    Result<JsonDoc> doc = JsonDoc::load(cmptest::repoRoot() / "schemas" / "examples" / "components" /
                                        (type + "-1") / "valid" / (which + ".json"));
    return doc ? std::move(doc).value() : JsonDoc();
}

inline project::ComponentSpec componentOf(const std::string& type, const JsonDoc& body, project::Rect rect,
                                          int z) {
    project::ComponentSpec c;
    c.id = uuidRandom();
    c.type = type;
    c.version = 1;
    c.rect = rect;
    c.z = z;
    c.body = body;
    c.known = project::isKnownComponentType(type);
    return c;
}

inline const char* const kMvpTypes[] = {"time_distance",  "outing",      "scatter",        "histogram",
                                        "channel_report", "time_report", "track_position", "values_list",
                                        "gauge_numeric",  "gauge_bar"};

// A value of a number field that differs from `current` and lies inside the node's limits.
inline double otherNumber(const QJsonObject& node, std::optional<double> current) {
    const bool hasMin =
        node.contains(QStringLiteral("minimum")) || node.contains(QStringLiteral("exclusiveMinimum"));
    const bool hasMax =
        node.contains(QStringLiteral("maximum")) || node.contains(QStringLiteral("exclusiveMaximum"));
    const double lo = node.contains(QStringLiteral("minimum"))
                          ? node.value(QStringLiteral("minimum")).toDouble()
                          : node.value(QStringLiteral("exclusiveMinimum")).toDouble();
    const double hi = node.contains(QStringLiteral("maximum"))
                          ? node.value(QStringLiteral("maximum")).toDouble()
                          : node.value(QStringLiteral("exclusiveMaximum")).toDouble();
    double v = 0.0;
    if (hasMin && hasMax) {
        v = lo + (hi - lo) * 0.375;
        if (current && std::abs(*current - v) < 1e-12) {
            v = lo + (hi - lo) * 0.625;
        }
    } else if (hasMin) {
        v = std::max(lo + 1.25, current.value_or(lo) + 1.25);
    } else if (hasMax) {
        v = std::min(hi - 1.25, current.value_or(hi) - 1.25);
    } else {
        v = current.value_or(0.0) + 1.25;
    }
    if (node.value(QStringLiteral("type")).toString() == QLatin1String("integer")) {
        v = std::round(v);
    }
    return v;
}

// Gives the leaf widget `w` (schema node `node`) another value, as the user would; false for a widget kind
// this does not know.
inline bool mutateLeaf(QWidget* w, const QJsonObject& node) {
    if (auto* box = qobject_cast<QCheckBox*>(w)) {
        box->click();
        return true;
    }
    if (auto* combo = qobject_cast<QComboBox*>(w)) {
        if (combo->isEditable()) {
            combo->setEditText(combo->currentText().isEmpty() ? QStringLiteral("Changed Channel")
                                                              : combo->currentText() + QStringLiteral(" B"));
            return true;
        }
        if (combo->count() == 0) {
            return false;
        }
        combo->setCurrentIndex(combo->currentIndex() < 0 ? 0 : (combo->currentIndex() + 1) % combo->count());
        return true;
    }
    if (auto* spin = qobject_cast<QSpinBox*>(w)) {
        const int lo = node.contains(QStringLiteral("minimum"))
                           ? node.value(QStringLiteral("minimum")).toInt()
                           : spin->minimum();
        const int hi = node.contains(QStringLiteral("maximum"))
                           ? node.value(QStringLiteral("maximum")).toInt()
                           : spin->maximum();
        int v = spin->value() + 1;
        if (v > hi || v < lo) {
            v = spin->value() - 1 >= lo ? spin->value() - 1 : lo;
        }
        if (v == spin->value()) {
            v = std::min(hi, lo + 1);
        }
        spin->setValue(v);
        return true;
    }
    if (auto* number = qobject_cast<NumberEdit*>(w)) {
        number->setText(
            QString::number(otherNumber(node, number->value()), 'g', QLocale::FloatingPointShortest));
        return true;
    }
    if (auto* colour = qobject_cast<ColourEdit*>(w)) {
        colour->setText(colour->text().toLower() == QLatin1String("#1a2b3c") ? QStringLiteral("#4d5e6f")
                                                                             : QStringLiteral("#1a2b3c"));
        return true;
    }
    if (auto* line = qobject_cast<QLineEdit*>(w)) {
        line->setText(line->text().isEmpty() ? QStringLiteral("Changed")
                                             : line->text() + QStringLiteral("x"));
        return true;
    }
    return false;
}

} // namespace esses::widgets::shelltest
