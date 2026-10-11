// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Synthetic families for the independent oracle (D-041, AC-M0-03). This hidden test case writes each family
// with LdWriter into $ESSES_LD_ORACLE_OUT, together with <family>.expect.json holding (a) the generator's own
// expectations (channel list, encodings, stored first/last/min/max values, computed from the generator input,
// never from the reader) and (b) what the reader decodes (scaled first/last/min/max/mean). The ctest
// formats_ld.oracle_synthetic (ld_oracle_synthetic.py) runs tools/ld_ref_decode.py on every file and checks
// that the oracle agrees with (a) - so the writer is verified independently - and with (b) - so the reader
// is verified against the oracle, not against the writer.
#include "ld_test_support.hpp"

#include <esses/base/utf8.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::ld;
using namespace esses::formats::ld::test;
namespace L = esses::formats::ld::layout;
using json = nlohmann::json;

namespace {

struct Family {
    std::string name;
    LdWriteInput input;
    LdWriteOptions options;
};

std::vector<double> storedValues(const LdWriteChannel& c) {
    std::vector<double> v;
    if (!c.rawFloats.empty()) {
        v = c.rawFloats;
    } else {
        for (const std::int64_t x : c.rawInts) {
            v.push_back(static_cast<double>(x));
        }
    }
    return v;
}

json head(const std::vector<double>& v) {
    json a = json::array();
    for (std::size_t i = 0; i < std::min<std::size_t>(5, v.size()); ++i) {
        a.push_back(v[i]);
    }
    return a;
}
json tail(const std::vector<double>& v) {
    json a = json::array();
    for (std::size_t i = v.size() >= 5 ? v.size() - 5 : 0; i < v.size(); ++i) {
        a.push_back(v[i]);
    }
    return a;
}

std::vector<Family> families() {
    std::vector<Family> out;
    {
        Family f{"pairs", {}, {}};
        const std::vector<std::pair<int, int>> ints = {{0, 2}, {3, 2}, {5, 2}, {0, 4}, {3, 4}, {5, 4},
                                                       {6, 4}, {3, 1}, {6, 2}, {4, 1}, {4, 2}, {4, 4},
                                                       {8, 2}, {8, 4}, {9, 2}, {5, 8}};
        for (const auto& [code, width] : ints) {
            // Both readings of 0x04 (unsigned here, signed in the oracle) agree below 2^(8w-1).
            const std::int64_t span =
                width == 8 ? (std::int64_t{1} << 40) : (std::int64_t{1} << (8 * width - 2));
            std::vector<std::int64_t> v;
            for (std::int64_t i = 0; i < 500; ++i) {
                const std::int64_t x = (i * 7919 + 13) % span;
                v.push_back(code == L::kTypeUnsigned4 ? x : x - span / 2);
            }
            f.input.channels.push_back(intChannel("int " + std::to_string(code) + "/" + std::to_string(width),
                                                  static_cast<std::uint16_t>(code),
                                                  static_cast<std::uint16_t>(width), 10, std::move(v)));
        }
        const std::vector<std::pair<int, int>> floats = {{7, 2}, {7, 4}, {8, 8}, {7, 8}};
        for (const auto& [code, width] : floats) {
            std::vector<double> v;
            for (int i = 0; i < 500; ++i) {
                v.push_back((i - 250) * 0.125); // exact in every float width
            }
            f.input.channels.push_back(floatChannel(
                "float " + std::to_string(code) + "/" + std::to_string(width),
                static_cast<std::uint16_t>(code), static_cast<std::uint16_t>(width), 20, std::move(v)));
        }
        out.push_back(std::move(f));
    }
    {
        Family f{"multirate", {}, {}};
        f.options.dataAlignment = 1;
        for (const int hz : {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 5000}) {
            f.input.channels.push_back(intChannel("rate " + std::to_string(hz), L::kTypeInt5, 4,
                                                  static_cast<std::uint16_t>(hz), ramp(hz * 60 + 7, -hz, 3)));
        }
        out.push_back(std::move(f));
    }
    {
        Family f{"scaling", {}, {}};
        auto temp = intChannel("Gbox Oil Temp", L::kTypeInt3, 2, 2, ramp(400, 238, 1));
        temp.shift = 32;
        temp.mul = 9;
        temp.scale = 5;
        temp.dec = 1;
        f.input.channels.push_back(temp);
        auto m1 = intChannel("M1 fixed point", L::kTypeInt5, 4, 100, ramp(1000, -123456789, 98765));
        m1.scale = 10000;
        m1.dec = 4;
        f.input.channels.push_back(m1);
        auto gps = intChannel("GPS Latitude", L::kTypeInt5, 4, 10, ramp(300, -338765432, 1234567));
        gps.dec = 7;
        f.input.channels.push_back(gps);
        auto zs = intChannel("zero scale", L::kTypeInt3, 2, 10, ramp(100, -50, 1));
        zs.scale = 0;
        zs.mul = 3;
        f.input.channels.push_back(zs);
        auto zm = intChannel("zero mul", L::kTypeInt3, 2, 10, ramp(100, -50, 1));
        zm.mul = 0;
        zm.dec = 2;
        f.input.channels.push_back(zm);
        auto nd = intChannel("negative dec", L::kTypeInt3, 2, 10, ramp(100, -50, 1));
        nd.dec = -2;
        f.input.channels.push_back(nd);
        auto neg = intChannel("negative mul", L::kTypeInt5, 4, 10, ramp(100, -50, 3));
        neg.mul = -4;
        neg.scale = 3;
        f.input.channels.push_back(neg);
        auto fl = floatChannel("scaled float", L::kTypeFloat, 4, 10, std::vector<double>(50, 1.5));
        fl.mul = 2;
        fl.scale = 4;
        f.input.channels.push_back(fl);
        out.push_back(std::move(f));
    }
    {
        Family f{"aux-enum-gaps-events", {}, {}};
        LdEnumTable t;
        t.index = 9;
        t.entries = {{0, "Off", false, 0}, {1, "On", true, 1}, {2, "Fault", true, 3}};
        f.input.enumTables = {t};
        auto e = intChannel("State", L::kTypeInt3, 2, 10, {0, 1, 2, 1, 0, 0, 2});
        e.enumRef = std::uint16_t{9};
        f.input.channels.push_back(e);
        auto ln = intChannel("A channel name that is much longer than thirty-two bytes", L::kTypeInt5, 4, 50,
                             ramp(2000, 100, 2));
        ln.invalid = {{0, 51}, {1500, 1600}};
        ln.enumRef = std::uint16_t{9};
        f.input.channels.push_back(ln);
        f.input.channels.push_back(
            intChannel("0123456789abcdef0123456789abcdef", L::kTypeInt3, 2, 1, ramp(30)));
        f.input.events = {{0, 0.0}, {3, 1500.25}, {52, 2'000'000.0}};
        f.input.details.venue = "Synthetic Raceway";
        f.input.details.driver = "Test Driver";
        out.push_back(std::move(f));
    }
    for (const WriteDialect d : {WriteDialect::M1, WriteDialect::CSeries, WriteDialect::SimExporter,
                                 WriteDialect::ConverterLegacy}) {
        Family f{"dialect-" + std::string(writeDialectName(d)), {}, {}};
        f.options.dialect = d;
        auto a = intChannel("Device Up Time", L::kTypeInt5, 4, 10, ramp(600, 1000, 100));
        a.shortName = "Up Time";
        a.unit = "s";
        a.dec = 3;
        f.input.channels.push_back(a);
        auto b = floatChannel("Lambda", L::kTypeFloat, 4, 20, std::vector<double>(1200, 0.875));
        b.unit = "LA";
        f.input.channels.push_back(b);
        out.push_back(std::move(f));
    }
    return out;
}

} // namespace

TEST_CASE("ld oracle fixtures: write the synthetic families for tools/ld_ref_decode.py",
          "[.oracle_fixtures]") {
    const auto dir = getEnv(envName("LD_ORACLE_OUT"));
    if (!dir || dir->empty()) {
        SKIP("set ESSES_LD_ORACLE_OUT to the output directory");
    }
    const std::filesystem::path outDir = pathFromUtf8(*dir);
    std::filesystem::create_directories(outDir);
    for (const Family& fam : families()) {
        const auto path = outDir / (fam.name + ".ld");
        REQUIRE(LdWriter::write(path, fam.input, fam.options).ok());
        auto file = LdFile::open(path);
        REQUIRE(file.ok());
        const LdFile& f = **file;
        REQUIRE(f.channels().size() == fam.input.channels.size());
        json channels = json::array();
        for (std::uint32_t k = 0; k < f.channels().size(); ++k) {
            const LdWriteChannel& w = fam.input.channels[k];
            const std::vector<double> stored = storedValues(w);
            json c;
            // (a) the generator's expectations
            c["name"] = w.name.substr(0, L::kNameWidth); // ASCII names: the 32-byte field holds the start
            c["dtype_a"] = w.typeCode;
            c["dtype"] = w.width;
            c["freq"] = w.rateHz;
            c["count"] = stored.size();
            c["shift"] = w.shift;
            c["mul"] = w.mul;
            c["scale"] = w.scale;
            c["dec"] = w.dec;
            c["raw_first"] = head(stored);
            c["raw_last"] = tail(stored);
            c["raw_min"] = *std::min_element(stored.begin(), stored.end());
            c["raw_max"] = *std::max_element(stored.begin(), stored.end());
            // (b) what the reader decodes
            const std::vector<double> v = decodeAll(f, k);
            double sum = 0.0, comp = 0.0;
            for (const double x : v) {
                const double t = sum + x;
                comp += std::abs(sum) >= std::abs(x) ? (sum - t) + x : (x - t) + sum;
                sum = t;
            }
            json r;
            r["dtype"] = std::string(store::dtypeName(f.channels()[k].dtype));
            r["first"] = head(v);
            r["last"] = tail(v);
            r["min"] = *std::min_element(v.begin(), v.end());
            r["max"] = *std::max_element(v.begin(), v.end());
            r["mean"] = (sum + comp) / static_cast<double>(v.size());
            c["reader"] = r;
            channels.push_back(c);
        }
        json doc;
        doc["family"] = fam.name;
        doc["channels"] = channels;
        std::ofstream os(outDir / (fam.name + ".expect.json"), std::ios::binary);
        os << doc.dump(1) << "\n";
        REQUIRE(os.good());
    }
}
