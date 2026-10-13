// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The CSV writers (05 section 6 "Writer"): the legacy skeleton (eleven metadata lines, quoted fields, CRLF,
// one rate, "" for invalid samples) read back by the motec.csv importer, and the plain table read back by
// the generic profile; resampling onto one grid, display units, ranges, errors.
#include "text_test_support.hpp"

#include <catch2/catch_approx.hpp>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::texttest;
using Catch::Approx;

namespace {

// A 10 Hz source (generic profile) whose Throttle has an invalid sample (the NaN sentinel) at 0.1 s.
constexpr std::string_view kSource = "Time,Speed,Throttle\n"
                                     "s,km/h,%\n"
                                     "0.0,36,10\n"
                                     "0.1,72,NaN\n"
                                     "0.2,108,30\n"
                                     "0.3,144,40\n";

} // namespace

TEST_CASE("csv writers: legacy skeleton round trip through the motec.csv importer", "[unit]") {
    Env env;
    auto o = env.open(kSource, "in.csv");
    REQUIRE(o.ok());
    // Add a detail the legacy header carries.
    auto src = std::make_shared<model::DataSource>(*o->source);
    src->details.push_back(
        model::Detail{"Venue", std::string("Testville"), model::Provenance::Logged, std::nullopt});
    TempDir dir("writer");
    const auto out = dir.path() / "export.csv";
    IWriter* w = env.registry->writer("motec.csv");
    REQUIRE(w != nullptr);
    ExportView view;
    view.source = src;
    WriterOptions wo;
    auto opt = model::OpaqueJson::parse(R"({"csv": {"units": {"Speed": "km/h"}}})");
    REQUIRE(opt.ok());
    wo.options = *opt;
    auto rep = w->write(out, view, wo, {}, {});
    REQUIRE(rep.ok());
    const std::string text = readText(out);
    CHECK(rep->bytes == text.size());
    CHECK(text.starts_with("\"Format\",\"" + std::string(kLegacyCsvFormatName) + "\",,,\"Workbook\","));
    CHECK(text.find("\"Venue\",\"Testville\"") != std::string::npos);
    CHECK(text.find("\"Sample Rate\",\"10.000\",\"Hz\"") != std::string::npos);
    CHECK(text.find("\r\n\r\n\r\n\"Time\",\"Speed\",\"Throttle\"\r\n\"s\",\"km/h\",\"ratio\"\r\n\r\n\r\n") !=
          std::string::npos);
    CHECK(text.find("\"0.100\",\"72\",\"\"\r\n") != std::string::npos); // the invalid sample is ""

    auto back = env.open(text, "export.csv");
    REQUIRE(back.ok());
    CHECK(back->probe.importerId == kLegacyCsvImporterId);
    CHECK(std::get<Rate>(back->probe.layers[0].channels[0].timing) == Rate{10, 1});
    const Dump speed = dumpSeries(channel(*back, "Speed").series);
    REQUIRE(speed.values.size() == 4);
    CHECK(speed.values[3] == Approx(40.0)); // 144 km/h in m/s
    const Dump thr = dumpSeries(channel(*back, "Throttle").series);
    // The legacy profile holds empty cells (sparse "hold"): the invalid sample written as "" reads back as
    // the previous value. The plain writer below keeps it invalid.
    CHECK(thr.values[1] == Approx(0.1));
    CHECK(thr.values[2] == Approx(0.3));
}

TEST_CASE("csv writers: plain table round trip, resampling and ranges", "[unit]") {
    Env env;
    auto o = env.open(kSource, "in.csv");
    REQUIRE(o.ok());
    TempDir dir("writer");
    const auto out = dir.path() / "plain.csv";
    IWriter* w = env.registry->writer("csv");
    REQUIRE(w != nullptr);
    ExportView view;
    view.source = o->source;
    view.channels = {channel(*o, "Throttle").desc.id};
    view.resampleRate = Rate{20, 1};
    view.start = 100'000;
    auto rep = w->write(out, view, {}, {}, {});
    REQUIRE(rep.ok());
    const std::string text = readText(out);
    INFO(text);
    CHECK(text.starts_with("Time,Throttle\ns,ratio\n0.000,"));
    CHECK(text.find('\r') == std::string::npos);
    auto back = env.open(text, "plain.csv");
    REQUIRE(back.ok());
    CHECK(back->probe.importerId == kGenericCsvImporterId);
    CHECK(detailOf(back->probe.warnings, "text.auto_units_row") == "yes");
    const auto& thr = channel(*back, "Throttle");
    CHECK(detailOf(back->probe.warnings, "text.rate_inferred") == "20 Hz");
    REQUIRE(std::holds_alternative<store::TimestampedSeries>(thr.series)); // the empty cells make it sparse
    const Dump d = dumpSeries(thr.series);
    // Throttle's cells are whole numbers, so it is an Integer channel drawn with Previous: 20 Hz from 0.1 s
    // gives invalid (0.1 s), invalid (still the 0.1 s sample), 0.3, 0.3, 0.4 and nothing after its last
    // sample (0.3 s). Time restarts at 0 at the range start; the empty cells make it sparse when read back.
    CHECK(d.times == std::vector<Micros>{100'000, 150'000, 200'000});
    CHECK(d.values[0] == Approx(0.3));
    CHECK(d.values[1] == Approx(0.3));
    CHECK(d.values[2] == Approx(0.4));

    view.start = 500'000;
    CHECK(w->write(out, view, {}, {}, {}).error().code == ErrorCode::Invalid); // an empty range
    WriterOptions bad;
    bad.options = *model::OpaqueJson::parse(R"({"ld": {}})");
    view.start = 0;
    CHECK(w->write(out, view, bad, {}, {}).error().code == ErrorCode::Invalid);
}
