// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SectionDetector (06 section 13.5 item 3). Owner: the track work (src/track/).
//
// The centreline is resampled every metre; the heading comes from chords of +-smoothingM / 2 and the
// curvature from the heading change over the same span, so the curvature is smoothed over about 2 *
// smoothingM. Samples whose |curvature| exceeds 1 / cornerRadiusM are corner samples (signed: left or right),
// the others straight. Runs shorter than the minimum are absorbed shortest first: a short corner becomes
// straight; a short straight between two corners turning the same way joins them into one corner, and between
// corners turning opposite ways (an S-bend) is split between them. A corner whose heading change (the sum of
// its curvature) is below minTurnDeg becomes straight too; both steps repeat until nothing changes. On a
// closed map a run that crosses the start line is cut there, and a piece shorter than the minimum goes to its
// neighbour. Boundaries are rounded to the millimetre.

#include "track_path.hpp"

#include <esses/analysis/section_detector.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace esses::analysis {

namespace {

using trackpath::Xy;

struct Run {
    std::size_t start = 0; // first sample (cyclic index on a closed map)
    std::size_t count = 0; // samples
    int sign = 0;          // 0 straight, +1 left corner, -1 right corner
};

double roundMm(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

// Merges neighbours with the same sign (cyclically on a closed map).
void coalesce(std::vector<Run>& runs, bool closed, std::size_t n) {
    std::vector<Run> out;
    for (const Run& r : runs) {
        if (!out.empty() && out.back().sign == r.sign) {
            out.back().count += r.count;
        } else {
            out.push_back(r);
        }
    }
    if (closed && out.size() > 1 && out.front().sign == out.back().sign) {
        out.back().count += out.front().count;
        out.erase(out.begin());
    }
    if (closed && out.size() == 1) {
        out.front().start = 0;
        out.front().count = n;
    }
    runs = std::move(out);
}

} // namespace

Result<void> SectionDetector::defaults(TrackMap& map, const SectionDetectorOptions& options) {
    if (map.centre.size() < 2 || !std::isfinite(map.lengthM) || map.lengthM <= 0.0) {
        Error e{ErrorCode::Invalid, "the track map has no centreline", {}};
        e.context.detail = "/lines/centre";
        return Failure{std::move(e)};
    }
    if (!std::isfinite(options.cornerRadiusM) || options.cornerRadiusM <= 0.0 ||
        !std::isfinite(options.smoothingM) || options.smoothingM < 0.0 ||
        !std::isfinite(options.minSectionM) || !std::isfinite(options.minTurnDeg) ||
        options.minTurnDeg < 0.0) {
        return Failure{Error{ErrorCode::Invalid, "the section detector options are out of range", {}}};
    }
    const double minM = std::max(options.minSectionM, TrackMap::kMinSectionM);

    // The centreline every metre (without the closing duplicate of a closed loop).
    std::vector<Xy> pts;
    pts.reserve(map.centre.size());
    for (const TrackPoint& p : map.centre) {
        pts.push_back(Xy{p.x, p.y});
    }
    const bool closed = map.closed;
    if (closed && pts.size() > 2 &&
        std::hypot(pts.back().x - pts.front().x, pts.back().y - pts.front().y) < 1e-6) {
        pts.pop_back();
    }
    std::vector<TrackPoint> uniform = trackpath::resample(pts, closed, 1.0);
    const double polyLength = uniform.empty() ? 0.0 : uniform.back().dist;
    if (closed && !uniform.empty()) {
        uniform.pop_back();
    }
    const std::size_t n = uniform.size();
    if (n < 2 || polyLength <= 0.0) {
        Error e{ErrorCode::Invalid, "the track map's centreline has no length", {}};
        e.context.detail = "/lines/centre";
        return Failure{std::move(e)};
    }
    const double toMap = map.lengthM / polyLength; // centreline metres -> track distance
    std::vector<Xy> xy(n);
    for (std::size_t i = 0; i < n; ++i) {
        xy[i] = Xy{uniform[i].x, uniform[i].y};
    }
    const double half = 0.5 * options.smoothingM;
    const std::vector<double> kappa =
        trackpath::chordCurvature(trackpath::chordHeadings(xy, 1.0, half, closed), 1.0, half, closed);
    const double threshold = 1.0 / options.cornerRadiusM;

    // Runs of equal sign.
    std::vector<Run> runs;
    for (std::size_t i = 0; i < n; ++i) {
        const int sign = std::abs(kappa[i]) > threshold ? (kappa[i] > 0.0 ? 1 : -1) : 0;
        if (!runs.empty() && runs.back().sign == sign) {
            ++runs.back().count;
        } else {
            runs.push_back(Run{i, 1, sign});
        }
    }
    coalesce(runs, closed, n);

    // Absorb short runs, shortest first, and straighten weak corners, until nothing changes.
    const auto lengthOf = [&](const Run& r) { return static_cast<double>(r.count) * toMap; };
    const auto turnOf = [&](const Run& r) {
        double turn = 0.0;
        for (std::size_t j = 0; j < r.count; ++j) {
            turn += kappa[(r.start + j) % n]; // one sample per metre
        }
        return std::abs(turn);
    };
    const double minTurn = options.minTurnDeg * std::numbers::pi / 180.0;
    for (bool changed = true; changed;) {
        changed = false;
        while (runs.size() > 1) {
            std::size_t shortest = runs.size();
            for (std::size_t i = 0; i < runs.size(); ++i) {
                if (lengthOf(runs[i]) < minM &&
                    (shortest == runs.size() || runs[i].count < runs[shortest].count)) {
                    shortest = i;
                }
            }
            if (shortest == runs.size()) {
                break;
            }
            const std::size_t m = runs.size();
            const bool hasPrev = closed || shortest > 0;
            const bool hasNext = closed || shortest + 1 < m;
            const std::size_t ip = (shortest + m - 1) % m;
            const std::size_t in = (shortest + 1) % m;
            Run& r = runs[shortest];
            if (r.sign != 0) {
                r.sign = 0; // a short corner is part of the straight
            } else if (hasPrev && hasNext && ip != in && runs[ip].sign != 0 && runs[in].sign != 0 &&
                       runs[ip].sign != runs[in].sign) {
                // An S-bend: the straight between is split between the two corners.
                const std::size_t first = r.count / 2;
                runs[ip].count += first;
                runs[in].count += r.count - first;
                runs[in].start = (runs[in].start + n - (r.count - first)) % n;
                runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(shortest));
                coalesce(runs, closed, n);
                continue;
            } else {
                // A short straight joins its corner neighbour (the previous one when both turn the same way).
                r.sign = ((hasPrev && runs[ip].sign != 0) || !hasNext) ? runs[ip].sign : runs[in].sign;
            }
            coalesce(runs, closed, n);
        }
        for (Run& r : runs) {
            if (runs.size() > 1 && r.sign != 0 && turnOf(r) < minTurn) {
                r.sign = 0;
                changed = true;
            }
        }
        if (changed) {
            coalesce(runs, closed, n);
        }
    }

    // Boundaries in track distance; a closed map is cut at the start line.
    struct Piece {
        double start = 0.0;
        double end = 0.0;
        int sign = 0;
    };
    std::vector<Piece> pieces;
    for (const Run& r : runs) {
        const double a = static_cast<double>(r.start) * toMap;
        const double b = static_cast<double>(r.start + r.count) * toMap;
        if (runs.size() == 1) {
            pieces.push_back(Piece{0.0, map.lengthM, r.sign});
        } else if (closed && b > map.lengthM + 1e-9) {
            pieces.push_back(Piece{a, map.lengthM, r.sign});
            pieces.push_back(Piece{0.0, b - map.lengthM, r.sign});
        } else {
            pieces.push_back(Piece{a, std::min(b, map.lengthM), r.sign});
        }
    }
    std::sort(pieces.begin(), pieces.end(), [](const Piece& x, const Piece& y) { return x.start < y.start; });
    if (!pieces.empty()) {
        pieces.front().start = 0.0;
        pieces.back().end = map.lengthM;
    }
    // A piece cut by the start line that is now too short joins its neighbour.
    for (std::size_t i = 0; i < pieces.size() && pieces.size() > 1;) {
        if (pieces[i].end - pieces[i].start >= minM) {
            ++i;
            continue;
        }
        if (i == 0) {
            pieces[1].start = pieces[0].start;
        } else {
            pieces[i - 1].end = pieces[i].end;
        }
        pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(i));
        i = 0;
    }

    // Replace the generated Default sections; keep every other section.
    std::vector<TrackSection> kept;
    std::set<std::string, std::less<>> keptIds;
    for (TrackSection& s : map.sections) {
        if (s.set == TrackMap::kDefaultSet && !s.userDefined) {
            continue;
        }
        keptIds.insert(s.id);
        kept.push_back(std::move(s));
    }
    std::vector<TrackSection> generated;
    int turns = 0;
    int straights = 0;
    for (const Piece& p : pieces) {
        if (p.end - p.start < minM) {
            continue; // only a track shorter than the minimum
        }
        TrackSection s;
        const bool corner = p.sign != 0;
        const int number = corner ? ++turns : ++straights;
        s.name = std::string(corner ? "Turn " : "Straight ") + std::to_string(number);
        s.id = std::string(corner ? "t" : "s") + std::to_string(number);
        for (int k = 2; keptIds.count(s.id) != 0; ++k) {
            s.id = std::string(corner ? "t" : "s") + std::to_string(number) + "-" + std::to_string(k);
        }
        s.set = std::string(TrackMap::kDefaultSet);
        s.category = corner ? "corner" : "straight";
        s.userDefined = false;
        s.startM = roundMm(p.start);
        s.endM = roundMm(p.end);
        if (!generated.empty()) {
            s.startM = generated.back().endM; // tile without gaps after rounding
        }
        generated.push_back(std::move(s));
    }
    if (!generated.empty()) {
        generated.front().startM = 0.0;
        generated.back().endM = map.lengthM;
    }
    for (TrackSection& s : kept) {
        generated.push_back(std::move(s));
    }
    map.sections = std::move(generated);
    return ok();
}

} // namespace esses::analysis
