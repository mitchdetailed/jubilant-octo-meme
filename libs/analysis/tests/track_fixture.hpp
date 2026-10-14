// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The synthetic-circuit fixture of AC-M3-04 (fixtures/track/synthetic-circuit, written by
// tools/gen_track_fixtures.py; D-041: the expected shapes come from that script, never from the code under
// test) and the measuring helpers of the track tests: distance to a polyline (brute force), a best-fit rigid
// alignment (2-D Kabsch) and the apex of a section (the point where half of its heading change is done).
// The helpers are written here, independently of src/track/.
#pragma once

#include "analysis_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

namespace esses::analysis::test {

struct Xy2 {
    double x = 0.0;
    double y = 0.0;
};

struct TruthLine {
    std::vector<double> dist;
    std::vector<Xy2> pts; // the closing row repeats the first point
};

struct TruthCorner {
    std::string name;
    double startM = 0.0;
    double endM = 0.0;
    double apexM = 0.0;
    double angleDeg = 0.0;
};

struct CircuitFixture {
    TrackOrigin origin;
    double lengthM = 0.0;
    bool ccw = true;
    LapWindow lap;
    Micros dropoutStart = 0;
    Micros dropoutEnd = 0;
    std::vector<TruthCorner> corners;
    std::vector<TruthCorner> kinks;
    TruthLine centre;
    TruthLine inner;
    TruthLine outer;
    GpsLap gps;
    GpsLap innerGps;
    GpsLap outerGps;
    UniformSignal speed;
    UniformSignal latAccel;
};

inline std::filesystem::path circuitDir() {
    return repoRoot() / "fixtures" / "track" / "synthetic-circuit";
}

inline std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Rows of a CSV file after its header; empty cells are empty strings.
inline std::vector<std::vector<std::string>> csvRows(const std::filesystem::path& path) {
    std::istringstream in(readText(path));
    std::vector<std::vector<std::string>> rows;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) {
            header = false;
            continue;
        }
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> cells;
        std::size_t at = 0;
        while (true) {
            const std::size_t comma = line.find(',', at);
            cells.push_back(line.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
            if (comma == std::string::npos) {
                break;
            }
            at = comma + 1;
        }
        rows.push_back(std::move(cells));
    }
    return rows;
}

inline TruthLine truthLine(const std::filesystem::path& path) {
    TruthLine t;
    bool wellFormed = true;
    for (const auto& r : csvRows(path)) {
        wellFormed = wellFormed && r.size() == 3;
        if (r.size() == 3) {
            t.dist.push_back(std::stod(r[0]));
            t.pts.push_back(Xy2{std::stod(r[1]), std::stod(r[2])});
        }
    }
    REQUIRE(wellFormed);
    REQUIRE(t.pts.size() > 2);
    return t;
}

// Columns 1.. of a uniform CSV ("t,a,b"): one UniformSignal per column, invalid where a cell is empty.
inline std::vector<UniformSignal> uniformColumns(const std::filesystem::path& path, std::size_t columns,
                                                 std::uint32_t rateHz) {
    std::vector<UniformSignal> out(columns);
    const auto rows = csvRows(path);
    REQUIRE(rows.size() > 2);
    const Micros t0 = std::stoll(rows[0][0]);
    for (UniformSignal& s : out) {
        s.rate = Rate::fromHz(rateHz);
        s.t0 = t0;
    }
    bool wellFormed = true;
    for (std::size_t i = 0; i < rows.size() && wellFormed; ++i) {
        wellFormed = rows[i].size() == columns + 1 &&
                     std::stoll(rows[i][0]) == out[0].rate.timeAt(static_cast<SampleIdx>(i), t0);
        if (!wellFormed) {
            break;
        }
        for (std::size_t c = 0; c < columns; ++c) {
            const std::string& cell = rows[i][c + 1];
            out[c].values.push_back(cell.empty() ? std::numeric_limits<double>::quiet_NaN()
                                                 : std::stod(cell));
            out[c].valid.push_back(cell.empty() ? 0 : 1);
        }
    }
    REQUIRE(wellFormed); // one row per sample period, every column present
    return out;
}

inline CircuitFixture loadCircuit() {
    CircuitFixture f;
    const nlohmann::json j = nlohmann::json::parse(readText(circuitDir() / "circuit.json"));
    f.origin = TrackOrigin{j["origin"]["lat"].get<double>(), j["origin"]["lon"].get<double>(),
                           j["origin"]["alt"].get<double>()};
    f.lengthM = j["lengthM"].get<double>();
    f.ccw = j["direction"].get<std::string>() == "ccw";
    f.lap = LapWindow{j["lap"]["startUs"].get<Micros>(), j["lap"]["endUs"].get<Micros>()};
    f.dropoutStart = j["gps"]["dropout"]["startUs"].get<Micros>();
    f.dropoutEnd = j["gps"]["dropout"]["endUs"].get<Micros>();
    const auto corners = [](const nlohmann::json& a) {
        std::vector<TruthCorner> out;
        for (const auto& c : a) {
            out.push_back(TruthCorner{c["name"].get<std::string>(), c["startM"].get<double>(),
                                      c["endM"].get<double>(), c["apexM"].get<double>(),
                                      c["angleDeg"].get<double>()});
        }
        return out;
    };
    f.corners = corners(j["corners"]);
    f.kinks = corners(j["kinks"]);
    f.centre = truthLine(circuitDir() / "expected.centre.csv");
    f.inner = truthLine(circuitDir() / "expected.inner.csv");
    f.outer = truthLine(circuitDir() / "expected.outer.csv");
    const std::uint32_t gpsHz = j["gps"]["rateHz"].get<std::uint32_t>();
    const auto gps = [&](const char* file) {
        auto cols = uniformColumns(circuitDir() / file, 2, gpsHz);
        return GpsLap{f.lap, std::move(cols[0]), std::move(cols[1])};
    };
    f.gps = gps("lap.gps.csv");
    f.innerGps = gps("inner.gps.csv");
    f.outerGps = gps("outer.gps.csv");
    auto imu = uniformColumns(circuitDir() / "lap.imu.csv", 2, j["imu"]["rateHz"].get<std::uint32_t>());
    f.speed = std::move(imu[0]);
    f.latAccel = std::move(imu[1]);
    return f;
}

// The distance from q to the polyline (every segment, brute force).
inline double distanceToPolyline(Xy2 q, const std::vector<Xy2>& line) {
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const Xy2 a = line[i];
        const Xy2 b = line[i + 1];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double l2 = dx * dx + dy * dy;
        const double t = l2 > 0.0 ? std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(q.x - (a.x + t * dx), q.y - (a.y + t * dy)));
    }
    return best;
}

// The RMS distance of the points of `line` from the polyline `truth`.
inline double rmsToTruth(const std::vector<TrackPoint>& line, const std::vector<Xy2>& truth) {
    double sum = 0.0;
    for (const TrackPoint& p : line) {
        const double d = distanceToPolyline(Xy2{p.x, p.y}, truth);
        sum += d * d;
    }
    return line.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(line.size()));
}

// The RMS residual of the best rigid alignment (rotation + translation) of a onto b (2-D Kabsch).
inline double rigidRms(const std::vector<Xy2>& a, const std::vector<Xy2>& b) {
    REQUIRE(a.size() == b.size());
    REQUIRE(!a.empty());
    const auto n = static_cast<double>(a.size());
    Xy2 ca;
    Xy2 cb;
    for (std::size_t i = 0; i < a.size(); ++i) {
        ca.x += a[i].x / n;
        ca.y += a[i].y / n;
        cb.x += b[i].x / n;
        cb.y += b[i].y / n;
    }
    double sxx = 0.0;
    double sxy = 0.0;
    double syx = 0.0;
    double syy = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double ax = a[i].x - ca.x;
        const double ay = a[i].y - ca.y;
        const double bx = b[i].x - cb.x;
        const double by = b[i].y - cb.y;
        sxx += ax * bx;
        sxy += ax * by;
        syx += ay * bx;
        syy += ay * by;
    }
    const double angle = std::atan2(sxy - syx, sxx + syy);
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double ax = a[i].x - ca.x;
        const double ay = a[i].y - ca.y;
        const double rx = c * ax - s * ay + cb.x;
        const double ry = s * ax + c * ay + cb.y;
        sum += (rx - b[i].x) * (rx - b[i].x) + (ry - b[i].y) * (ry - b[i].y);
    }
    return std::sqrt(sum / n);
}

// The heading at track distance d from the chord p(d + 5) - p(d - 5).
inline double chordHeading(const TrackMap& map, double d) {
    const TrackPoint a = map.pointAt(d - 5.0);
    const TrackPoint b = map.pointAt(d + 5.0);
    return std::atan2(b.y - a.y, b.x - a.x);
}

// The apex of a section: the track distance where half of the section's heading change is done (heading
// sampled every metre, unwrapped).
inline double apexOf(const TrackMap& map, const TrackSection& s) {
    std::vector<double> d;
    std::vector<double> h;
    for (double x = s.startM; x <= s.endM + 1e-9; x += 1.0) {
        const double theta = chordHeading(map, x);
        if (!h.empty()) {
            double delta = theta - h.back();
            while (delta > std::numbers::pi) {
                delta -= 2.0 * std::numbers::pi;
            }
            while (delta <= -std::numbers::pi) {
                delta += 2.0 * std::numbers::pi;
            }
            h.push_back(h.back() + delta);
        } else {
            h.push_back(theta);
        }
        d.push_back(x);
    }
    const double half = 0.5 * (h.back() - h.front());
    for (std::size_t i = 1; i < h.size(); ++i) {
        const double a = h[i - 1] - h.front();
        const double b = h[i] - h.front();
        if ((half >= 0.0 && b >= half) || (half < 0.0 && b <= half)) {
            const double t = b != a ? (half - a) / (b - a) : 0.0;
            return d[i - 1] + t * (d[i] - d[i - 1]);
        }
    }
    return 0.5 * (s.startM + s.endM);
}

inline std::vector<TrackSection> cornersOf(const TrackMap& map) {
    std::vector<TrackSection> out;
    for (const TrackSection& s : map.sections) {
        if (s.set == TrackMap::kDefaultSet && s.category == "corner") {
            out.push_back(s);
        }
    }
    return out;
}

} // namespace esses::analysis::test
