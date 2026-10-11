// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private smoke tests over the user's real .ld files (spike S-1, fixtures/private/s1; never committed). They
// skip cleanly when fixtures/private is absent (08 section 6.2, tag [oracle]).
//
// 1. The reader against the independent oracle (D-041): for every .ld listed in corpus.json, the channel list
//    and per-channel min/max/mean/first/last of the stored values and of the scaled values are compared with
//    ref/<id>.json, written by tools/ld_ref_decode.py. Integers bit-exact, floats within 1 F32 ulp; a mean
//    additionally allows the F32 rounding of the samples it averages (half an ulp of the largest magnitude).
//    The oracle marks every sample valid, so validity is not compared here (05 section 3.11).
// 2. read -> write -> read of every real file: samples, validity, enumerations, events and the unknown header
//    and detail-record bytes survive (05 section 5 item 8); 0x5DE..0x5E1 is always written as 0 (D-143).
// The fixtures root can be moved with ESSES_PRIVATE_FIXTURES (the directory that holds s1/).
#include "ld/ld_strings.hpp"
#include "ld_test_support.hpp"

#include <esses/base/utf8.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::ld;
using namespace esses::formats::ld::test;
namespace fs = std::filesystem;
namespace L = esses::formats::ld::layout;
using json = nlohmann::json;

namespace {

fs::path s1Dir() {
    if (const auto v = getEnv(envName("PRIVATE_FIXTURES")); v && !v->empty()) {
        return pathFromUtf8(*v) / "s1";
    }
    return pathFromUtf8(ESSES_TEST_REPO_ROOT) / "fixtures" / "private" / "s1";
}

std::optional<json> loadJson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return json::parse(ss.str());
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

struct CorpusFile {
    std::size_t index = 0; // position among the corpus .ld entries = the ref id prefix
    fs::path path;
    std::uint64_t size = 0;
    fs::path ref;
};

// The .ld entries of corpus.json with their ref/<nn>_*.json, or empty when the fixtures are absent.
std::vector<CorpusFile> corpus() {
    const fs::path dir = s1Dir();
    const auto c = loadJson(dir / "corpus.json");
    if (!c || !c->is_array()) {
        return {};
    }
    std::map<std::size_t, fs::path> refs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "ref", ec)) {
        const std::string stem = pathToUtf8(e.path().filename());
        const std::size_t us = stem.find('_');
        if (us == std::string::npos) {
            continue;
        }
        try {
            refs[static_cast<std::size_t>(std::stoul(stem.substr(0, us)))] = e.path();
        } catch (const std::exception&) {
        }
    }
    std::vector<CorpusFile> out;
    std::size_t k = 0;
    for (const auto& e : *c) {
        if (e.value("ext", "") != ".ld") {
            continue;
        }
        CorpusFile f;
        f.index = k++;
        f.path = pathFromUtf8(e.value("path", ""));
        f.size = e.value("size", std::uint64_t{0});
        if (const auto it = refs.find(f.index); it != refs.end()) {
            f.ref = it->second;
        }
        out.push_back(std::move(f));
    }
    return out;
}

// A JSON statistic: a number, or "nan"/"inf"/"-inf" as the oracle writes non-finite values.
std::optional<double> num(const json& v) {
    if (v.is_number()) {
        return v.get<double>();
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s == "nan") {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (s == "inf") {
            return std::numeric_limits<double>::infinity();
        }
        if (s == "-inf") {
            return -std::numeric_limits<double>::infinity();
        }
    }
    return std::nullopt;
}

bool sameExact(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Neumaier-compensated sum, as Python 3.12's sum() of floats.
struct Sum {
    double s = 0.0, c = 0.0;
    void add(double x) {
        const double t = s + x;
        c += std::abs(s) >= std::abs(x) ? (s - t) + x : (x - t) + s;
        s = t;
    }
    double value() const { return s + c; }
};

struct Stats {
    std::uint64_t n = 0, used = 0, nonfinite = 0;
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    Sum sum;
    std::int64_t intSum = 0;
    bool intExact = true;
    std::vector<double> first, last;
    double mean() const {
        if (used == 0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (intExact && std::abs(intSum) < (std::int64_t{1} << 53)) {
            return static_cast<double>(intSum) /
                   static_cast<double>(used); // correctly rounded, as the oracle
        }
        return sum.value() / static_cast<double>(used);
    }
};

// Statistics of channel k over all samples, in blocks; raw (stored values) or scaled (the reader's output).
Stats stats(const LdFile& f, std::uint32_t k, bool raw) {
    const LdChannel& c = f.channels()[k];
    Stats st;
    st.n = c.sampleCount;
    const bool integers = raw && !isFloatSample(c.sampleType);
    st.intExact = integers;
    constexpr std::uint64_t kBlock = std::uint64_t{1} << 20;
    for (std::uint64_t first = 0; first < c.sampleCount; first += kBlock) {
        const std::uint64_t n = std::min(kBlock, c.sampleCount - first);
        auto v = raw ? f.rawValues(k, first, n) : f.decodeAsDouble(k, first, n);
        REQUIRE(v.ok());
        for (std::uint64_t i = 0; i < n; ++i) {
            const double x = (*v)[static_cast<std::size_t>(i)];
            const std::uint64_t idx = first + i;
            if (idx < 5) {
                st.first.push_back(x);
            }
            if (idx + 5 >= c.sampleCount) {
                st.last.push_back(x);
            }
            if (!std::isfinite(x)) {
                ++st.nonfinite;
                continue;
            }
            ++st.used;
            st.min = std::min(st.min, x);
            st.max = std::max(st.max, x);
            st.sum.add(x);
            if (integers) {
                st.intSum += static_cast<std::int64_t>(x);
            }
        }
    }
    return st;
}

class Report {
public:
    explicit Report(std::string file) : file_(std::move(file)) {}
    void fail(const std::string& channel, const std::string& what) {
        ++failures_;
        if (shown_ < 40) {
            ++shown_;
            WARN(file_ << " | " << channel << " | " << what);
        }
    }
    std::size_t failures() const { return failures_; }

private:
    std::string file_;
    std::size_t failures_ = 0;
    std::size_t shown_ = 0;
};

std::string fmt(double v) {
    std::ostringstream os;
    os.precision(17);
    os << v;
    return os.str();
}

// The 32-byte name as the oracle decodes it: cut at NUL, cp1252, right-trimmed.
std::string oracleName(const LdChannel& c) {
    const auto* p = c.enc.raw.data() + L::kRecName;
    std::size_t n = 0;
    while (n < L::kNameWidth && p[n] != std::byte{0}) {
        ++n;
    }
    std::string s = cp1252ToUtf8({p, n});
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

void compareChannel(const LdFile& f, std::uint32_t k, const json& rc, Report& rep) {
    const LdChannel& c = f.channels()[k];
    const std::string name = c.loggedName;
    const auto field = [&](const char* key, std::uint64_t mine) {
        if (!rc.contains(key) || rc.at(key).get<std::int64_t>() != static_cast<std::int64_t>(mine)) {
            rep.fail(name, std::string(key) + " differs: " + std::to_string(mine));
        }
    };
    const auto sfield = [&](const char* key, std::int64_t mine) {
        if (!rc.contains(key) || rc.at(key).get<std::int64_t>() != mine) {
            rep.fail(name, std::string(key) + " differs: " + std::to_string(mine));
        }
    };
    if (rc.value("name", std::string()) != oracleName(c)) {
        rep.fail(name, "name differs from the oracle's '" + rc.value("name", std::string()) + "'");
    }
    field("data_ptr", c.enc.dataOffset);
    field("count_decoded", c.sampleCount);
    field("counter", c.enc.counter);
    field("dtype_a", c.enc.typeCode);
    field("dtype", c.enc.bytesPerSample);
    field("freq", c.enc.rateHz);
    sfield("shift", c.enc.shift);
    sfield("mul", c.enc.mul);
    sfield("scale", c.enc.scale);
    sfield("dec", c.enc.dec);

    // ---- stored values -----------------------------------------------------------------------------
    const json& rs = rc.at("raw_stats");
    const Stats raw = stats(f, k, true);
    const bool sampled = rs.value("stride", 1) != 1;
    if (rs.value("n", std::uint64_t{0}) != raw.n) {
        rep.fail(name, "raw n differs");
        return;
    }
    if (raw.n == 0) {
        return;
    }
    const auto listEq = [&](const json& list, const std::vector<double>& mine, const char* what) {
        if (!list.is_array() || list.size() != mine.size()) {
            rep.fail(name, std::string(what) + " length differs");
            return;
        }
        for (std::size_t i = 0; i < mine.size(); ++i) {
            const auto o = num(list[i]);
            if (!o || !sameExact(*o, mine[i])) {
                rep.fail(name, std::string(what) + "[" + std::to_string(i) + "] " + fmt(mine[i]) +
                                   " != " + list[i].dump());
            }
        }
    };
    listEq(rs.at("first"), raw.first, "raw first");
    listEq(rs.at("last"), raw.last, "raw last");
    if (!sampled) {
        if (rs.value("used", std::uint64_t{0}) != raw.used ||
            rs.value("nonfinite", std::uint64_t{0}) != raw.nonfinite) {
            rep.fail(name, "raw used/nonfinite counts differ");
        }
        if (raw.used != 0) {
            const auto omin = num(rs.at("min"));
            const auto omax = num(rs.at("max"));
            const auto omean = num(rs.at("mean"));
            if (!omin || !sameExact(*omin, raw.min) || !omax || !sameExact(*omax, raw.max)) {
                rep.fail(name, "raw min/max " + fmt(raw.min) + "/" + fmt(raw.max) +
                                   " != " + rs.at("min").dump() + "/" + rs.at("max").dump());
            }
            if (!omean || f32UlpDistance(raw.mean(), *omean) > 1) {
                rep.fail(name, "raw mean " + fmt(raw.mean()) + " != " + rs.at("mean").dump());
            }
        }
    }

    // ---- scaled values (the reader's output dtype) --------------------------------------------------
    const json& all = rc.at("stats");
    const bool orderMatters = c.enc.shift != 0 && c.enc.mul != 0 && c.enc.mul != 1;
    const char* key = all.contains("afterMul") ? "afterMul" : "beforeMul";
    if (orderMatters && !all.contains("afterMul")) {
        rep.fail(name, "oracle has no afterMul statistics for a shift/mul channel");
        return;
    }
    const json& os = all.at(key);
    const Stats sc = stats(f, k, false);
    const bool exactInt = c.dtype == store::Dtype::I32;
    const auto close = [&](double mine, double oracle) {
        return exactInt ? sameExact(mine, oracle) : f32UlpDistance(mine, oracle) <= 1;
    };
    const auto scaledList = [&](const json& list, const std::vector<double>& mine, const char* what) {
        if (!list.is_array() || list.size() != mine.size()) {
            rep.fail(name, std::string(what) + " length differs");
            return;
        }
        for (std::size_t i = 0; i < mine.size(); ++i) {
            const auto o = num(list[i]);
            if (!o || !close(mine[i], *o)) {
                rep.fail(name, std::string(what) + "[" + std::to_string(i) + "] " + fmt(mine[i]) +
                                   " != " + list[i].dump());
            }
        }
    };
    scaledList(os.at("first"), sc.first, "scaled first");
    scaledList(os.at("last"), sc.last, "scaled last");
    if (!sampled && sc.used != 0) {
        const auto omin = num(os.at("min"));
        const auto omax = num(os.at("max"));
        const auto omean = num(os.at("mean"));
        if (!omin || !close(sc.min, *omin) || !omax || !close(sc.max, *omax)) {
            rep.fail(name, "scaled min/max " + fmt(sc.min) + "/" + fmt(sc.max) +
                               " != " + os.at("min").dump() + "/" + os.at("max").dump());
        }
        if (omean && omin && omax) {
            const double tol =
                f32Ulp(*omean) + (exactInt || c.dtype == store::Dtype::F64
                                      ? 0.0
                                      : std::ldexp(std::max(std::abs(*omin), std::abs(*omax)), -24));
            if (!(std::abs(sc.mean() - *omean) <= tol)) {
                rep.fail(name, "scaled mean " + fmt(sc.mean()) + " != " + os.at("mean").dump());
            }
        } else {
            rep.fail(name, "oracle has no scaled min/max/mean");
        }
    }
}

} // namespace

TEST_CASE("ld private corpus: the reader agrees with the independent oracle", "[oracle][ld_private]") {
    const auto files = corpus();
    if (files.empty()) {
        SKIP("fixtures/private/s1 is absent; the private corpus comparison needs the user's files");
    }
    std::size_t compared = 0, skipped = 0, channels = 0, failures = 0;
    for (const CorpusFile& cf : files) {
        std::error_code ec;
        if (cf.ref.empty() || !fs::exists(cf.path, ec) || fs::file_size(cf.path, ec) != cf.size) {
            ++skipped;
            continue;
        }
        const auto ref = loadJson(cf.ref);
        REQUIRE(ref.has_value());
        auto opened = LdFile::open(cf.path);
        REQUIRE(opened.ok());
        const LdFile& f = **opened;
        Report rep(pathToUtf8(cf.path.filename()));
        const json& rch = ref->at("channels");
        if (rch.size() != f.channels().size()) {
            rep.fail("-", "channel count " + std::to_string(f.channels().size()) + " != oracle " +
                              std::to_string(rch.size()));
        } else {
            for (std::uint32_t k = 0; k < f.channels().size(); ++k) {
                compareChannel(f, k, rch[k], rep);
            }
            channels += f.channels().size();
        }
        // The header-only test file: the oracle reports no channels, the reader a valid empty source.
        if (rch.empty() && !f.headerOnly() && !f.log().hasErrors()) {
            rep.fail("-", "no channels, but neither header-only nor an error");
        }
        failures += rep.failures();
        ++compared;
    }
    WARN("private corpus: " << compared << " files compared (" << channels << " channels), " << skipped
                            << " skipped, " << failures << " differences");
    CHECK(compared > 0);
    CHECK(failures == 0);
}

TEST_CASE("ld private corpus: read -> write -> read keeps every real file's data",
          "[oracle][ld_private][slow]") {
    const auto files = corpus();
    if (files.empty()) {
        SKIP("fixtures/private/s1 is absent; the private round trip needs the user's files");
    }
    std::size_t done = 0;
    for (const CorpusFile& cf : files) {
        std::error_code ec;
        if (!fs::exists(cf.path, ec) || fs::file_size(cf.path, ec) != cf.size) {
            continue;
        }
        INFO(pathToUtf8(cf.path));
        auto opened = LdFile::open(cf.path);
        REQUIRE(opened.ok());
        const LdFile& f1 = **opened;
        auto in = LdWriter::inputFromFile(f1);
        REQUIRE(in.ok());
        auto out = LdWriter::build(*in, LdWriter::optionsFromFile(f1));
        REQUIRE(out.ok());
        const auto f2 = parseBytes(std::move(*out));
        const std::span<const std::byte> h2 = f2->bytes().first(L::kHeaderSize);
        CHECK(h2[L::kHdrProLogging] == std::byte{0});
        // Header bytes outside the pointers and counts the writer owns are verbatim.
        const auto owned = [](std::size_t o) {
            return (o >= 0x004 && o < 0x010) || (o >= 0x024 && o < 0x028) || (o >= 0x056 && o < 0x05A) ||
                   (o >= 0x5DE && o < 0x5E2) || (o >= 0x688 && o < 0x694);
        };
        std::size_t headerDiffs = 0;
        for (std::size_t o = 0; o < L::kHeaderSize; ++o) {
            headerDiffs += !owned(o) && f1.rawHeader()[o] != h2[o] ? 1 : 0;
        }
        CHECK(headerDiffs == 0);
        CHECK(f2->header().deviceType == f1.header().deviceType);
        CHECK(f2->header().driver == f1.header().driver);
        CHECK(f2->eventRecord().has_value() == f1.eventRecord().has_value());
        if (f1.eventRecord() && f2->eventRecord()) {
            CHECK(f2->eventRecord()->longComment == f1.eventRecord()->longComment);
        }
        if (f1.vehicleRecord()) {
            REQUIRE(f2->vehicleRecord().has_value());
            CHECK(std::equal(f1.rawVehicleRecord().begin(), f1.rawVehicleRecord().end(),
                             f2->rawVehicleRecord().begin()));
        }
        REQUIRE(f2->channels().size() == f1.channels().size());
        for (std::uint32_t k = 0; k < f1.channels().size(); ++k) {
            const LdChannel& a = f1.channels()[k];
            const LdChannel& b = f2->channels()[k];
            INFO(a.loggedName);
            CHECK(b.loggedName == a.loggedName);
            CHECK(b.unit == a.unit);
            CHECK(b.shortName == a.shortName);
            CHECK(b.dtype == a.dtype);
            CHECK(b.kind == a.kind);
            CHECK(b.interp == a.interp);
            CHECK(b.dps == a.dps);
            CHECK(b.sampleCount == a.sampleCount);
            CHECK(b.invalid == a.invalid);
            CHECK(b.enumRef == a.enumRef);
            const auto da = f1.bytes().subspan(a.enc.dataOffset, a.sampleCount * a.enc.bytesPerSample);
            const auto db = f2->bytes().subspan(b.enc.dataOffset, b.sampleCount * b.enc.bytesPerSample);
            CHECK(std::equal(da.begin(), da.end(), db.begin(), db.end()));
        }
        REQUIRE(f2->enumTables().size() == f1.enumTables().size());
        for (std::size_t t = 0; t < f1.enumTables().size(); ++t) {
            CHECK(f2->enumTables()[t].index == f1.enumTables()[t].index);
            CHECK(f2->enumTables()[t].entries == f1.enumTables()[t].entries);
        }
        REQUIRE(f2->deviceEvents().size() == f1.deviceEvents().size());
        for (std::size_t e = 0; e < f1.deviceEvents().size(); ++e) {
            CHECK(f2->deviceEvents()[e].code == f1.deviceEvents()[e].code);
            CHECK(f2->deviceEvents()[e].tUs == f1.deviceEvents()[e].tUs);
        }
        ++done;
    }
    CHECK(done > 0);
}
