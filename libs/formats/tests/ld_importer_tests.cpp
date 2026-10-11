// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The motec.ld importer through the public API (05 sections 2-4): the probe of a synthetic file, the
// DataSource the open pipeline builds (channels, ids, descriptors with sourceEncoding and display decimals,
// enumerations, validity, device events, details, ranges), unit resolution with conversion to base units at
// decode (D-010), the .ldx merge with precedence sidecar > companion > file (D-015), readBlock against the
// .ld chunk provider, options and the header-only file.
#include "formats_test_support.hpp"
#include "importer_chunk_provider.hpp"
#include "synthetic_families.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::test;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = std::numbers::pi;

struct Fixture {
    ld::test::TempDir dir{"importer"};
    fs::path path;
    std::unique_ptr<ImporterRegistry> registry = ImporterRegistry::withBuiltins(&shippedUnits());

    explicit Fixture(const ld::LdWriteInput& in = mixedInput(),
                     std::optional<std::string> ldx = std::nullopt) {
        path = dir.path() / "session.ld";
        writeLd(path, in, fixedIdentity());
        if (ldx) {
            writeText(dir.path() / "session.ldx", *ldx);
        }
    }
    OpenedSource open(const units::UnitRegistry* units, ImportOptions options = {}) const {
        auto r = registry->open(path, std::move(options), context(units));
        REQUIRE(r.ok());
        return std::move(r).value();
    }
};

double rpmToRadS(double rpm) {
    return rpm * 2.0 * kPi / 60.0;
}

} // namespace

TEST_CASE("ld importer: the probe of a synthetic file", "[unit][ld_importer]") {
    Fixture fx;
    auto probed = fx.registry->probe(fx.path);
    REQUIRE(probed.ok());
    const ProbeResult& p = probed->second;
    CHECK(p.importerId == "motec.ld");
    CHECK(p.dialect == "pc-layout");
    CHECK(p.companions.empty());
    REQUIRE(p.layers.size() == 1);
    const LayerInfo& l = p.layers[0];
    CHECK(l.name == "Normal");
    CHECK(l.durationUs == 60'000'000); // the long-name channel: 60 samples at 1 Hz
    REQUIRE(l.utc.has_value());
    CHECK(l.utc->unixUs == 1'791'623'730'000'000); // 2026-10-10 09:15:30 read as UTC
    CHECK(l.utcAssumedLocal);
    REQUIRE(l.channels.size() == 8);
    const ChannelInfo& rpm = l.channels[0];
    CHECK(rpm.loggedName == "Engine Speed");
    CHECK(rpm.vendorUnit == "rpm");
    CHECK(rpm.vendorDtype == "i16");
    CHECK(rpm.dtype == store::Dtype::I32);
    CHECK(rpm.kind == store::SampleKind::Integer);
    CHECK(rpm.vendorId == std::optional<std::string>("20101"));
    CHECK(std::get<Rate>(rpm.timing) == Rate{50, 1});
    CHECK(rpm.sampleCount == 500);
    const ChannelInfo& oil = l.channels[1];
    CHECK(oil.dtype == store::Dtype::F32);
    CHECK(oil.dps == 1);
    const ChannelInfo& state = l.channels[4];
    CHECK(state.kind == store::SampleKind::Enum);
    CHECK(state.enumTable == std::optional<std::uint32_t>(3));
    CHECK(l.channels[5].loggedName == "A channel name far longer than thirty-two bytes");
    CHECK(l.channels[6].dtype == store::Dtype::F64); // dec 7 >= 5
    // Device events: the model's labels, ordinals per (class, group, tUs), stored order kept at equal times.
    REQUIRE(l.markers.size() == 3);
    CHECK(l.markers[0].cls == model::MarkerClass::DeviceEvent);
    CHECK(l.markers[0].name == "Logging on");
    CHECK(l.markers[0].group == "DeviceEvents");
    CHECK(l.markers[0].flags == 3);
    CHECK(l.markers[0].ordinal == 0);
    CHECK(l.markers[1].name == "Event 17");
    CHECK(l.markers[1].ordinal == 1);
    CHECK(l.markers[2].tUs == 25'000'000);
    REQUIRE(p.enumTables.size() == 1);
    CHECK(p.enumTables[0].id == 3);
    CHECK(p.enumTables[0].entries[4] == model::EnumEntry{3, "Overheat", 3});
    // Details: header and records, numbers in base units with their symbols (05 section 3.3).
    const auto text = [&](std::string_view id) {
        const model::Detail* d = detailOf(p.details, id);
        REQUIRE(d != nullptr);
        return std::get<std::string>(d->value);
    };
    const auto number = [&](std::string_view id) {
        const model::Detail* d = detailOf(p.details, id);
        REQUIRE(d != nullptr);
        return std::get<model::Quantityd>(d->value);
    };
    CHECK(text("Device Type") == "TESTLOG");
    CHECK(text("Device Serial") == "12345");
    CHECK(text("Device Version") == "7");
    CHECK(text("Driver") == "Test Driver");
    CHECK(text("Engine Id") == "E-02"); // the vehicle record's copy wins
    CHECK(text("Session") == "Practice 1");
    CHECK(text("Event") == "Club Day");
    CHECK(number("Venue Length").v == 6213.0);
    CHECK(number("Venue Length").symbol == "m");
    CHECK(number("Venue Length").dps == 0);
    CHECK(number("Vehicle Weight").v == 1250.0);
    CHECK(number("Fuel Tank Capacity").v == 0.065);
    CHECK(number("Fuel Tank Capacity").symbol == "m3");
    CHECK(number("Diff Ratio").v == 3.545);
    CHECK(number("Gear 6").v == 1.1);
    CHECK(detailOf(p.details, "Gear 7") == nullptr); // zero: no detail
    CHECK(number("Vehicle Track").v == 1.6);
    CHECK(number("Air Temp").v == 23.5);
    CHECK(number("Air Temp").symbol == "C");
    CHECK(number("Air Temp").dps == 1);
    CHECK(number("Baro Pressure").symbol == "mbar");
    CHECK(text("Wind Direction") == "NE");
    // Heuristics logged (rule 9): string encoding, shift and its order, dps from the record.
    CHECK(logHas(p.warnings, "ld.string_encoding"));
    CHECK(logEntry(p.warnings, "ld.shift_mul_order")->ctx.detail == std::optional<std::string>("afterMul"));
}

TEST_CASE("ld importer: the DataSource without a unit registry keeps vendor values", "[unit][ld_importer]") {
    Fixture fx;
    const OpenedSource o = fx.open(nullptr);
    const model::DataSource& src = *o.source;
    CHECK(src.prov.dialectFlags == std::vector<std::string>{"pc-layout"});
    CHECK(src.prov.rawHeader.size() > 0x6E2);
    CHECK(logHas(o.log, open_code::kUnitsSkipped));
    REQUIRE(src.layers.size() == 1);
    const model::DataLayer& layer = src.layers[0];
    REQUIRE(layer.channels.size() == 8);
    CHECK(layer.duration == 60'000'000);
    REQUIRE(layer.origin.has_value());

    const model::Channel& rpm = channelNamed(src, "Engine Speed");
    CHECK(rpm.desc.id == model::channelIdLogged(src.id, "Normal", "Engine Speed", "motec.ld"));
    CHECK(rpm.desc.loggedName == "Engine Speed");
    CHECK(rpm.desc.source == model::ChannelSource::Logged);
    CHECK(rpm.desc.unit == units::kNoUnit);
    CHECK(rpm.desc.dtype == store::Dtype::I32);
    CHECK(rpm.desc.kind == store::SampleKind::Integer);
    CHECK(rpm.desc.interp == store::Interp::Linear); // the record's interpolation byte
    CHECK(rpm.desc.rate == std::optional<Rate>(Rate{50, 1}));
    const auto* enc = dynamic_cast<const LdSourceEncoding*>(rpm.desc.sourceEncoding.get());
    REQUIRE(enc != nullptr);
    CHECK(enc->importerId() == "motec.ld");
    CHECK(enc->origin() != nullptr);
    CHECK(enc->encoding().typeCode == 0x03);
    CHECK(enc->encoding().width == 2);
    CHECK(enc->encoding().unit == "rpm");
    CHECK(enc->encoding().counter == std::optional<std::uint16_t>(std::uint16_t{20101}));
    CHECK(enc->encoding().rawRecord.has_value());
    const ColumnDump d = dumpColumn(columnOf(rpm));
    REQUIRE(d.values.size() == 500);
    CHECK(d.values[0] == 800.0);
    CHECK(d.values[499] == 5790.0);

    const model::Channel& oil = channelNamed(src, "Gbox Oil Temp");
    CHECK(oil.desc.dps == 1);
    const ColumnDump od = dumpColumn(columnOf(oil));
    CHECK(od.values[0] == static_cast<double>(static_cast<float>(238.0 / 5.0 * 0.1 * 9.0 + 32.0)));

    const model::Channel& gear = channelNamed(src, "Gear");
    CHECK(gear.desc.interp == store::Interp::Previous);
    CHECK(dumpColumn(columnOf(gear)).values[249] == 6.0);

    const model::Channel& state = channelNamed(src, "Engine State");
    CHECK(state.desc.kind == store::SampleKind::Enum);
    REQUIRE(state.desc.enums.has_value());
    const model::EnumTable* table = layer.enumTable(*state.desc.enums);
    REQUIRE(table != nullptr);
    CHECK(table->find(1)->label == "Cranking");
    CHECK(table->find(1)->severity == 1);

    // Gap blocks are validity: invalid samples are NaN in float channels and cleared bits everywhere.
    const ColumnDump lambda = dumpColumn(columnOf(channelNamed(src, "Lambda")));
    for (std::size_t i = 0; i < lambda.values.size(); ++i) {
        const bool inGap = (i >= 10 && i < 20) || (i >= 90 && i < 100);
        INFO(i);
        CHECK(lambda.valid[i] == !inGap);
        CHECK(std::isnan(lambda.values[i]) == inGap);
    }
    CHECK(lambda.values[0] == 0.75);

    // Markers: device events (Logged) and the synthesised SOL/EOL (Derived); the whole-outing range.
    std::size_t events = 0, derived = 0;
    for (const model::Marker& m : layer.markers) {
        if (m.cls == model::MarkerClass::DeviceEvent) {
            ++events;
            CHECK(m.prov == model::Provenance::Logged);
            CHECK(m.group == "DeviceEvents");
        } else {
            ++derived;
            CHECK(m.prov == model::Provenance::Derived);
        }
    }
    CHECK(events == 3);
    CHECK(derived == 2);
    const auto* outings = layer.ranges.find("Outings");
    REQUIRE(outings != nullptr);
    REQUIRE(outings->ranges.size() == 1);
    CHECK(outings->ranges[0].start == 0);
    CHECK(outings->ranges[0].end == 60'000'000);
    CHECK(outings->ranges[0].name == "session.ld");

    // Details: the file's (Logged) plus the derived File group.
    CHECK(model::findDetail(src.details, "Driver")->prov == model::Provenance::Logged);
    CHECK(std::get<std::string>(model::findDetail(src.details, "File Name")->value) == "session.ld");
}

TEST_CASE("ld importer: units resolve and samples are converted to base units at decode",
          "[unit][ld_importer]") {
    Fixture fx;
    const units::UnitRegistry& reg = shippedUnits();
    const OpenedSource o = fx.open(&reg);
    const model::DataSource& src = *o.source;
    CHECK_FALSE(logHas(o.log, open_code::kUnitsSkipped));

    const model::Channel& rpm = channelNamed(src, "Engine Speed");
    CHECK(reg.quantity(rpm.desc.quantity).name == "Angular Speed");
    CHECK(reg.unit(rpm.desc.unit).symbol == "rad/s");
    CHECK(rpm.desc.dtype == store::Dtype::F32); // a 16-bit integer converted: F32
    CHECK(rpm.desc.kind == store::SampleKind::Real);
    const ColumnDump d = dumpColumn(columnOf(rpm));
    CHECK(d.values[0] == static_cast<double>(static_cast<float>(rpmToRadS(800.0))));
    CHECK(d.values[499] == static_cast<double>(static_cast<float>(rpmToRadS(5790.0))));

    // Shift/mul channel in deg F to kelvin: one rounding of the exact composition.
    const model::Channel& oil = channelNamed(src, "Gbox Oil Temp");
    CHECK(reg.unit(oil.desc.unit).symbol == "K");
    const double f = 238.0 / 5.0 * 0.1 * 9.0 + 32.0;
    const double k = (f - 32.0) * 5.0 / 9.0 + 273.15;
    CHECK(std::abs(dumpColumn(columnOf(oil)).values[0] - k) <= 1e-4);

    // The 32-bit fixed-point latitude in degrees becomes radians in F64.
    const model::Channel& lat = channelNamed(src, "GPS Latitude");
    CHECK(lat.desc.dtype == store::Dtype::F64);
    CHECK(reg.unit(lat.desc.unit).symbol == "rad");
    CHECK(std::abs(dumpColumn(columnOf(lat)).values[0] - (-33.8765432 * kPi / 180.0)) <= 1e-12);

    // Unitless and identity channels keep their dtype; an enumeration is never converted.
    const model::Channel& gear = channelNamed(src, "Gear");
    CHECK(gear.desc.unit == units::kNoUnit);
    CHECK(gear.desc.dtype == store::Dtype::I32);
    const model::Channel& volts = channelNamed(src, "A channel name far longer than thirty-two bytes");
    CHECK(reg.unit(volts.desc.unit).symbol == "V");
    CHECK(volts.desc.dtype == store::Dtype::F32);
    CHECK(dumpColumn(columnOf(volts)).values[3] == 1.5);
    const model::Channel& lambda = channelNamed(src, "Lambda");
    CHECK(reg.unit(lambda.desc.unit).symbol == "lambda");

    // An unknown unit: unitless, the status text of 05 section 2 step 4, one aggregated warning.
    const model::Channel& bias = channelNamed(src, "Brake Bias");
    CHECK(bias.desc.unit == units::kNoUnit);
    CHECK(bias.desc.statusText == "Unit unknown: \"furlongs\"");
    CHECK(bias.status == model::ChannelStatus::Ok);
    const ImportLogEntry* e = logEntry(o.log, open_code::kUnitUnknown);
    REQUIRE(e != nullptr);
    CHECK(e->level == ImportLogEntry::Level::Warning);
    CHECK(e->ctx.detail == std::optional<std::string>("furlongs"));
    CHECK(e->ctx.channel == std::optional<std::string>("Brake Bias"));

    // strictUnits refuses the open instead.
    ImportOptions strict;
    strict.strictUnits = true;
    auto r = fx.registry->open(fx.path, strict, context(&reg));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::NotFound);
    CHECK(r.error().message.find("Brake Bias") != std::string::npos);

    // Weather details resolve against the registry (detail units, 02 section 4.2).
    const model::Detail* air = model::findDetail(src.details, "Air Temp");
    REQUIRE(air != nullptr);
    CHECK(reg.unit(std::get<model::Quantityd>(air->value).unit).symbol == "deg C");

    // A numeric detail the sidecar adds is resolved too (after the sidecar joined the details).
    model::SidecarDocument doc = model::newSidecarFor(src);
    model::Quantityd weight;
    weight.v = 1300.0;
    weight.symbol = "kg";
    doc.setDetail("Normal", model::Detail{"Vehicle Weight", weight, model::Provenance::User, std::nullopt});
    REQUIRE(doc.save(model::SidecarDocument::pathFor(fx.path)).ok());
    const OpenedSource edited = fx.open(&reg);
    const model::Detail* w = model::findDetail(edited.source->details, "Vehicle Weight");
    REQUIRE(w != nullptr);
    CHECK(w->prov == model::Provenance::User);
    CHECK(reg.unit(std::get<model::Quantityd>(w->value).unit).symbol == "kg");
}

TEST_CASE("ld importer: shiftOrder beforeMul is an option", "[unit][ld_importer]") {
    Fixture fx;
    ImportOptions options;
    options.options = model::OpaqueJson::parse(R"({"ld": {"shiftOrder": "beforeMul"}})").value();
    const OpenedSource o = fx.open(nullptr, options);
    const double before = (238.0 / 5.0 * 0.1 + 32.0) * 9.0;
    CHECK(dumpColumn(columnOf(channelNamed(*o.source, "Gbox Oil Temp"))).values[0] ==
          static_cast<double>(static_cast<float>(before)));
    CHECK(logEntry(o.log, "ld.shift_mul_order")->ctx.detail == std::optional<std::string>("beforeMul"));
}

TEST_CASE("ld importer: readBlock agrees with the chunk provider", "[unit][ld_importer]") {
    Fixture fx;
    auto source = openByteSource(fx.path);
    REQUIRE(source.ok());
    const IImporter* imp = fx.registry->find("motec.ld");
    auto opened = imp->open(*source, {});
    REQUIRE(opened.ok());
    std::shared_ptr<IReader> reader(std::move(opened).value());
    const units::UnitRegistry& reg = shippedUnits();
    // Convert Engine Speed like the pipeline does, then compare both providers chunk by chunk.
    REQUIRE(
        reader->setConversion(0, 0, ChannelConversion{reg.toBase(*reg.find("rpm")), store::Dtype::F32}).ok());
    CHECK(reader->setConversion(0, 0, ChannelConversion{reg.toBase(*reg.find("rpm")), store::Dtype::I32})
              .error()
              .code == ErrorCode::Invalid);
    store::ColumnOptions co;
    co.cache = &testCache();
    for (ChannelIx ch = 0; ch < reader->probe().layers[0].channels.size(); ++ch) {
        INFO(ch);
        auto native = reader->makeChunkProvider(0, ch);
        REQUIRE(native != nullptr);
        auto generic =
            std::make_shared<ImporterChunkProvider>(reader, 0, ch, native->dtype(), reader->decodeLog());
        const ColumnDump a = dumpColumn(store::Column(native, co));
        const ColumnDump b = dumpColumn(store::Column(generic, co));
        REQUIRE(a.values.size() == b.values.size());
        CHECK(a.valid == b.valid);
        for (std::size_t i = 0; i < a.values.size(); ++i) {
            CHECK(((std::isnan(a.values[i]) && std::isnan(b.values[i])) || a.values[i] == b.values[i]));
        }
    }
    // Block contract: dtype must match, integer gaps need validBits, ranges are checked.
    std::vector<std::byte> buf(4 * 8);
    Block block;
    block.dtype = store::Dtype::F64;
    block.values = buf;
    CHECK(reader->readBlock(0, 2, 0, 8, block, {}).error().code == ErrorCode::Invalid);
    block.dtype = store::Dtype::I32;
    CHECK(reader->readBlock(0, 2, 0, 8, block, {}).ok());
    CHECK(reader->readBlock(0, 2, 249, 8, block, {}).error().code == ErrorCode::Invalid);
    CHECK(reader->readBlock(0, 99, 0, 1, block, {}).error().code == ErrorCode::NotFound);
    CHECK(reader->readBlock(1, 0, 0, 1, block, {}).error().code == ErrorCode::NotFound);
    CHECK(reader->readTimestamps(0, 0, 0, 1, {}, {}).error().code == ErrorCode::Unsupported);
    std::uint8_t bits = 0;
    block.dtype = store::Dtype::F32;
    block.validBits = std::span<std::uint8_t>(&bits, 1);
    REQUIRE(reader->readBlock(0, 3, 8, 8, block, {}).ok()); // Lambda samples 8..15, gap from 10
    CHECK(bits == 0x03);
    CHECK(reader->enumTable(3).has_value());
    CHECK_FALSE(reader->enumTable(4).has_value());
    CHECK(reader->chunkIndependent());
}

TEST_CASE("ld importer: readBlock validity applies the gap intervals and the stored-NaN rule",
          "[unit][ld_importer]") {
    ld::LdWriteInput in;
    std::vector<double> v(32, 1.5);
    v[3] = std::numeric_limits<double>::quiet_NaN(); // a NaN stored in a float channel is invalid (S-1 D13)
    auto c = ld::test::floatChannel("Lambda", ld::layout::kTypeFloat, 4, 10, v);
    c.invalid = {{10, 20}};
    in.channels.push_back(c);
    const Fixture fx(in);
    auto source = openByteSource(fx.path);
    REQUIRE(source.ok());
    auto opened = fx.registry->find("motec.ld")->open(*source, {});
    REQUIRE(opened.ok());
    const std::shared_ptr<IReader> reader(std::move(opened).value());
    std::vector<std::byte> values(32 * 4);
    std::array<std::uint8_t, 4> bits{};
    Block block;
    block.dtype = store::Dtype::F32;
    block.values = values;
    block.validBits = bits;
    REQUIRE(reader->readBlock(0, 0, 0, 32, block, {}).ok());
    CHECK(bits == std::array<std::uint8_t, 4>{0xF7, 0x03, 0xF0, 0xFF});
    // A window that starts inside the gap: bits are relative to `first`.
    bits.fill(0);
    REQUIRE(reader->readBlock(0, 0, 15, 8, block, {}).ok());
    CHECK(bits[0] == 0xE0);
}

TEST_CASE("ld importer: the .ldx companion merges with precedence sidecar > companion > file",
          "[unit][ld_importer]") {
    Fixture fx(mixedInput(), richLdx());
    const OpenedSource o = fx.open(nullptr);
    const model::DataSource& src = *o.source;
    REQUIRE(src.prov.companions.size() == 1);
    CHECK(src.prov.companions[0].filename() == fs::path("session.ldx"));

    // The probe reports what both files say (companion markers and details, the .ldx codes).
    CHECK(o.probe.companions == std::vector<fs::path>{fs::path("session.ldx")});
    std::size_t probeBcn = 0;
    for (const MarkerInfo& m : o.probe.layers[0].markers) {
        probeBcn += m.cls == model::MarkerClass::BCN ? 1 : 0;
    }
    CHECK(probeBcn == 2);
    CHECK(detailOf(o.probe.details, "Total Laps") != nullptr);
    CHECK(logHas(o.log, "ldx.unknown_marker_class"));
    CHECK(logHas(o.log, "ldx.unknown_element"));
    CHECK(logHas(o.log, "ldx.unknown_attribute"));
    CHECK(logHas(o.log, "ldx.marker_unsorted"));

    // Companion markers carry prov Companion and the ids of 02 section 4.3 rule 5.
    const model::DataLayer& layer = src.layers[0];
    const model::Marker* bcn =
        layer.findMarker(model::markerIdLogged(model::MarkerClass::BCN, 15'000'000, "Beacons", 0));
    REQUIRE(bcn != nullptr);
    CHECK(bcn->prov == model::Provenance::Companion);
    CHECK(bcn->name == "Manual.2");
    CHECK(bcn->flags == 77);
    const model::Marker* split =
        layer.findMarker(model::markerIdLogged(model::MarkerClass::SPLTBCN, 9'000'000, "Beacons", 0));
    REQUIRE(split != nullptr);
    CHECK(split->flags == 5);
    // The companion has no SOL/EOL either, so they are synthesised.
    CHECK(std::count_if(layer.markers.begin(), layer.markers.end(),
                        [](const model::Marker& m) { return m.prov == model::Provenance::Derived; }) == 2);

    // Details: companion over file, with the file value kept as `original`.
    const model::Detail* driver = model::findDetail(src.details, "Driver");
    REQUIRE(driver != nullptr);
    CHECK(std::get<std::string>(driver->value) == "Companion Driver");
    CHECK(driver->prov == model::Provenance::Companion);
    REQUIRE(driver->original.has_value());
    CHECK(std::get<std::string>(*driver->original) == "Test Driver");
    CHECK(model::findDetail(src.details, "Total Laps")->prov == model::Provenance::Companion);

    // Unknown elements are kept verbatim in companionOpaque.
    for (const std::string& xml : richLdxUnknownElements()) {
        INFO(xml);
        CHECK(std::any_of(src.companionOpaque.begin(), src.companionOpaque.end(), [&](const std::string& s) {
            return s.size() >= xml.size() && s.compare(s.size() - xml.size(), xml.size(), xml) == 0;
        }));
    }

    // A sidecar wins over both: a user detail and an ignore edit on a companion beacon.
    model::SidecarDocument doc = model::newSidecarFor(src);
    doc.setDetail("Normal", model::Detail{"Driver", std::string("Sidecar Driver"), model::Provenance::User,
                                          std::nullopt});
    REQUIRE(doc.applyEdit("Normal", model::MarkerEdit{bcn->id, model::MarkerEditOp::Ignore}).ok());
    REQUIRE(doc.save(model::SidecarDocument::pathFor(fx.path)).ok());
    const OpenedSource again = fx.open(nullptr);
    const model::Detail* edited = model::findDetail(again.source->details, "Driver");
    REQUIRE(edited != nullptr);
    CHECK(std::get<std::string>(edited->value) == "Sidecar Driver");
    CHECK(edited->prov == model::Provenance::User);
    const model::Marker* ignored = again.source->layers[0].findMarker(bcn->id);
    REQUIRE(ignored != nullptr);
    CHECK_FALSE(ignored->enabled);
    CHECK_FALSE(logHas(again.log, "sidecar.source_changed"));

    // Without the companion merge the .ldx is still in the probe, not in the source.
    OpenContext noCompanion = context();
    noCompanion.mergeCompanion = false;
    noCompanion.loadSidecar = false;
    auto plain = fx.registry->open(fx.path, {}, noCompanion);
    REQUIRE(plain.ok());
    CHECK(model::findDetail(plain->source->details, "Total Laps") == nullptr);
    CHECK(plain->source->companionOpaque.empty());

    // A sidecar that does not parse: the source opens without its edits and says so.
    writeText(model::SidecarDocument::pathFor(fx.path), "{ not json");
    const OpenedSource broken = fx.open(nullptr);
    CHECK(logHas(broken.log, open_code::kSidecarLoadFailed));
    CHECK(std::get<std::string>(model::findDetail(broken.source->details, "Driver")->value) ==
          "Companion Driver");

    // expectedHash: a different SourceId is reported, the open goes on.
    ImportOptions expect;
    expect.expectedHash = Hash128{1, 2};
    fs::remove(model::SidecarDocument::pathFor(fx.path));
    CHECK(logHas(fx.open(nullptr, expect).log, "sidecar.source_changed"));
}

TEST_CASE("ld importer: a malformed .ldx is logged and the .ld opens", "[unit][ld_importer]") {
    Fixture fx(mixedInput(), std::string("<LDXFile><Layers></LDXFile>"));
    const OpenedSource o = fx.open(nullptr);
    CHECK(logHas(o.log, "ldx.parse_error"));
    CHECK(o.source->layers[0].channels.size() == 8);
    CHECK(o.source->companionOpaque.empty());
}

TEST_CASE("ld importer: a header-only file is a valid empty source", "[unit][ld_importer]") {
    ld::LdWriteInput in;
    in.details.date = "10/10/2026";
    in.details.time = "09:15:00";
    Fixture fx(in);
    const OpenedSource o = fx.open(&shippedUnits());
    CHECK(logHas(o.log, "ld.header_only"));
    REQUIRE(o.source->layers.size() == 1);
    CHECK(o.source->layers[0].channels.empty());
    CHECK(o.source->layers[0].duration == 0);
    CHECK(o.source->layers[0].markers.empty());
    CHECK(o.probe.layers[0].channels.empty());
    CHECK(schemaErrors("probe-1", o.probe.canonicalJson().value()).empty());
}
