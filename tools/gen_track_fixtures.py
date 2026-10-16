#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Synthetic track-generation fixtures with a known centreline (AC-M3-04, 07 section 6.2; D-041).

Writes fixtures/track/synthetic-circuit/ (CC0-1.0):
  circuit.json         the ground truth and the simulation facts: the ENU origin, the true length and direction,
                       the lap window, the corners (start, end, apex = heading midpoint, signed angle, radius),
                       the gentle kink that is not a corner, the GPS and IMU rates and noise, the dropout
  expected.centre.csv  the true centreline "dist,x,y" in ENU metres every 1 m, the last row closing the loop
  expected.inner.csv   the inner and outer lines (the centreline offset 6 m to the left and to the right),
  expected.outer.csv   "dist,x,y" against the centreline distance
  lap.gps.csv          "t,lat,lon" at 10 Hz of one lap driven on the centreline: WGS-84 degrees of the true
                       position plus white noise of sigma 1.5 m on each ENU axis, a 2 s dropout (empty cells)
                       through a corner
  lap.imu.csv          "t,speed,latAccel" at 50 Hz of the same lap: m/s and m/s^2, lateral acceleration positive
                       to the left (ISO 8855, a left turn), with a speed scale error, white noise and a lateral
                       bias
  inner.gps.csv        "t,lat,lon" at 10 Hz of a lap driven on the inner line and one on the outer line (same
  outer.gps.csv        noise, no dropout)
Times are integer microseconds; every signal starts at 0 and runs 1 s past each end of the lap window
[1 s, 1 s + lap time), so the generator must cut the lap out of a longer signal.

Nothing here comes from the code under test: the centreline is integrated from a closed-form curvature profile
(straights, corners with linear curvature ramps; two straights are lengthened to close the loop exactly), the lap
follows a velocity profile limited by lateral grip, braking and traction, and the geodetic coordinates come from
the WGS-84 ellipsoid (local tangent plane about the origin; the inverse is iterated to below 1e-12 degrees).

Standard library only:  python -I tools/gen_track_fixtures.py --root <repo> [--check]
--check regenerates into memory and exits 1 when a committed file differs; numbers are compared within one unit
of their last written decimal, so a last-ulp difference of the platform's libm cannot fail the check.
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import random
import sys

VERSION = "1.0.0"
FAMILY = "synthetic-circuit"

# WGS-84 (NIMA TR8350.2).
WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = WGS84_F * (2.0 - WGS84_F)

ORIGIN = {"lat": 45.0612, "lon": 7.4287, "alt": 250.0}  # a neutral, made-up place
HEADING0_DEG = 20.0  # the heading at the start line, degrees counter-clockwise from east

# The circuit, from the start line: ("straight", length) or ("corner", name, signed angle in degrees, radius,
# ramp length). Positive angles turn left (counter-clockwise); the angles sum to +360 (a ccw circuit). The two
# straights marked "close" are lengthened to close the loop.
LAYOUT = [
    ("straight", 200.0),
    ("corner", "T1", 90.0, 50.0, 15.0),
    ("straight", 330.0, "close"),
    ("corner", "T2", -40.0, 110.0, 15.0),
    ("straight", 120.0),
    ("corner", "T3", 130.0, 40.0, 15.0),
    ("straight", 600.0, "close"),
    ("corner", "T4", 160.0, 18.0, 10.0),
    ("straight", 80.0),
    ("corner", "T5", -70.0, 90.0, 15.0),
    ("straight", 100.0),
    ("kink", "K1", -12.0, 400.0, 15.0),
    ("straight", 100.0),
    ("corner", "T6", 60.0, 70.0, 15.0),
    ("straight", 120.0),
    ("corner", "T7", 42.0, 120.0, 15.0),
    ("straight", 250.0),
]

TRACK_HALF_WIDTH = 6.0   # inner/outer lines
GPS_HZ = 10
IMU_HZ = 50
GPS_SIGMA = 1.5          # m, white, per ENU axis
DROPOUT_S = 2.0
DROPOUT_CORNER = "T6"    # the dropout starts where T6's curvature reaches its radius
SPEED_SCALE = 1.008      # the logged speed reads 0.8 % high (tyre radius)
SPEED_SIGMA = 0.05       # m/s
LAT_SIGMA = 0.25         # m/s^2
LAT_BIAS = 0.08          # m/s^2
LAT_GRIP = 15.0          # m/s^2, the velocity profile's lateral limit
BRAKE = 14.0             # m/s^2
TRACTION = 6.0           # m/s^2 at standstill, falling linearly to 0 at TOP_SPEED
TOP_SPEED = 75.0         # m/s
PAD_US = 1_000_000       # signal before and after the lap window
SEED = 20261014

DS = 0.01                # integration step of the centreline, m
TABLE_STEP = 0.05        # stored table step, m
PROFILE_STEP = 0.5       # velocity-profile step, m


# ---- geodesy -------------------------------------------------------------------------------------------------

def geodetic_to_ecef(lat: float, lon: float, h: float) -> tuple[float, float, float]:
    p, l = math.radians(lat), math.radians(lon)
    n = WGS84_A / math.sqrt(1.0 - WGS84_E2 * math.sin(p) ** 2)
    return ((n + h) * math.cos(p) * math.cos(l), (n + h) * math.cos(p) * math.sin(l),
            (n * (1.0 - WGS84_E2) + h) * math.sin(p))


def ecef_to_geodetic(x: float, y: float, z: float) -> tuple[float, float, float]:
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1.0 - WGS84_E2))
    h = 0.0
    for _ in range(50):
        n = WGS84_A / math.sqrt(1.0 - WGS84_E2 * math.sin(lat) ** 2)
        h = p / math.cos(lat) - n
        nxt = math.atan2(z, p * (1.0 - WGS84_E2 * n / (n + h)))
        if abs(nxt - lat) < 1e-15:
            lat = nxt
            break
        lat = nxt
    return math.degrees(lat), math.degrees(lon), h


class Enu:
    """The local tangent plane about ORIGIN: (east, north) metres -> (lat, lon) degrees."""

    def __init__(self, origin: dict) -> None:
        self.lat0 = math.radians(origin["lat"])
        self.lon0 = math.radians(origin["lon"])
        self.x0, self.y0, self.z0 = geodetic_to_ecef(origin["lat"], origin["lon"], origin["alt"])

    def to_geodetic(self, e: float, n: float) -> tuple[float, float]:
        sp, cp = math.sin(self.lat0), math.cos(self.lat0)
        sl, cl = math.sin(self.lon0), math.cos(self.lon0)
        dx = -sl * e - sp * cl * n
        dy = cl * e - sp * sl * n
        dz = cp * n
        lat, lon, _ = ecef_to_geodetic(self.x0 + dx, self.y0 + dy, self.z0 + dz)
        return lat, lon


# ---- the centreline ------------------------------------------------------------------------------------------

class Piece:
    """One piece of the curvature profile: kappa runs linearly from k0 to k1 over `length` metres."""

    def __init__(self, length: float, k0: float, k1: float) -> None:
        self.length, self.k0, self.k1 = length, k0, k1

    def heading_change(self, s: float) -> float:
        return self.k0 * s + 0.5 * (self.k1 - self.k0) * s * s / self.length if self.length > 0 else 0.0

    def kappa(self, s: float) -> float:
        return self.k0 + (self.k1 - self.k0) * s / self.length if self.length > 0 else self.k0


def pieces_of(layout: list) -> tuple[list[Piece], list[dict], list[int]]:
    """The curvature pieces, the corners (start/end/apex distances) and the indexes of the closing straights."""
    pieces: list[Piece] = []
    corners: list[dict] = []
    closing: list[int] = []
    s = 0.0
    for item in layout:
        if item[0] == "straight":
            if len(item) > 2 and item[2] == "close":
                closing.append(len(pieces))
            pieces.append(Piece(item[1], 0.0, 0.0))
            s += item[1]
            continue
        kind, name, angle_deg, radius, ramp = item
        angle = math.radians(angle_deg)
        k = math.copysign(1.0 / radius, angle)
        arc = abs(angle) * radius - ramp  # heading change = k * (arc + ramp)
        if arc < 0:
            raise ValueError(f"{name}: the ramps are longer than the corner")
        start = s
        pieces += [Piece(ramp, 0.0, k), Piece(arc, k, k), Piece(ramp, k, 0.0)]
        s += 2 * ramp + arc
        corners.append({"name": name, "kind": kind, "angleDeg": angle_deg, "radiusM": radius, "rampM": ramp,
                        "startM": start, "endM": s, "apexM": start + ramp + 0.5 * arc,
                        "fullRadiusM": start + ramp})
    return pieces, corners, closing


def integrate(pieces: list[Piece], heading0: float) -> tuple[list[tuple[float, float, float, float]], float]:
    """(s, x, y, kappa) every TABLE_STEP metres from the midpoint rule at DS with the exact heading."""
    table = [(0.0, 0.0, 0.0, pieces[0].kappa(0.0))]
    x = y = 0.0
    s_total = 0.0
    theta0 = heading0
    keep = round(TABLE_STEP / DS)
    for piece in pieces:
        steps = max(1, round(piece.length / DS))
        h = piece.length / steps
        for i in range(steps):
            mid = (i + 0.5) * h
            theta = theta0 + piece.heading_change(mid)
            x += h * math.cos(theta)
            y += h * math.sin(theta)
            if (i + 1) % keep == 0 or i + 1 == steps:
                table.append((s_total + (i + 1) * h, x, y, piece.kappa((i + 1) * h)))
        theta0 += piece.heading_change(piece.length)
        s_total += piece.length
    return table, s_total


def closed_circuit() -> tuple[list[Piece], list[dict], list[tuple[float, float, float, float]], float]:
    layout = [list(item) for item in LAYOUT]
    heading0 = math.radians(HEADING0_DEG)
    pieces, _, closing = pieces_of([tuple(i) for i in layout])
    table, _ = integrate(pieces, heading0)
    gx, gy = table[-1][1], table[-1][2]
    # Directions of the two closing straights (the heading where they start).
    dirs = []
    for idx in closing:
        theta = heading0 + sum(p.heading_change(p.length) for p in pieces[:idx])
        dirs.append((math.cos(theta), math.sin(theta)))
    (ax, ay), (bx, by) = dirs
    det = ax * by - ay * bx
    da = (-gx * by + gy * bx) / det
    db = (-ax * gy + ay * gx) / det
    # Apply the changes to the layout entries of the closing straights, in order.
    seen = 0
    for item in layout:
        if item[0] == "straight" and len(item) > 2 and item[2] == "close":
            item[1] += da if seen == 0 else db
            if item[1] < 50.0:
                raise ValueError("a closing straight became too short")
            seen += 1
    pieces, corners, _ = pieces_of([tuple(i) for i in layout])
    table, length = integrate(pieces, heading0)
    gap = math.hypot(table[-1][1], table[-1][2])
    if gap > 1e-6:
        raise ValueError(f"the loop does not close ({gap} m)")
    return pieces, corners, table, length


class Path:
    """Interpolation along the centreline table (wrapping modulo the length)."""

    def __init__(self, table: list[tuple[float, float, float, float]], length: float) -> None:
        self.table, self.length = table, length

    def at(self, s: float) -> tuple[float, float, float, float]:
        """(x, y, heading, kappa) at distance s."""
        s %= self.length
        i = min(int(s / TABLE_STEP), len(self.table) - 2)
        while i > 0 and self.table[i][0] > s:
            i -= 1
        while i + 2 < len(self.table) and self.table[i + 1][0] < s:
            i += 1
        s0, x0, y0, k0 = self.table[i]
        s1, x1, y1, k1 = self.table[i + 1]
        u = (s - s0) / (s1 - s0) if s1 > s0 else 0.0
        heading = math.atan2(y1 - y0, x1 - x0)
        return x0 + u * (x1 - x0), y0 + u * (y1 - y0), heading, k0 + u * (k1 - k0)


# ---- the lap -------------------------------------------------------------------------------------------------

def velocity_profile(path: Path) -> tuple[list[float], list[float]]:
    """Speeds every PROFILE_STEP metres and the time at each of those distances (cyclic, from the line)."""
    n = round(path.length / PROFILE_STEP)
    step = path.length / n
    cap = []
    for i in range(n):
        k = abs(path.at(i * step)[3])
        cap.append(min(TOP_SPEED, math.sqrt(LAT_GRIP / k)) if k > 1e-9 else TOP_SPEED)
    v = cap[:]
    for _ in range(3):  # cyclic forward (traction) and backward (braking) passes until settled
        for i in range(1, 2 * n):
            a, b = (i - 1) % n, i % n
            acc = TRACTION * max(0.0, 1.0 - v[a] / TOP_SPEED)
            v[b] = min(v[b], math.sqrt(v[a] ** 2 + 2.0 * acc * step))
        for i in range(2 * n - 1, 0, -1):
            a, b = i % n, (i - 1) % n
            v[b] = min(v[b], math.sqrt(v[a] ** 2 + 2.0 * BRAKE * step))
    times = [0.0]
    for i in range(n):
        times.append(times[-1] + 2.0 * step / (v[i] + v[(i + 1) % n]))
    return v, times


class Lap:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.v, self.times = velocity_profile(path)
        self.n = len(self.v)
        self.step = path.length / self.n
        self.lap_s = self.times[-1]

    def state(self, t: float) -> tuple[float, float]:
        """(distance from the line, speed) at time t since the lap start (any t; laps repeat)."""
        laps = math.floor(t / self.lap_s)
        t -= laps * self.lap_s
        lo, hi = 0, self.n
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self.times[mid] <= t:
                lo = mid
            else:
                hi = mid
        dt = self.times[lo + 1] - self.times[lo]
        u = (t - self.times[lo]) / dt if dt > 0 else 0.0
        v0, v1 = self.v[lo], self.v[(lo + 1) % self.n]
        # Constant acceleration inside the step: s(u) from the two speeds.
        speed = v0 + u * (v1 - v0)
        s = self.step * (lo + (u * v0 + 0.5 * u * u * (v1 - v0)) * dt / self.step)
        return s, speed


# ---- writing -------------------------------------------------------------------------------------------------

def fmt(v: float, dp: int) -> str:
    text = f"{v:.{dp}f}"
    return "0." + "0" * dp if text == "-0." + "0" * dp else text


def build() -> dict[str, bytes]:
    pieces, corners, table, length = closed_circuit()
    path = Path(table, length)
    lap = Lap(path)
    enu = Enu(ORIGIN)
    rng = random.Random(SEED)
    lap_us = round(lap.lap_s * 1e6)
    total_us = lap_us + 2 * PAD_US

    def offset_point(s: float, d: float) -> tuple[float, float]:
        x, y, heading, _ = path.at(s)
        return x - d * math.sin(heading), y + d * math.cos(heading)

    files: dict[str, bytes] = {}

    def lines_csv(d: float) -> bytes:
        rows = ["dist,x,y"]
        n = math.floor(length)
        for i in range(n + 1):
            x, y = offset_point(float(i), d)
            rows.append(f"{fmt(float(i), 3)},{fmt(x, 4)},{fmt(y, 4)}")
        if length - n > 1e-9:  # the closing row at the length itself
            x, y = offset_point(0.0, d)
            rows.append(f"{fmt(length, 3)},{fmt(x, 4)},{fmt(y, 4)}")
        return ("\n".join(rows) + "\n").encode()

    files["expected.centre.csv"] = lines_csv(0.0)
    files["expected.inner.csv"] = lines_csv(TRACK_HALF_WIDTH)   # left of a ccw circuit is inside
    files["expected.outer.csv"] = lines_csv(-TRACK_HALF_WIDTH)

    dropout_corner = next(c for c in corners if c["name"] == DROPOUT_CORNER)
    s_drop = dropout_corner["fullRadiusM"]
    # The lap time at which the car reaches s_drop (bisection on the monotone distance).
    lo, hi = 0.0, lap.lap_s
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        if lap.state(mid)[0] < s_drop:
            lo = mid
        else:
            hi = mid
    drop_start_us = PAD_US + round(hi * 1e6)
    drop_end_us = drop_start_us + round(DROPOUT_S * 1e6)

    def gps_csv(d: float, dropout: bool) -> bytes:
        rows = ["t,lat,lon"]
        period = 1_000_000 // GPS_HZ
        for i in range(total_us // period + 1):
            t_us = i * period
            if t_us >= total_us:
                break
            s, _ = lap.state((t_us - PAD_US) / 1e6)
            x, y = offset_point(s, d)
            x += rng.gauss(0.0, GPS_SIGMA)
            y += rng.gauss(0.0, GPS_SIGMA)
            if dropout and drop_start_us <= t_us < drop_end_us:
                rows.append(f"{t_us},,")
                continue
            lat, lon = enu.to_geodetic(x, y)
            rows.append(f"{t_us},{fmt(lat, 9)},{fmt(lon, 9)}")
        return ("\n".join(rows) + "\n").encode()

    files["lap.gps.csv"] = gps_csv(0.0, True)
    files["inner.gps.csv"] = gps_csv(TRACK_HALF_WIDTH, False)
    files["outer.gps.csv"] = gps_csv(-TRACK_HALF_WIDTH, False)

    rows = ["t,speed,latAccel"]
    period = 1_000_000 // IMU_HZ
    for i in range(total_us // period + 1):
        t_us = i * period
        if t_us >= total_us:
            break
        s, speed = lap.state((t_us - PAD_US) / 1e6)
        kappa = path.at(s)[3]
        logged_speed = speed * SPEED_SCALE + rng.gauss(0.0, SPEED_SIGMA)
        lat_accel = speed * speed * kappa + LAT_BIAS + rng.gauss(0.0, LAT_SIGMA)
        rows.append(f"{t_us},{fmt(logged_speed, 4)},{fmt(lat_accel, 4)}")
    files["lap.imu.csv"] = ("\n".join(rows) + "\n").encode()

    circuit = {
        "generator": {"tool": "tools/gen_track_fixtures.py", "version": VERSION, "seed": SEED},
        "licence": "CC0-1.0",
        "origin": ORIGIN,
        "frame": "ENU metres about origin (local tangent plane of the WGS-84 ellipsoid at origin.alt)",
        "lengthM": round(length, 4),
        "direction": "ccw",
        "headingAtStartDeg": HEADING0_DEG,
        "lap": {"startUs": PAD_US, "endUs": PAD_US + lap_us, "signalEndUs": total_us},
        "corners": [{"name": c["name"], "angleDeg": c["angleDeg"], "radiusM": c["radiusM"],
                     "startM": round(c["startM"], 4), "endM": round(c["endM"], 4),
                     "apexM": round(c["apexM"], 4)} for c in corners if c["kind"] == "corner"],
        "kinks": [{"name": c["name"], "angleDeg": c["angleDeg"], "radiusM": c["radiusM"],
                   "startM": round(c["startM"], 4), "endM": round(c["endM"], 4),
                   "apexM": round(c["apexM"], 4)} for c in corners if c["kind"] == "kink"],
        "lines": {"halfWidthM": TRACK_HALF_WIDTH, "inner": "left (inside of the ccw loop)", "outer": "right"},
        "gps": {"rateHz": GPS_HZ, "noiseSigmaM": GPS_SIGMA, "noise": "white, each ENU axis",
                "dropout": {"startUs": drop_start_us, "endUs": drop_end_us, "file": "lap.gps.csv"}},
        "imu": {"rateHz": IMU_HZ, "speedScale": SPEED_SCALE, "speedSigmaMps": SPEED_SIGMA,
                "latAccelSigmaMps2": LAT_SIGMA, "latAccelBiasMps2": LAT_BIAS,
                "latAccelSign": "positive to the left (ISO 8855)"},
        "profile": {"latGripMps2": LAT_GRIP, "brakeMps2": BRAKE, "tractionMps2": TRACTION,
                    "topSpeedMps": TOP_SPEED, "lapTimeS": round(lap.lap_s, 6)},
    }
    files["circuit.json"] = (json.dumps(circuit, indent=2) + "\n").encode()
    return files


# ---- check ---------------------------------------------------------------------------------------------------

def _tokens(text: str) -> list[str]:
    out, cur = [], ""
    for ch in text:
        if ch.isdigit() or ch in ".-+eE":
            cur += ch
            continue
        if cur:
            out.append(cur)
            cur = ""
        out.append(ch)
    if cur:
        out.append(cur)
    return out


def _number(tok: str) -> float | None:
    try:
        return float(tok)
    except ValueError:
        return None


def same(committed: bytes, fresh: bytes) -> bool:
    """Equal text, numbers within one unit of the committed token's last decimal."""
    if committed == fresh:
        return True
    a, b = _tokens(committed.decode()), _tokens(fresh.decode())
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if x == y:
            continue
        vx, vy = _number(x), _number(y)
        if vx is None or vy is None:
            return False
        dp = len(x.split(".")[1]) if "." in x and "e" not in x.lower() else 0
        if abs(vx - vy) > 1.000001 * 10.0 ** (-dp) + 1e-12 * abs(vx):
            return False
    return True


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", required=True, help="repository root")
    ap.add_argument("--check", action="store_true", help="compare instead of writing")
    a = ap.parse_args(argv)
    d = pathlib.Path(a.root) / "fixtures" / "track" / FAMILY
    differences = 0
    for rel, data in build().items():
        p = d / rel
        if a.check:
            if not p.exists() or not same(p.read_bytes(), data):
                print(f"differs: {p}")
                differences += 1
            continue
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    if not a.check:
        print(f"wrote {d}")
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
