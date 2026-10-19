// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The motec.ld IWriter (05 section 5):
//   * read -> write -> read of a synthetic .ld + .ldx through the public API keeps the stored samples, gap
//     blocks, records, header bytes, enumerations, device events and details (item 8), and the unknown .ldx
//     elements byte-identical at their positions (AC-M0-03);
//   * an in-memory DataSource (the fixture generator's path) is written with its LdSourceEncoding or the
//     default encodings and reads back with the same values and validity;
//   * options, skipped channels and errors.
#include "formats_test_support.hpp"
#include "ld/ldx.hpp"
#include "synthetic_families.hpp"

#include <esses/store/memory_provider.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::test;
namespace fs = std::filesystem;

namespace {

std::shared_ptr<const ld::LdFile> parseLdFile(const fs::path& p) {
    auto f = ld::LdFile::open(p);
    REQUIRE(f.ok());
    return *f;
}

ld::LdxDocument parseLdxFile(const fs::path& p) {
    ImportLog log;
    auto d = ld::readLdxFile(p, log);
    REQUIRE(d.ok());
    return std::move(d).value();
}

std::span<const std::byte> storedBytes(const ld::LdFile& f, const ld::LdChannel& c) {
    return f.bytes().subspan(c.enc.dataOffset, c.sampleCount * c.enc.bytesPerSample);
}

std::uint64_t floatUlps(double a, double b) {
    return ld::test::f32UlpDistance(a, b);
}

} // namespace

TEST_CASE("ld export: read -> write -> read keeps the data and the .ldx unknown elements byte-identical",
          "[unit][ld_export]") {
    ld::test::TempDir dir("export-roundtrip");
    const fs::path in = dir.path() / "in.ld";
    writeLd(in, mixedInput(), fixedIdentity());
    writeText(dir.path() / "in.ldx", richLdx());
    const auto reg = ImporterRegistry::withBuiltins(&shippedUnits());
    auto opened = reg->open(in, {}, context(&shippedUnits())); // with conversions to base units
    REQUIRE(opened.ok());

    ExportView view;
    view.source = opened->source;
    const fs::path out = dir.path() / "out.ld";
    std::vector<double> progress;
    auto rep = reg->writer("motec.ld")->write(out, view, {}, [&](double f) { progress.push_back(f); }, {});
    REQUIRE(rep.ok());
    REQUIRE(rep->files.size() == 2);
    CHECK(rep->files[0] == out);
    CHECK(rep->files[1] == dir.path() / "out.ldx");
    CHECK(rep->bytes == fs::file_size(out) + fs::file_size(dir.path() / "out.ldx"));
    REQUIRE(!progress.empty());
    CHECK(progress.back() == 1.0);
    CHECK_FALSE(fs::exists(dir.path() / "out.ld.tmp"));

    // The .ld, compared through the core reader: stored bytes, gaps, records, events, enumerations, details.
    const auto f1 = parseLdFile(in);
    const auto f2 = parseLdFile(out);
    REQUIRE(f2->channels().size() == f1->channels().size());
    for (std::size_t k = 0; k < f1->channels().size(); ++k) {
        const ld::LdChannel& a = f1->channels()[k];
        const ld::LdChannel& b = f2->channels()[k];
        INFO(a.loggedName);
        CHECK(b.loggedName == a.loggedName);
        CHECK(b.unit == a.unit);
        CHECK(b.invalid == a.invalid);
        CHECK(b.enumRef == a.enumRef);
        CHECK(b.dps == a.dps);
        CHECK(b.enc.counter == a.enc.counter);
        const auto da = storedBytes(*f1, a);
        const auto db = storedBytes(*f2, b);
        CHECK(std::equal(da.begin(), da.end(), db.begin(), db.end()));
    }
    CHECK(f2->header().driver == f1->header().driver);
    CHECK(f2->header().deviceType == "TESTLOG");
    CHECK(std::equal(f1->rawVehicleRecord().begin(), f1->rawVehicleRecord().end(),
                     f2->rawVehicleRecord().begin(), f2->rawVehicleRecord().end()));
    CHECK(f2->bytes()[ld::layout::kHdrProLogging] == std::byte{0});
    REQUIRE(f2->deviceEvents().size() == 3);
    CHECK(f2->deviceEvents()[2].timeUs == f1->deviceEvents()[2].timeUs); // the stored fraction survives
    REQUIRE(f2->enumTables().size() == 1);
    CHECK(f2->enumTables()[0].entries == f1->enumTables()[0].entries);

    // The probe of the written pair equals the original's (layers, markers, details, enumerations).
    auto again = reg->open(out, {}, context(&shippedUnits()));
    REQUIRE(again.ok());
    CHECK(again->probe.dialect == opened->probe.dialect);
    CHECK(again->probe.layers == opened->probe.layers);
    CHECK(again->probe.enumTables == opened->probe.enumTables);
    CHECK(again->probe.details == opened->probe.details);
    // ... and the converted columns are identical.
    for (const model::Channel& c : opened->source->layers[0].channels) {
        INFO(c.desc.name);
        const ColumnDump a = dumpColumn(columnOf(c));
        const ColumnDump b = dumpColumn(columnOf(channelNamed(*again->source, c.desc.name)));
        CHECK(a.valid == b.valid);
        REQUIRE(a.values.size() == b.values.size());
        for (std::size_t i = 0; i < a.values.size(); ++i) {
            CHECK(std::bit_cast<std::uint64_t>(a.values[i]) == std::bit_cast<std::uint64_t>(b.values[i]));
        }
    }

    // The .ldx: every unknown element verbatim, at the same parent and position; markers and details kept.
    const std::string text = readText(dir.path() / "out.ldx");
    for (const std::string& xml : richLdxUnknownElements()) {
        INFO(xml);
        CHECK(text.find(xml) != std::string::npos);
    }
    const ld::LdxDocument x1 = parseLdxFile(dir.path() / "in.ldx");
    const ld::LdxDocument x2 = parseLdxFile(dir.path() / "out.ldx");
    CHECK(x2.opaque == x1.opaque);
    CHECK(x2.rootAttributes == x1.rootAttributes);
    REQUIRE(x2.groups.size() == 1);
    CHECK(x2.groups[0].attributes == x1.groups[0].attributes);
    REQUIRE(x2.markers.size() == x1.markers.size());
    for (std::size_t i = 0; i < x1.markers.size(); ++i) {
        CHECK(x2.markers[i].className == x1.markers[i].className);
        CHECK(x2.markers[i].name == x1.markers[i].name);
        CHECK(x2.markers[i].flags == x1.markers[i].flags);
        CHECK(x2.markers[i].timeUs == x1.markers[i].timeUs);         // "15000000.25" keeps its fraction
        CHECK(x2.markers[i].attributes == x1.markers[i].attributes); // Colour="red" included
    }
    CHECK(x2.details == x1.details); // order, values and the unknown Source attribute

    // A second round trip is a fixed point for the .ldx text.
    auto third = reg->open(out, {}, context(&shippedUnits()));
    REQUIRE(third.ok());
    view.source = third->source;
    REQUIRE(reg->writer("motec.ld")->write(dir.path() / "out2.ld", view, {}, {}, {}).ok());
    CHECK(readText(dir.path() / "out2.ldx") == text);
}

TEST_CASE("ld export: sidecar edits reach the .ldx (ignored beacons, user markers and details)",
          "[unit][ld_export]") {
    ld::test::TempDir dir("export-edits");
    const fs::path in = dir.path() / "in.ld";
    writeLd(in, mixedInput(), fixedIdentity());
    writeText(dir.path() / "in.ldx", richLdx());
    const auto reg = ImporterRegistry::withBuiltins(&shippedUnits());
    auto opened = reg->open(in, {}, context());
    REQUIRE(opened.ok());
    model::SidecarDocument doc = model::newSidecarFor(*opened->source);
    const model::MarkerId bcn = model::markerIdLogged(model::MarkerClass::BCN, 5'000'000, "Beacons", 0);
    REQUIRE(doc.applyEdit("Normal", model::MarkerEdit{bcn, model::MarkerEditOp::Ignore}).ok());
    model::Marker user =
        model::makeLoggedMarker(model::MarkerClass::BCN, 20'000'000, "Beacons", "User beacon");
    user.prov = model::Provenance::User;
    user.id = {};
    doc.addUserMarker("Normal", user);
    doc.setDetail("Normal",
                  model::Detail{"Driver", std::string("Edited"), model::Provenance::User, std::nullopt});
    ExportView view;
    view.source = opened->source->withSidecar(doc);
    REQUIRE(reg->writer("motec.ld")->write(dir.path() / "out.ld", view, {}, {}, {}).ok());
    const ld::LdxDocument x = parseLdxFile(dir.path() / "out.ldx");
    REQUIRE(x.markers.size() == 4);
    CHECK(x.markers[0].className == "IGRDBCN");
    CHECK(x.markers[0].name == "Manual.1");
    CHECK(x.markers[3].name == "User beacon");
    CHECK(x.markers[3].timeUs == 20'000'000.0);
    const ld::LdxDetail* driver = x.findDetail("Driver");
    REQUIRE(driver != nullptr);
    CHECK(driver->value == "Edited");
}

TEST_CASE("ld export: an in-memory source is quantised with its encodings", "[unit][ld_export]") {
    const units::UnitRegistry& units = shippedUnits();
    store::ColumnOptions co;
    co.cache = &testCache();
    auto src = std::make_shared<model::DataSource>();
    src->id = Hash128{0x1111, 0x2222};
    src->prov.importerId = "test.memory";
    model::DataLayer layer;
    layer.duration = 10'000'000;
    const auto add = [&](std::string name, store::Column column, Rate rate, units::UnitId unit,
                         std::shared_ptr<const model::SourceEncoding> enc = nullptr) {
        model::Channel c;
        c.desc.name = name;
        c.desc.loggedName = name;
        c.desc.id = model::channelIdLogged(src->id, "Normal", name, "test.memory");
        c.desc.dtype = column.dtype();
        c.desc.kind =
            column.dtype() == store::Dtype::I32 ? store::SampleKind::Integer : store::SampleKind::Real;
        c.desc.unit = unit;
        c.desc.quantity = units.quantityOf(unit);
        c.desc.rate = rate;
        c.desc.dps = 2;
        c.desc.sourceEncoding = std::move(enc);
        c.series = store::UniformSeries{rate, 0, std::move(column)};
        layer.addChannel(std::move(c));
    };
    const units::UnitId kelvin = *units.find("K");
    const units::UnitId degF = *units.find("deg F");
    const units::UnitId ms = *units.find("m/s");
    // Base-unit values whose deg F readings are whole tenths of deg C (raw counts of the encoding below).
    std::vector<float> oilK;
    std::vector<bool> oilValid;
    for (int raw = 238; raw < 338; ++raw) {
        oilK.push_back(static_cast<float>(units.toBase(degF)(raw / 5.0 * 0.1 * 9.0 + 32.0)));
        oilValid.push_back(raw < 260 || raw >= 270);
    }
    LdEncoding oil;
    oil.typeCode = 0x03;
    oil.width = 2;
    oil.rateHz = 10;
    oil.shift = 32;
    oil.mul = 9;
    oil.scale = 5;
    oil.dec = 1;
    oil.dpsByte = 1;
    oil.unit = "F";
    add("Oil Temp", store::Column(store::InMemoryProvider::fromF32(oilK, oilValid), co), Rate{10, 1}, kelvin,
        std::make_shared<LdSourceEncoding>(oil));
    std::vector<float> speed;
    for (int i = 0; i < 100; ++i) {
        speed.push_back(0.25f * static_cast<float>(i));
    }
    add("Speed", store::Column(store::InMemoryProvider::fromF32(speed), co), Rate{10, 1}, ms);
    add("Small Count", store::Column(store::InMemoryProvider::fromI32({-3, 0, 7, 32767}), co), Rate{1, 1},
        units::kNoUnit);
    add("Big Count", store::Column(store::InMemoryProvider::fromI32({1, 1'000'000, -40'000}), co), Rate{1, 1},
        units::kNoUnit);
    add("Latitude", store::Column(store::InMemoryProvider::fromF64({-0.5912345678901234, 0.25}), co),
        Rate{2, 1}, units::kNoUnit);
    {
        model::Channel t;
        t.desc.name = "Stamped";
        t.desc.loggedName = "Stamped";
        t.desc.id = model::channelIdLogged(src->id, "Normal", "Stamped", "test.memory");
        auto ts =
            store::makeTimestampedSeries(store::Column(store::InMemoryProvider::fromI64({0, 5, 9}), co),
                                         store::Column(store::InMemoryProvider::fromF32({1, 2, 3}), co));
        REQUIRE(ts.ok());
        t.series = *ts;
        layer.addChannel(std::move(t));
    }
    layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::BCN, 4'000'000, "Beacons", "Lap", {},
                                                    77, 0, model::Provenance::User));
    src->layers.push_back(std::move(layer));
    model::Quantityd length;
    length.v = 6213.0;
    length.symbol = "m";
    src->details = {{"Driver", std::string("Fixture Driver"), model::Provenance::Logged, std::nullopt},
                    {"Venue Length", length, model::Provenance::Logged, std::nullopt},
                    {"Log Date", std::string("09/10/2026"), model::Provenance::Logged, std::nullopt},
                    {"Log Time", std::string("14:32:11"), model::Provenance::Logged, std::nullopt}};

    ld::test::TempDir dir("export-memory");
    const auto reg = ImporterRegistry::withBuiltins(&units);
    ExportView view;
    view.source = src;
    auto rep = reg->writer("motec.ld")->write(dir.path() / "fx.ld", view, {}, {}, {});
    REQUIRE(rep.ok());
    CHECK(logHas(rep->notes, "ld.channel_skipped")); // the timestamped channel
    REQUIRE(rep->files.size() == 2);                 // the user marker needs an .ldx

    const auto f = parseLdFile(dir.path() / "fx.ld");
    REQUIRE(f->channels().size() == 5);
    CHECK(f->channels()[0].enc.typeCode == 0x03);
    CHECK(f->channels()[0].unit == "F");
    CHECK(f->channels()[0].invalid == std::vector<ld::LdInterval>{{22, 32}});
    CHECK(f->channels()[1].enc.typeCode == 0x07);
    CHECK(f->channels()[1].unit == "m/s");
    CHECK(f->channels()[2].enc.typeCode == 0x03); // fits 16 bits
    CHECK(f->channels()[2].enc.bytesPerSample == 2);
    CHECK(f->channels()[3].enc.typeCode == 0x05); // does not
    CHECK(f->channels()[4].enc.typeCode == 0x08);
    CHECK(f->header().deviceType == "Esses"); // the writer's neutral identity (05 section 5 item 1)
    CHECK(f->header().driver == "Fixture Driver");
    CHECK(f->header().date == "09/10/2026");
    REQUIRE(f->venueRecord().has_value());
    CHECK(f->venueRecord()->lengthMm == 6'213'000);
    auto raw = f->rawValues(0, 0, 3);
    REQUIRE(raw.ok());
    CHECK(*raw == std::vector<double>{238.0, 239.0, 240.0});

    auto back = reg->open(dir.path() / "fx.ld", {}, context(&units));
    REQUIRE(back.ok());
    const ColumnDump oilBack = dumpColumn(columnOf(channelNamed(*back->source, "Oil Temp")));
    REQUIRE(oilBack.values.size() == oilK.size());
    for (std::size_t i = 0; i < oilK.size(); ++i) {
        INFO(i);
        CHECK(oilBack.valid[i] == static_cast<bool>(oilValid[i]));
        if (oilValid[i]) {
            CHECK(floatUlps(oilBack.values[i], static_cast<double>(oilK[i])) <= 1);
        }
    }
    const ColumnDump speedBack = dumpColumn(columnOf(channelNamed(*back->source, "Speed")));
    CHECK(speedBack.values[99] == 24.75);
    CHECK(dumpColumn(columnOf(channelNamed(*back->source, "Small Count"))).values ==
          std::vector<double>{-3, 0, 7, 32767});
    CHECK(dumpColumn(columnOf(channelNamed(*back->source, "Big Count"))).values ==
          std::vector<double>{1, 1'000'000, -40'000});
    CHECK(dumpColumn(columnOf(channelNamed(*back->source, "Latitude"))).values[0] == -0.5912345678901234);
    const auto bcn =
        std::find_if(back->source->layers[0].markers.begin(), back->source->layers[0].markers.end(),
                     [](const model::Marker& m) { return m.cls == model::MarkerClass::BCN; });
    REQUIRE(bcn != back->source->layers[0].markers.end());
    CHECK(bcn->t == 4'000'000);
    CHECK(bcn->prov == model::Provenance::Companion);

    // A converted channel with an encoding needs the registry to go back to its unit.
    const auto noUnits = ImporterRegistry::withBuiltins();
    auto refused = noUnits->writer("motec.ld")->write(dir.path() / "nounits.ld", view, {}, {}, {});
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::Invalid);
    CHECK_FALSE(fs::exists(dir.path() / "nounits.ld"));
}

// Regression (PR #5 review E2, D-217): an export that writes no companion leaves no other companion beside
// the .ld (an earlier export to the same name, another file's), which the reader would merge into it.
TEST_CASE("ld export: a companion already beside the output goes when none is written", "[unit][ld_export]") {
    ld::test::TempDir dir("export-stale-ldx");
    const fs::path in = dir.path() / "in.ld";
    writeLd(in, mixedInput(), fixedIdentity());
    const auto reg = ImporterRegistry::withBuiltins(&shippedUnits());
    auto opened = reg->open(in, {}, context(&shippedUnits()));
    REQUIRE(opened.ok());
    const fs::path out = dir.path() / "out.ld";
    writeText(dir.path() / "out.ldx", richLdx()); // markers and details of another file
    ExportView view;
    view.source = opened->source;
    auto rep = reg->writer("motec.ld")->write(out, view, {}, {}, {});
    REQUIRE(rep.ok());
    REQUIRE(rep->files.size() == 1); // the .ld alone: nothing of this source needs a companion
    CHECK_FALSE(fs::exists(dir.path() / "out.ldx"));
    auto back = reg->open(out, {}, context(&shippedUnits()));
    REQUIRE(back.ok());
    for (const model::Marker& m : back->source->layers[0].markers) {
        CHECK(m.prov != model::Provenance::Companion);
    }
}

TEST_CASE("ld export: options, dialects and errors", "[unit][ld_export]") {
    ld::test::TempDir dir("export-options");
    const fs::path in = dir.path() / "in.ld";
    writeLd(in, mixedInput(), fixedIdentity());
    const auto reg = ImporterRegistry::withBuiltins(&shippedUnits());
    auto opened = reg->open(in, {}, context(&shippedUnits()));
    REQUIRE(opened.ok());
    IWriter* w = reg->writer("motec.ld");
    ExportView view;
    view.source = opened->source;

    // An explicit dialect writes a fresh header from the details: M1 identity, the details still there.
    WriterOptions m1;
    m1.options = model::OpaqueJson::parse(R"({"ld": {"dialect": "m1", "dataAlignment": 1}})").value();
    REQUIRE(w->write(dir.path() / "m1.ld", view, m1, {}, {}).ok());
    auto m1Probe = reg->probe(dir.path() / "m1.ld");
    REQUIRE(m1Probe.ok());
    CHECK(m1Probe->second.dialect.find("m1") != std::string::npos);
    CHECK(std::get<std::string>(model::findDetail(m1Probe->second.details, "Driver")->value) ==
          "Test Driver");
    CHECK(m1Probe->second.layers[0].channels.size() == 8);
    CHECK_FALSE(fs::exists(dir.path() / "m1.ldx")); // nothing for a companion

    // verbatim off: every channel is re-quantised from its base-unit column with its encoding, and the stored
    // values come back exactly (integers through the inverse conversion and scaling; NaN in float gaps).
    WriterOptions requantise;
    requantise.options = model::OpaqueJson::parse(R"({"ld": {"verbatim": false}})").value();
    REQUIRE(w->write(dir.path() / "requantised.ld", view, requantise, {}, {}).ok());
    const auto orig = parseLdFile(in);
    const auto requ = parseLdFile(dir.path() / "requantised.ld");
    REQUIRE(requ->channels().size() == orig->channels().size());
    for (std::uint32_t k = 0; k < orig->channels().size(); ++k) {
        const ld::LdChannel& a = orig->channels()[k];
        const ld::LdChannel& b = requ->channels()[k];
        INFO(a.loggedName);
        CHECK(b.enc.typeCode == a.enc.typeCode);
        CHECK(b.enc.counter == a.enc.counter);
        CHECK(b.unit == a.unit);
        CHECK(b.invalid == a.invalid);
        const auto ra = orig->rawValues(k, 0, a.sampleCount);
        const auto rb = requ->rawValues(k, 0, b.sampleCount);
        REQUIRE(ra.ok());
        REQUIRE(rb.ok());
        REQUIRE(ra->size() == rb->size());
        for (std::size_t i = 0; i < ra->size(); ++i) {
            if (orig->validAt(k, i)) {
                CHECK((*rb)[i] == (*ra)[i]);
            } else if (ld::isFloatSample(a.sampleType)) {
                CHECK(std::isnan((*rb)[i]));
            }
        }
    }

    // A subset of channels, details and markers left out.
    ExportView subset = view;
    subset.channels = {channelNamed(*opened->source, "Gear").desc.id,
                       channelNamed(*opened->source, "Lambda").desc.id};
    subset.includeDetails = false;
    subset.includeMarkers = false;
    REQUIRE(w->write(dir.path() / "subset.ld", subset, {}, {}, {}).ok());
    const auto sub = parseLdFile(dir.path() / "subset.ld");
    REQUIRE(sub->channels().size() == 2);
    CHECK(sub->channels()[1].loggedName == "Lambda");
    CHECK(sub->channels()[1].invalid.size() == 2);
    CHECK(sub->deviceEvents().empty());
    CHECK(sub->header().driver.empty());

    WriterOptions bad;
    bad.options = model::OpaqueJson::parse(R"({"ld": {"dialect": "cassette"}})").value();
    CHECK(w->write(dir.path() / "x.ld", view, bad, {}, {}).error().code == ErrorCode::Invalid);
    bad.options = model::OpaqueJson::parse(R"({"ld": {"dataAlignment": 0}})").value();
    CHECK(w->write(dir.path() / "x.ld", view, bad, {}, {}).error().code == ErrorCode::Invalid);
    ExportView partial = view;
    partial.start = 1'000'000;
    CHECK(w->write(dir.path() / "x.ld", partial, {}, {}, {}).error().code == ErrorCode::Unsupported);
    ExportView resampled = view;
    resampled.resampleRate = Rate{10, 1};
    CHECK(w->write(dir.path() / "x.ld", resampled, {}, {}, {}).error().code == ErrorCode::Unsupported);
    ExportView wrongLayer = view;
    wrongLayer.layer = "Burst0";
    CHECK(w->write(dir.path() / "x.ld", wrongLayer, {}, {}, {}).error().code == ErrorCode::NotFound);
    ExportView wrongChannel = view;
    wrongChannel.channels = {model::ChannelId{}};
    CHECK(w->write(dir.path() / "x.ld", wrongChannel, {}, {}, {}).error().code == ErrorCode::NotFound);
    CancelSource cancel;
    cancel.cancel();
    CHECK(w->write(dir.path() / "x.ld", view, {}, {}, cancel.token()).error().code == ErrorCode::Cancelled);
    CHECK(w->write(dir.path() / "x.ld", ExportView{}, {}, {}, {}).error().code == ErrorCode::Invalid);
    CHECK_FALSE(fs::exists(dir.path() / "x.ld"));
}

TEST_CASE("ld export: a header-only source keeps its header through the raw-header blob",
          "[unit][ld_export]") {
    ld::test::TempDir dir("export-header-only");
    ld::LdWriteInput in;
    in.details = richDetails();
    writeLd(dir.path() / "empty.ld", in, fixedIdentity());
    const auto reg = ImporterRegistry::withBuiltins();
    auto opened = reg->open(dir.path() / "empty.ld", {}, context());
    REQUIRE(opened.ok());
    ExportView view;
    view.source = opened->source;
    REQUIRE(reg->writer("motec.ld")->write(dir.path() / "out.ld", view, {}, {}, {}).ok());
    const auto a = parseLdFile(dir.path() / "empty.ld");
    const auto b = parseLdFile(dir.path() / "out.ld");
    CHECK(b->headerOnly());
    CHECK(std::equal(a->rawWeatherRecord().begin(), a->rawWeatherRecord().end(),
                     b->rawWeatherRecord().begin(), b->rawWeatherRecord().end()));
    CHECK(b->header().deviceType == "TESTLOG");
    CHECK(b->header().team == a->header().team);
}
