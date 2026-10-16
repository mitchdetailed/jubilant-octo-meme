// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Header-only helpers of the reports and track widget tests (tests/components/reports_track/tst_*.cpp): the
// qtmodel fixtures (report_track_fixtures.hpp: hand-written range trees with closed-form channels, the oval,
// rectangle and synthetic-circuit maps and sources), publishing a source into a cmptest::World, component
// specs from JSON bodies, and waiting for a ReportModel delivery.
#pragma once

#include "../../../../qtmodel/tests/reports/report_track_fixtures.hpp"
#include "components/component_test_support.hpp"

#include <esses/qtmodel/report_model.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <memory>
#include <string>

namespace esses::widgets::rtw {

using namespace esses::qt::rttest;

inline std::shared_ptr<const model::DataSource> publish(cmptest::World& w,
                                                        std::shared_ptr<const model::DataSource> src) {
    w.maths.addSource(src);
    w.distance.prefetch(src->id);
    w.selection.sourcePublished(src);
    return src;
}

inline JsonDoc docOf(const QJsonObject& o) {
    Result<JsonDoc> doc = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());
    return doc ? std::move(doc).value() : JsonDoc{};
}

inline QJsonObject objectOf(const JsonDoc& doc) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(doc.compact())).object();
}

inline project::ComponentSpec componentSpec(const std::string& type, const QJsonObject& body,
                                            const std::string& id = {}) {
    project::ComponentSpec spec;
    spec.id = id.empty() ? uuidRandom() : Uuid::parse(id);
    spec.type = type;
    spec.body = docOf(body);
    return spec;
}

// The three laps of 10 s (Lap 2 untrusted) with Turn 1 / Straight 1 per lap (tst_report_model's source).
inline std::shared_ptr<const model::DataSource> threeLaps(cmptest::World& w,
                                                          const std::string& tag = "rt-a") {
    const std::vector<RangeSpec> laps{{"Lap 1", 0, 10}, {"Lap 2", 10, 20, false}, {"Lap 3", 20, 30}};
    const std::vector<RangeSpec> sections{{"Turn 1", 0, 3},     {"Straight 1", 3, 10},
                                          {"Turn 1", 10, 12},   {"Straight 1", 12, 20},
                                          {"Turn 1", 20, 22.5}, {"Straight 1", 22.5, 30}};
    return publish(w, reportSource(tag, 30.0, laps, sections, {}, *cmptest::shippedUnits(), w.rt.cache()));
}

inline bool settle(const qt::ReportModel& m, int before, int timeoutMs = 20000) {
    return QTest::qWaitFor([&] { return !m.busy() && m.deliveries() > before; }, timeoutMs);
}

} // namespace esses::widgets::rtw
