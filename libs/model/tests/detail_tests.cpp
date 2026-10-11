// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Details, DateTime and the merge precedence sidecar > companion > file (02 section 4.2, section 5.3 rule 2).
#include "model_test_support.hpp"

#include <esses/model/detail.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace esses;
using namespace esses::model;

TEST_CASE("detail: provenance names round-trip", "[unit]") {
    for (const Provenance p :
         {Provenance::Logged, Provenance::Companion, Provenance::User, Provenance::Derived}) {
        CHECK(parseProvenance(provenanceName(p)) == p);
    }
    CHECK_FALSE(parseProvenance("Imported").has_value());
    CHECK_FALSE(parseProvenance("user").has_value());
}

TEST_CASE("detail: DateTime ISO form round-trips", "[unit]") {
    struct Case {
        const char* text;
        std::int64_t unixUs;
        int offset;
    };
    const Case cases[] = {
        {"1970-01-01T00:00:00Z", 0, 0},
        {"2026-10-09T21:14:00+10:00", 1'791'544'440'000'000, 600},
        {"2026-10-10T08:00:00.250-03:30", 1'791'631'800'250'000, -210},
        {"2026-10-10T08:00:00.000001Z", 1'791'619'200'000'001, 0},
        {"1969-12-31T23:59:59.999Z", -1'000, 0},
        {"2000-02-29T12:00:00Z", 951'825'600'000'000, 0},
    };
    for (const Case& c : cases) {
        INFO(c.text);
        const auto dt = DateTime::parseIso(c.text);
        REQUIRE(dt.has_value());
        CHECK(dt->unixUs == c.unixUs);
        CHECK(dt->tzOffsetMin == c.offset);
        CHECK(dt->iso() == c.text);
    }
}

TEST_CASE("detail: DateTime parsing normalises and rejects", "[unit]") {
    CHECK(DateTime::parseIso("2026-10-10T08:00:00+00:00")->iso() == "2026-10-10T08:00:00Z");
    CHECK(DateTime::parseIso("2026-10-10T08:00:00.5Z")->iso() == "2026-10-10T08:00:00.500Z");
    CHECK(DateTime::parseIso("2026-10-10T08:00:00.123456789Z")->unixUs % 1'000'000 == 123'456); // truncated
    for (const char* bad :
         {"2026-10-10T08:00:00", "2026-10-10 08:00:00Z", "2026-13-01T00:00:00Z", "2026-02-29T00:00:00Z",
          "2026-10-10T24:00:00Z", "2026-10-10T08:00:60Z", "2026-10-10T08:00:00.Z", "2026-10-10T08:00:00+1000",
          "26-10-10T08:00:00Z", "2026-10-10T08:00:00Zjunk", ""}) {
        INFO(bad);
        CHECK_FALSE(DateTime::parseIso(bad).has_value());
    }
    const DateTime now = DateTime::nowUtc();
    CHECK(now.tzOffsetMin == 0);
    CHECK(now.unixUs > 1'760'000'000'000'000);
    CHECK(DateTime::parseIso(now.iso()) == now);
}

TEST_CASE("detail: the standard detail table", "[unit]") {
    std::set<std::string_view> ids;
    for (const StandardDetail& s : standardDetails()) {
        INFO(s.id);
        CHECK(ids.insert(s.id).second);
        if (!s.quantity.empty()) {
            CHECK(s.type == DetailType::Number);
            CHECK(test::shippedUnits().quantityByName(s.quantity).has_value()); // every quantity exists
        }
    }
    for (const char* id : {"Event", "Venue", "Driver", "Venue Length", "Start Lap", "Log Date",
                           "Fastest Time", "File Name", "Air Temp", "Baro Pressure", "Device Serial",
                           "Vehicle Wheelbase", "Diff Ratio", "Gear 10", "Time 1320 ft", "Stoich Ratio"}) {
        INFO(id);
        CHECK(findStandardDetail(id) != nullptr);
    }
    CHECK(findStandardDetail("diff ratio")->quantity == "Ratio");
    CHECK(findStandardDetail("File Size")->derived);
    CHECK_FALSE(findStandardDetail("Driver")->derived);
    CHECK(findStandardDetail("Log Time")->type == DetailType::DateTime);
    CHECK(findStandardDetail("Custom Thing") == nullptr);
}

TEST_CASE("detail: findDetail is case-insensitive, exact first", "[unit]") {
    std::vector<Detail> details{{"Driver", std::string("A"), Provenance::Logged, std::nullopt},
                                {"driver", std::string("B"), Provenance::Logged, std::nullopt}};
    CHECK(std::get<std::string>(findDetail(details, "driver")->value) == "B");
    CHECK(std::get<std::string>(findDetail(details, "DRIVER")->value) == "A");
    CHECK(findDetail(details, "Venue") == nullptr);
}

TEST_CASE("detail: value text", "[unit]") {
    CHECK(detailValueText(std::string("Bathurst")) == "Bathurst");
    Quantityd q;
    q.v = 3.9;
    q.dps = 2;
    q.symbol = "ratio";
    CHECK(detailValueText(q) == "3.90 ratio");
    q.symbol.clear();
    q.dps = 0;
    q.v = 12.0;
    CHECK(detailValueText(q) == "12");
    CHECK(detailValueText(DateTime{0, 60}) == "1970-01-01T01:00:00+01:00");
}

TEST_CASE("detail: merge precedence sidecar > companion > file (D-015)", "[unit]") {
    const auto text = [](const char* s) { return DetailValue{std::string(s)}; };
    std::vector<Detail> file{{"Driver", text("MMINTON"), Provenance::Logged, std::nullopt},
                             {"Venue", text("Bathurst"), Provenance::Logged, std::nullopt},
                             {"Session", text("R1"), Provenance::Logged, std::nullopt}};
    std::vector<Detail> companion{{"venue", text("Mount Panorama"), Provenance::Logged, std::nullopt},
                                  {"Total Laps", text("12"), Provenance::Logged, std::nullopt}};
    std::vector<Detail> sidecar{{"Driver", text("M. Minton"), Provenance::Logged, text("MMINTON")},
                                {"VENUE", text("Mt Panorama"), Provenance::Logged, std::nullopt},
                                {"Team", text("Esses"), Provenance::Logged, std::nullopt}};
    const auto merged = mergeDetails(file, companion, sidecar);
    REQUIRE(merged.size() == 5);
    // file order, then new companion ids, then new sidecar ids
    CHECK(merged[0].id == "Driver");
    CHECK(merged[1].id == "Venue");
    CHECK(merged[2].id == "Session");
    CHECK(merged[3].id == "Total Laps");
    CHECK(merged[4].id == "Team");

    CHECK(std::get<std::string>(merged[0].value) == "M. Minton");
    CHECK(merged[0].prov == Provenance::User);
    CHECK(std::get<std::string>(*merged[0].original) == "MMINTON");

    CHECK(std::get<std::string>(merged[1].value) == "Mt Panorama");
    CHECK(merged[1].prov == Provenance::User);
    CHECK(std::get<std::string>(*merged[1].original) == "Mount Panorama"); // the value it replaced

    CHECK(merged[2].prov == Provenance::Logged);
    CHECK_FALSE(merged[2].original.has_value());
    CHECK(merged[3].prov == Provenance::Companion);
    CHECK(merged[4].prov == Provenance::User);
    CHECK_FALSE(merged[4].original.has_value());
}

TEST_CASE("detail: numeric values resolve their unit through the registry", "[unit]") {
    const auto& reg = test::shippedUnits();
    Quantityd length;
    length.v = 6.213;
    length.symbol = "km";
    CHECK(resolveUnit(length, reg).ok());
    CHECK(reg.unit(length.unit).symbol == "km");
    CHECK(toBase(length, reg).value() == Catch::Approx(6213.0));

    Quantityd temp;
    temp.v = 25.0;
    temp.symbol = "deg C";
    CHECK(toBase(temp, reg).value() == Catch::Approx(298.15)); // resolves on the fly
    Quantityd bad;
    bad.symbol = "furlongs";
    const auto r = resolveUnit(bad, reg);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::NotFound);
    CHECK_FALSE(toBase(bad, reg).has_value());
    Quantityd unitless;
    unitless.v = 14.7;
    CHECK(toBase(unitless, reg).value() == 14.7);

    const Quantityd made = makeQuantity(0.5, reg.find("m").value(), reg, 3);
    CHECK(made.symbol == "m");
    CHECK(made.dps == 3);

    std::vector<Detail> details{{"Venue Length", length, Provenance::User, std::nullopt},
                                {"Odd", bad, Provenance::User, DetailValue{temp}}};
    std::get<Quantityd>(details[0].value).unit = units::kNoUnit;
    const auto notes = resolveDetailUnits(details, reg);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].code == kNoteDetailUnitUnknown);
    CHECK(notes[0].ref == "Odd");
    CHECK(std::get<Quantityd>(details[0].value).unit == reg.find("km").value());
    CHECK(std::get<Quantityd>(*details[1].original).unit == reg.find("deg C").value());
}
