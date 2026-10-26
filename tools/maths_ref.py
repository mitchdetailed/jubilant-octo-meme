#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Independent reference evaluator and generator of the maths case corpus (AC-M2-02, 03 section 14.1, D-041).

The expected values of tests/maths/cases/*.case.json come from here, never from the C++ under test. Every
function below is a plain per-sample F64 loop written from the semantics of docs/design/03_MATHS_ENGINE.md
section 5 and the readings of docs/maths-compat.md, deliberately in a different form from the kernels:

  * windows are brute force (every window summed from scratch; the kernels slide running sums);
  * least-squares slopes are mean-centred two-pass sums per window;
  * IIR filters are designed as one transfer-function polynomial (all poles and zeros multiplied out) and run
    as a direct-form-I difference equation of full order, primed by the total DC gain (the kernels run
    second-order sections in transposed direct form II, primed section by section);
  * inputs are gathered onto the result grid with the exact integer time rules of D-008 / 02 section 3.3.

Logged F32 inputs are rounded to F32 first, as the store holds them. Invalid samples are NaN throughout and
are written as null in the case files.

Usage (standard library only, run with python -I):
  python -I tools/maths_ref.py --write tests/maths/cases   regenerate every case file
  python -I tools/maths_ref.py --check tests/maths/cases   exit 1 when a committed file is missing or differs
                                                           (numbers beyond 1e-9 relative)
  python -I tools/maths_ref.py --self-test                 checks of the reference itself (filter design
                                                           against closed forms, window rules)
  python -I tools/maths_ref.py --list                      the case ids
"""
from __future__ import annotations

import argparse
import cmath
import json
import math
import pathlib
import struct
import sys
from typing import Any, Callable, Optional, Tuple

NaN = float("nan")
SCHEMA = "urn:esses:maths-case-1"


def isnan(v: float) -> bool:
    return v != v


def f32(v: float) -> float:
    """v rounded to the nearest F32 (NaN stays NaN)."""
    if isnan(v):
        return v
    return struct.unpack("<f", struct.pack("<f", v))[0]


def round_half_away(v: float) -> int:
    return int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))


# ---- time grid (D-008, D-129) ------------------------------------------------------------------------------
Rate = Tuple[int, int]  # (num, den) hertz; typing.Tuple: a subscripted builtin tuple needs Python 3.9


def hz(rate: Rate) -> float:
    return float(rate[0]) / float(rate[1])


def time_at(i: int, rate: Rate, t0: int = 0) -> int:
    num, den = rate
    d = 1_000_000 * den
    return t0 + (2 * i * d + num) // (2 * num)


def index_at(t: int, rate: Rate, t0: int = 0) -> int:
    num, den = rate
    d = 1_000_000 * den
    return (num * (2 * (t - t0) + 1) - 1) // (2 * d)


def count_for(duration_us: int, rate: Rate) -> int:
    return index_at(duration_us - 1, rate) + 1 if duration_us > 0 else 0


class Signal:
    """A uniform input channel: values (NaN = invalid) at `rate` from t = 0."""

    def __init__(self, values: list[float], rate: Rate, interp: str = "linear", dtype: str = "f32",
                 unit: str = "", valid: Optional[list[int]] = None):
        self.raw = list(values)
        self.valid = valid
        self.rate = rate
        self.interp = interp
        self.dtype = dtype
        self.unit = unit
        vals = []
        for k, v in enumerate(values):
            if valid is not None and not valid[k]:
                vals.append(NaN)
            elif dtype == "f32":
                vals.append(f32(float(v)))
            else:
                vals.append(float(v))
        self.values = vals

    def duration(self) -> int:
        return time_at(len(self.values), self.rate)

    def gather(self, out_rate: Rate, count: int, interp: Optional[str] = None) -> list[float]:
        """The channel at the output grid (02 section 3.3): outside [t(0), t(n-1)] invalid."""
        mode = interp or self.interp
        n = len(self.values)
        out = []
        for i in range(count):
            t = time_at(i, out_rate)
            if n == 0 or t < time_at(0, self.rate) or t > time_at(n - 1, self.rate):
                out.append(NaN)
                continue
            prev = min(max(index_at(t, self.rate), 0), n - 1)
            nxt = min(max(index_at(t - 1, self.rate) + 1, 0), n - 1)
            tp = time_at(prev, self.rate)
            tn = time_at(nxt, self.rate)
            if mode == "previous":
                out.append(self.values[prev])
            elif mode == "next":
                out.append(self.values[nxt])
            elif mode == "closest":
                if tn == t:
                    out.append(self.values[nxt])
                else:
                    out.append(self.values[prev] if (t - tp) <= (tn - t) else self.values[nxt])
            else:
                if tp == t:
                    out.append(self.values[prev])
                    continue
                va, vb = self.values[prev], self.values[nxt]
                if isnan(va) or isnan(vb):
                    out.append(NaN)
                elif tn == tp:
                    out.append(va)
                else:
                    w = float(t - tp) / float(tn - tp)
                    out.append(va + (vb - va) * w)
        return out


# ---- windows (03 sections 2.4 rule 2, 5.2) ------------------------------------------------------------------
def window_points(spec: Optional[tuple[str, float]], rate: Rate) -> int:
    if spec is None:
        return 2
    kind, v = spec
    if kind == "seconds":
        samples = v * hz(rate)
        n = 2.0 * math.floor(samples / 2.0 + 1e-9) + 1.0
    else:
        n = float(v)
    return 2 if n < 2 else int(n)


def window_bounds(i: int, n_points: int, size: int) -> tuple[int, int]:
    if n_points % 2 == 1:
        before = after = (n_points - 1) // 2
    else:
        before, after = n_points // 2, n_points // 2 - 1
    return max(0, i - before), min(size - 1, i + after)


def smooth(x: list[float], spec: tuple[str, float], rate: Rate) -> list[float]:
    n = window_points(spec, rate)
    out = []
    for i in range(len(x)):
        a, b = window_bounds(i, n, len(x))
        vals = [x[j] for j in range(a, b + 1) if not isnan(x[j])]
        out.append(sum(vals) / len(vals) if vals else NaN)
    return out


def ls_slope(points: list[tuple[float, float]]) -> float:
    if len(points) < 2:
        return NaN
    mu = sum(p[0] for p in points) / len(points)
    mv = sum(p[1] for p in points) / len(points)
    suu = sum((p[0] - mu) ** 2 for p in points)
    suv = sum((p[0] - mu) * (p[1] - mv) for p in points)
    return suv / suu if suu > 0 else NaN


def derivative(x: list[float], rate: Rate, spec: Optional[tuple[str, float]] = None,
               y: Optional[list[float]] = None) -> list[float]:
    n = window_points(spec, rate)
    out = []
    for i in range(len(x)):
        a, b = window_bounds(i, n, len(x))
        if y is None:
            pts = [(j / hz(rate), x[j]) for j in range(a, b + 1) if not isnan(x[j])]
        else:
            pts = [(y[j], x[j]) for j in range(a, b + 1) if not isnan(x[j]) and not isnan(y[j])]
        out.append(ls_slope(pts))
    return out


# ---- IIR (03 section 5.3) -----------------------------------------------------------------------------------
GAP_SECONDS = 1.0


def poly_mul(a: list[complex], b: list[complex]) -> list[complex]:
    out = [0j] * (len(a) + len(b) - 1)
    for i, ca in enumerate(a):
        for j, cb in enumerate(b):
            out[i + j] += ca * cb
    return out


def poly_from_roots(roots: list[complex]) -> list[float]:
    """Coefficients of prod (1 - r q) in powers of q = z^-1."""
    p: list[complex] = [1 + 0j]
    for r in roots:
        p = poly_mul(p, [1 + 0j, -r])
    return [c.real for c in p]


def poly_eval(c: list[float], q: complex) -> complex:
    return sum(ck * q ** k for k, ck in enumerate(c))


class TransferFunction:
    def __init__(self, b: list[float], a: list[float]):
        self.b = b
        self.a = a

    def dc_gain(self) -> float:
        return sum(self.b) / sum(self.a)

    def response(self, w: float) -> complex:
        q = cmath.exp(-1j * w)
        return poly_eval(self.b, q) / poly_eval(self.a, q)


def rc_transfer(high: bool, fc: float, fs: float) -> TransferFunction:
    rc = 1.0 / (2.0 * math.pi * fc)
    dt = 1.0 / fs
    if not high:
        a = dt / (rc + dt)
        return TransferFunction([a], [1.0, -(1.0 - a)])
    b = rc / (rc + dt)
    return TransferFunction([b, -b], [1.0, -b])


def prototype_poles(order: int, ripple: float) -> list[complex]:
    """Every left-half-plane pole of the normalised low-pass prototype."""
    if ripple <= 0:
        return [cmath.exp(1j * math.pi * (2 * k + order - 1) / (2 * order)) for k in range(1, order + 1)]
    eps = math.sqrt((1.0 + ripple / 100.0) ** 2 - 1.0)
    mu = math.asinh(1.0 / eps) / order
    return [complex(-math.sinh(mu) * math.sin(math.pi * (2 * k - 1) / (2 * order)),
                    math.cosh(mu) * math.cos(math.pi * (2 * k - 1) / (2 * order))) for k in range(1, order + 1)]


def bilinear(s: complex) -> complex:
    return (1 + s) / (1 - s)


def cheby_transfer(band: str, order: int, ripple: float, f_lo: float, f_hi: float, fs: float) -> TransferFunction:
    w_lo = math.tan(math.pi * f_lo / fs)
    w_hi = math.tan(math.pi * f_hi / fs)
    proto = prototype_poles(order, ripple)
    poles: list[complex] = []
    zeros: list[complex] = []
    if band == "lp":
        poles = [bilinear(w_lo * p) for p in proto]
        zeros = [-1 + 0j] * order
        ref_w = 0.0
    elif band == "hp":
        poles = [bilinear(w_lo / p) for p in proto]
        zeros = [1 + 0j] * order
        ref_w = math.pi
    else:
        w0 = math.sqrt(w_lo * w_hi)
        bw = w_hi - w_lo
        omega0 = 2.0 * math.atan(w0)
        for p in proto:
            c = p * bw if band == "bp" else bw / p
            root = cmath.sqrt(c * c - 4 * w0 * w0)
            poles += [bilinear((c + root) / 2), bilinear((c - root) / 2)]
        if band == "bp":
            zeros = [1 + 0j] * order + [-1 + 0j] * order
            ref_w = omega0
        else:
            zeros = [cmath.exp(1j * omega0)] * order + [cmath.exp(-1j * omega0)] * order
            ref_w = 0.0
    tf = TransferFunction(poly_from_roots(zeros), poly_from_roots(poles))
    g = abs(tf.response(ref_w))
    tf.b = [c / g for c in tf.b]
    return tf


def run_pass(x: list[float], tf: TransferFunction, fs: float, backward: bool) -> list[float]:
    """Direct form I of the whole transfer function; NaN passes through; primed by the DC gain."""
    n = len(x)
    out = list(x)
    order = range(n - 1, -1, -1) if backward else range(n)
    xs: list[float] = []  # previous inputs, most recent first
    ys: list[float] = []
    primed = False
    gap = 0
    nb, na = len(tf.b), len(tf.a)
    for i in order:
        v = x[i]
        if isnan(v):
            gap += 1
            continue
        if not primed or gap > GAP_SECONDS * fs:
            g = tf.dc_gain()
            xs = [v] * nb
            ys = [g * v] * na
            primed = True
        gap = 0
        y = tf.b[0] * v
        for k in range(1, nb):
            y += tf.b[k] * xs[k - 1]
        for k in range(1, na):
            y -= tf.a[k] * ys[k - 1]
        y /= tf.a[0]
        xs = [v] + xs[:-1]
        ys = [y] + ys[:-1]
        out[i] = y
    return out


def filter_rc(x: list[float], high: bool, fc: float, fs: float, order: int = 1) -> list[float]:
    tf = rc_transfer(high, fc, fs)
    if order > 0:
        for _ in range(order):
            x = run_pass(x, tf, fs, False)
        return x
    if order == -1:
        return run_pass(x, tf, fs, True)
    return run_pass(run_pass(x, tf, fs, False), tf, fs, True)


def filter_cheby(x: list[float], band: str, fs: float, f_lo: float, f_hi: float = 0.0, order: int = 2,
                 ripple: float = 0.0) -> list[float]:
    each = -order // 2 if order < 0 else order
    tf = cheby_transfer(band, each, ripple, f_lo, f_hi if band in ("bp", "bs") else f_lo, fs)
    y = run_pass(x, tf, fs, False)
    if order < 0:
        y = run_pass(y, tf, fs, True)
    return y


# ---- Stateful (03 section 5.4) ------------------------------------------------------------------------------
def truthy(v: float) -> bool:
    return not isnan(v) and v != 0.0


def segment_starts(reset: Optional[list[float]], n: int) -> list[bool]:
    starts = [False] * n
    if n:
        starts[0] = True
    if reset is None:
        return starts
    prev: Optional[float] = None
    for i, r in enumerate(reset):
        if isnan(r):
            continue
        whole = float(math.trunc(r))
        if prev is not None and whole > prev:
            starts[i] = True
        prev = whole
    return starts


def integrate(x: list[float], rate: Rate, cond: Optional[list[float]] = None,
              reset: Optional[list[float]] = None, dist: Optional[list[float]] = None) -> list[float]:
    starts = segment_starts(reset, len(x))
    dt = 1.0 / hz(rate)
    acc = 0.0
    last_d = NaN
    out = []
    for i, v in enumerate(x):
        if starts[i]:
            acc = 0.0
        step = dt
        if dist is not None:
            d = dist[i]
            step = 0.0
            if not isnan(d):
                step = 0.0 if isnan(last_d) else d - last_d
                last_d = d
        if (cond is None or truthy(cond[i])) and not isnan(v):
            acc += v * step
        out.append(acc)
    return out


def statistic(kind: str, vals: list[float]) -> float:
    if not vals:
        return NaN
    if kind == "min":
        return min(vals)
    if kind == "max":
        return max(vals)
    if kind == "mean":
        return sum(vals) / len(vals)
    if kind == "start":
        return vals[0]
    if kind == "end":
        return vals[-1]
    mean = sum(vals) / len(vals)
    return math.sqrt(sum((v - mean) ** 2 for v in vals) / len(vals))


def stat(kind: str, x: list[float], times: list[int], cond: Optional[list[float]] = None,
         reset: Optional[list[float]] = None, window: Optional[tuple[int, int]] = None) -> list[float]:
    n = len(x)
    gate = [cond is None or truthy(cond[i]) for i in range(n)]
    if reset is None:
        lo, hi = window if window is not None and window[1] > window[0] else (None, None)
        vals = [x[i] for i in range(n)
                if gate[i] and not isnan(x[i]) and (lo is None or lo <= times[i] < hi)]
        return [statistic(kind, vals)] * n
    starts = segment_starts(reset, n)
    out = [NaN] * n
    a = 0
    while a < n:
        b = a + 1
        while b < n and not starts[b]:
            b += 1
        v = statistic(kind, [x[i] for i in range(a, b) if gate[i] and not isnan(x[i])])
        for i in range(a, b):
            out[i] = v
        a = b
    return out


def running(kind: str, x: list[float], cond: Optional[list[float]] = None,
            reset: Optional[list[float]] = None) -> list[float]:
    starts = segment_starts(reset, len(x))
    cur = NaN
    out = []
    for i, v in enumerate(x):
        if starts[i]:
            cur = NaN
        if (cond is None or truthy(cond[i])) and not isnan(v):
            cur = v if isnan(cur) else (max(cur, v) if kind == "max" else min(cur, v))
        out.append(cur)
    return out


def flip_flop(s: list[float], r: list[float]) -> list[float]:
    state, ps, pr = False, False, False
    out = []
    for a, b in zip(s, r):
        ts, tr = truthy(a), truthy(b)
        if tr and not pr:
            state = False
        elif ts and not ps:
            state = True
        ps, pr = ts, tr
        out.append(1.0 if state else 0.0)
    return out


def delay_samples(seconds: float, rate: Rate) -> int:
    s = seconds * hz(rate) - 1e-9
    return 0 if s <= 0 else int(math.ceil(s))


def edge_delay(x: list[float], seconds: float, rate: Rate, rise: bool) -> list[float]:
    m = delay_samples(seconds, rate)
    out = []
    for i in range(len(x)):
        window = [truthy(x[j]) if j >= 0 else False for j in range(i - m, i + 1)]
        out.append(1.0 if (all(window) if rise else any(window)) else 0.0)
    return out


def time_shift(x: list[float], seconds: float, rate: Rate) -> list[float]:
    s = round_half_away(seconds * hz(rate))
    return [x[i - s] if 0 <= i - s < len(x) else NaN for i in range(len(x))]


def pulse_width(x: list[float], rate: Rate) -> list[float]:
    out, width, prev, rise = [], NaN, False, 0
    for i, v in enumerate(x):
        t = truthy(v)
        if t and not prev:
            rise = i
        elif not t and prev:
            width = (i - rise) / hz(rate)
        prev = t
        out.append(width)
    return out


def previous_sample(x: list[float], n: int = 1) -> list[float]:
    return [x[i - n] if i >= n else NaN for i in range(len(x))]


# ---- Range (03 section 5.5) ---------------------------------------------------------------------------------
def range_is(times: list[int], intervals: list[tuple[int, int]]) -> list[float]:
    return [1.0 if any(a <= t < b for a, b in intervals) else 0.0 for t in times]


def range_change(times: list[int], intervals: list[tuple[int, int]]) -> list[float]:
    return [float(sum(1 for a, _ in intervals if a <= t)) for t in times]


# ---- Table (03 section 4.4) ---------------------------------------------------------------------------------
def table(x: list[float], bp: list[float], vals: list[float], interp: str = "linear",
          oor: str = "clamp") -> list[float]:
    out = []
    for v in x:
        if isnan(v):
            out.append(NaN)
            continue
        if v < bp[0] or v > bp[-1]:
            out.append(NaN if oor == "invalid" else (vals[0] if v < bp[0] else vals[-1]))
            continue
        if interp == "next":
            out.append(next(vals[k] for k in range(len(bp)) if bp[k] >= v))
        elif interp == "previous":
            out.append([vals[k] for k in range(len(bp)) if bp[k] <= v][-1])
        else:
            k = max(k for k in range(len(bp) - 1) if bp[k] <= v)
            w = (v - bp[k]) / (bp[k + 1] - bp[k])
            out.append(vals[k] + w * (vals[k + 1] - vals[k]))
    return out


# ---- Variance (03 section 5.6) ------------------------------------------------------------------------------
def grid_at(x: list[float], rate: Rate, t: int) -> float:
    j = index_at(t, rate)
    if j < 0 or j >= len(x):
        return NaN
    tj = time_at(j, rate)
    if tj == t:
        return x[j]
    if j + 1 >= len(x):
        return NaN
    w = (t - tj) / (time_at(j + 1, rate) - tj)
    return x[j] + w * (x[j + 1] - x[j])


def lerp_at(times: list[int], vals: list[float], t: float) -> float:
    if t <= times[0]:
        return vals[0]
    if t >= times[-1]:
        return vals[-1]
    k = max(i for i in range(len(times) - 1) if times[i] <= t)
    w = (t - times[k]) / (times[k + 1] - times[k])
    return vals[k] + w * (vals[k + 1] - vals[k])


class LapDistance:
    """distAt/timeAt of one lap over a strictly increasing Corr Dist (03 section 5.6, D-016)."""

    def __init__(self, times: list[int], dist: list[float], start: int, end: int):
        self.start, self.end = start, end
        d0 = lerp_at(times, dist, start)
        self.t = [start] + [t for t in times if start < t < end] + [end]
        self.d = [0.0] + [dist[i] - d0 for i, t in enumerate(times) if start < t < end] + \
                 [lerp_at(times, dist, end) - d0]

    def dist_at(self, t: int) -> float:
        return lerp_at(self.t, self.d, t)

    def length(self) -> float:
        return max(self.d)

    def time_at(self, d: float) -> float:  # seconds since the lap start
        for k in range(1, len(self.d)):
            if self.d[k] >= d:
                f = (d - self.d[k - 1]) / (self.d[k] - self.d[k - 1])
                return ((self.t[k - 1] - self.start) + f * (self.t[k] - self.t[k - 1])) / 1e6
        return (self.t[-1] - self.start) / 1e6


def variance(x: list[float], rate: Rate, main: dict, ref: Optional[dict], by_distance: bool,
             main_dist: Optional[LapDistance] = None, ref_dist: Optional[LapDistance] = None) -> list[float]:
    out = []
    for i, v in enumerate(x):
        t = time_at(i, rate)
        r = NaN
        if ref is not None and main["start"] <= t < main["end"] and (not by_distance or (main_dist and ref_dist)):
            if by_distance:
                c = main_dist.dist_at(t) * main.get("stretch", 1.0) + main.get("offsetM", 0.0)
                d = (c - ref.get("offsetM", 0.0)) / ref.get("stretch", 1.0)
                t_ref = None if d < 0 or d > ref_dist.length() else \
                    ref["start"] + round_half_away(ref_dist.time_at(d) * 1e6)
            else:
                c = (t - main["start"]) + main.get("offsetUs", 0)
                t_ref = ref["start"] + c - ref.get("offsetUs", 0)
            if t_ref is not None and ref["start"] <= t_ref < ref["end"]:
                r = v - grid_at(x, rate, t_ref)
        out.append(r)
    return out


# ---- case corpus ---------------------------------------------------------------------------------------------
class Env:
    """What an expectation function sees: the inputs on the result grid, times, ranges, selection."""

    def __init__(self, case: "Case", rate: Rate, count: int):
        self.case = case
        self.rate = rate
        self.n = count
        self.t = [time_at(i, rate) for i in range(count)]

    def x(self, name: str, interp: Optional[str] = None) -> list[float]:
        return self.case.inputs[name].gather(self.rate, self.n, interp)

    def ranges(self, path: str) -> list[tuple[int, int]]:
        return sorted(self.case.ranges[path])

    def sel(self, which: str) -> Optional[dict]:
        return self.case.selection_geom(which)

    def const(self, values: list[float] | float) -> list[float]:
        return list(values) if isinstance(values, list) else [values] * self.n

    def ew(self, fn: Callable[..., float], *cols: list[float]) -> list[float]:
        """fn per sample; NaN in any operand gives NaN unless fn handles it (pass raw=True cols)."""
        out = []
        for k in range(self.n):
            args = [c[k] for c in cols]
            if any(isnan(a) for a in args):
                out.append(NaN)
                continue
            try:
                v = fn(*args)
            except (ValueError, ZeroDivisionError, OverflowError):
                v = NaN
            out.append(float(v))
        return out


class Case:
    def __init__(self, cid: str, expression: str, *, inputs: dict[str, Signal],
                 expect: Optional[Callable[[Env], list[float]]] = None, ref: str, note: str,
                 kind: str = "derived", rate: Optional[Rate] = None, compat: Optional[dict] = None,
                 precision: Optional[str] = None, result_unit: Optional[str] = None,
                 constants: Optional[dict] = None, details: Optional[dict] = None,
                 ranges: Optional[dict[str, list[tuple[int, int]]]] = None, selection: Optional[dict] = None,
                 duration: Optional[int] = None, item: Optional[dict] = None, tol: tuple[float, float] = (1e-6, 1e-6),
                 diagnostics: Optional[list[tuple[str, int]]] = None, error: Optional[str] = None):
        self.id = cid
        self.expression = expression
        self.inputs = inputs
        self.expect = expect
        self.ref = ref
        self.note = note
        self.kind = kind
        self.rate = rate
        self.compat = compat or {}
        self.precision = precision
        self.result_unit = result_unit
        self.constants = constants or {}
        self.details = details or {}
        self.ranges = ranges or {}
        self.selection = selection or {}
        self.duration = duration
        self.item = item
        self.tol = tol
        self.diagnostics = diagnostics or []
        self.error = error

    def selection_geom(self, which: str) -> Optional[dict]:
        s = self.selection.get(which)
        if s is None:
            return None
        spec = {"range": s} if isinstance(s, str) else dict(s)
        path, _, num = spec["range"].rpartition("/")
        a, b = sorted(self.ranges[path])[int(num) - 1]
        geom = {"start": a, "end": b}
        for k in ("offsetUs", "offsetM", "stretch"):
            if k in spec:
                geom[k] = spec[k]
        return geom

    def layer_duration(self) -> int:
        if self.duration is not None:
            return self.duration
        return max(s.duration() for s in self.inputs.values())

    def result_rate(self) -> Rate:
        if self.rate is not None:
            return self.rate
        return max((s.rate for s in self.inputs.values()), key=lambda r: r[0] / r[1])

    def to_json(self) -> dict[str, Any]:
        doc: dict[str, Any] = {"$schema": SCHEMA, "schemaVersion": 1, "id": self.id, "expression": self.expression}
        if self.item is not None:
            doc["item"] = self.item
        doc["provenance"] = {"kind": self.kind, "ref": self.ref, "note": self.note}
        doc["rate"] = "auto" if self.rate is None else {"num": self.rate[0], "den": self.rate[1]}
        doc["compat"] = self.compat
        doc["precision"] = self.precision or "single"
        if self.result_unit is not None:
            doc["resultUnit"] = self.result_unit
        doc["durationUs"] = self.layer_duration()
        inputs = {}
        for name, s in self.inputs.items():
            j: dict[str, Any] = {"rate": {"num": s.rate[0], "den": s.rate[1]}, "unit": s.unit,
                                 "values": [num(v) for v in s.raw]}
            if s.valid is not None:
                j["valid"] = list(s.valid)
            if s.interp != "linear":
                j["interp"] = s.interp
            if s.dtype != "f32":
                j["dtype"] = s.dtype
            inputs[name] = j
        doc["inputs"] = inputs
        if self.constants:
            doc["constants"] = {k: {"v": num(v), "unit": u} for k, (v, u) in self.constants.items()}
        if self.details:
            det = {}
            for k, v in self.details.items():
                det[k] = {"text": v} if isinstance(v, str) else {"v": num(v[0]), "unit": v[1]}
            doc["details"] = det
        if self.ranges:
            doc["ranges"] = {p: [{"startUs": a, "endUs": b} for a, b in iv] for p, iv in self.ranges.items()}
        if self.selection:
            doc["selection"] = self.selection
        expected: dict[str, Any] = {}
        rate = self.result_rate()
        if self.error is not None:
            expected["error"] = {"code": self.error}
        else:
            count = count_for(self.layer_duration(), rate)
            values = self.expect(Env(self, rate, count))
            if len(values) != count:
                raise ValueError(f"{self.id}: {len(values)} expected values for {count} samples")
            expected["rate"] = {"num": rate[0], "den": rate[1]}
            expected["values"] = [None if isnan(v) else num(v, 12) for v in values]
            expected["tolerance"] = {"abs": self.tol[0], "rel": self.tol[1]}
        if self.diagnostics:
            expected["diagnostics"] = [{"code": c, "count": n} for c, n in self.diagnostics]
        doc["expected"] = expected
        return doc


def num(v: float, digits: int = 17) -> Any:
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        return v
    if isnan(v) or math.isinf(v):
        raise ValueError("not a finite number")
    if v == math.trunc(v) and abs(v) < 1e15:
        return int(v)
    return float(f"{v:.{digits}g}")


def dumps(doc: Any, indent: int = 0) -> str:
    """Compact canonical JSON: objects one member per line, arrays of scalars on one line."""
    pad = "  " * indent
    if isinstance(doc, dict):
        if not doc:
            return "{}"
        items = [f'{pad}  {json.dumps(k, ensure_ascii=False)}: {dumps(v, indent + 1)}' for k, v in doc.items()]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if isinstance(doc, list):
        if all(not isinstance(v, (dict, list)) for v in doc):
            return "[" + ", ".join(dumps(v) for v in doc) + "]"
        items = [pad + "  " + dumps(v, indent + 1) for v in doc]
        return "[\n" + ",\n".join(items) + "\n" + pad + "]"
    if isinstance(doc, float):
        return repr(doc)
    return json.dumps(doc, ensure_ascii=False)


# ---- the corpus ----------------------------------------------------------------------------------------------
R10: Rate = (10, 1)
R20: Rate = (20, 1)
R5: Rate = (5, 1)
R100: Rate = (100, 1)

HELP_FN = "Maths Functions (Pro)"
HELP_EXPR = "Maths Expressions (Pro)"
HELP_OPS = "Maths Operators (Pro)"
HELP_TABLES = "Maths Tables (Pro)"
DESIGN = "03_MATHS_ENGINE.md"


def sig(values: list[float], rate: Rate = R10, **kw: Any) -> Signal:
    return Signal(values, rate, **kw)


X8 = [0, 1, 2, 3, 4, 5, 6, 7]
W12 = [3.0, 1.5, 4.25, 1.0, 5.5, 9.0, 2.75, 6.5, 5.25, 3.5, 5.0, 8.75]
SIGNED = [-2.5, -1.5, -0.5, 0.5, 1.5, 2.5, -3.75, 3.75]


def wave(n: int, rate: Rate, step_at: int = -1) -> list[float]:
    f = hz(rate)
    return [math.sin(2 * math.pi * 1.5 * i / f) + 0.4 * math.sin(2 * math.pi * 7.0 * i / f) +
            (1.0 if 0 <= step_at <= i else 0.0) for i in range(n)]


def elementwise_cases() -> list[Case]:
    cs: list[Case] = []
    X = {"X": sig(SIGNED)}
    P = {"X": sig([0.25, 0.5, 1.0, 2.0, 4.0, 9.0, 16.0, 100.0])}
    U = {"X": sig([-1.0, -0.75, -0.5, 0.0, 0.25, 0.5, 0.875, 1.0])}

    def ew(cid: str, expr: str, inputs: dict, fn: Callable, note: str, **kw: Any) -> None:
        names = list(inputs)
        cs.append(Case(cid, expr, inputs=inputs, ref=f"{HELP_FN}: {kw.pop('fname', cid.split('_')[0])}",
                       note=note, expect=lambda e: e.ew(fn, *[e.x(k) for k in names]), **kw))

    ew("abs_signed", "abs('X')", X, abs, "|x|")
    ew("sqr_signed", "sqr('X')", X, lambda v: v * v, "x squared")
    ew("sqrt_positive", "sqrt('X')", P, math.sqrt, "square root")
    ew("sqrt_negative_invalid", "sqrt('X')", X, lambda v: math.sqrt(v) if v >= 0 else NaN,
       "a negative argument gives an invalid sample (sqrt.domain)", fname="sqrt")
    ew("power_series_exponent", "power('X', 'Y')",
       {"X": sig([2, 3, 4, 5, 2, 9, 0.5, 10]), "Y": sig([2, 2, 0.5, -1, 10, 0.5, 3, 0])},
       lambda a, b: a ** b, "y may be a channel (release note 1.0.21)", fname="power")
    ew("power_literal", "power('X', 3)", P, lambda v: v ** 3, "integer literal exponent", fname="power")
    ew("hypot_pair", "hypot('X', 'Y')", {"X": sig([3, 5, 8, -6, 0, 1.5, 7, 20]),
                                          "Y": sig([4, 12, 15, 8, 0, 2, -24, 21])},
       math.hypot, "sqrt(x^2 + y^2)")
    ew("exp_signed", "exp('X')", X, math.exp, "e to the x")
    ew("exp10_signed", "exp10('X')", X, lambda v: 10.0 ** v, "10 to the x")
    ew("ln_positive", "ln('X')", P, math.log, "natural logarithm")
    ew("ln_nonpositive_invalid", "ln('X')", U, lambda v: math.log(v) if v > 0 else NaN,
       "x <= 0 gives an invalid sample", fname="ln")
    ew("log_positive", "log('X')", P, math.log10, "base-10 logarithm")
    ew("int_truncates", "int('X')", X, lambda v: float(math.trunc(v)), "truncate toward zero")
    ew("frac_signed", "frac('X')", X, lambda v: v - math.trunc(v), "x - int(x), sign of x")
    ew("round_half_away", "round('X')", X, lambda v: float(round_half_away(v)),
       "halves away from zero (round.half)")
    ew("round_down_signed", "round_down('X')", X, lambda v: float(math.floor(v)), "floor")
    ew("round_up_signed", "round_up('X')", X, lambda v: float(math.ceil(v)), "ceiling")
    ew("remainder_sign_of_x", "remainder('X', 'Y')",
       {"X": sig([7, -7, 7, -7, 5.5, 3, 0, 9]), "Y": sig([3, 3, -3, -3, 2, 0, 4, 2.5])},
       lambda a, b: math.fmod(a, b) if b != 0 else NaN, "f = x - i*y with the sign of x; y = 0 invalid")
    ew("sgn_signed", "sgn('X')", X, lambda v: float((v > 0) - (v < 0)), "-1, 0, +1")
    ew("sgn_zero", "sgn('X')", {"X": sig([0, -0.0, 3, -3, 0, 1e-3, -1e-3, 0])},
       lambda v: float((v > 0) - (v < 0)), "sgn(0) = 0", fname="sgn")
    ew("min_pair", "min('X', 'Y')", {"X": sig(X8), "Y": sig([7, 6, 5, 4, 3, 2, 1, 0])}, min, "elementwise")
    ew("max_pair", "max('X', 'Y')", {"X": sig(X8), "Y": sig([7, 6, 5, 4, 3, 2, 1, 0])}, max, "elementwise")
    cs.append(Case("min_one_invalid", "min('X', 'Y')",
                   inputs={"X": sig(X8, valid=[1, 0, 1, 1, 0, 1, 1, 1]), "Y": sig([7, 6, 5, 4, 3, 2, 1, 0],
                                                                                   valid=[1, 1, 1, 0, 0, 1, 1, 1])},
                   ref=f"{HELP_FN}: min", note="exactly one invalid operand: the other one (minmax.invalid)",
                   expect=lambda e: [b if isnan(a) else (a if isnan(b) else min(a, b))
                                     for a, b in zip(e.x("X"), e.x("Y"))]))
    cs.append(Case("max_one_invalid", "max('X', 'Y')",
                   inputs={"X": sig(X8, valid=[1, 0, 1, 1, 0, 1, 1, 1]), "Y": sig([7, 6, 5, 4, 3, 2, 1, 0],
                                                                                   valid=[1, 1, 1, 0, 0, 1, 1, 1])},
                   ref=f"{HELP_FN}: max", note="exactly one invalid operand: the other one (minmax.invalid)",
                   expect=lambda e: [b if isnan(a) else (a if isnan(b) else max(a, b))
                                     for a, b in zip(e.x("X"), e.x("Y"))]))
    cs.append(Case("rand_val_unit_interval", "rand_val() >= 0 && rand_val() < 1 && 'X' == 'X'", inputs=X,
                   ref=f"{HELP_FN}: rand_val", note="one value in [0, 1) per evaluation (D-087)",
                   expect=lambda e: [1.0] * e.n))
    cs.append(Case("rand_chan_unit_interval", "rand_chan() >= 0 && rand_chan() < 1", inputs=X,
                   ref=f"{HELP_FN}: rand_chan", note="a value in [0, 1) per sample (D-087)",
                   expect=lambda e: [1.0] * e.n))
    ew("sin_radians", "sin('X')", X, math.sin, "radians")
    ew("cos_radians", "cos('X')", X, math.cos, "radians")
    ew("tan_radians", "tan('X')", X, math.tan, "radians")
    ew("asin_domain", "asin('X')", {"X": sig([-1.5, -1, -0.5, 0, 0.5, 1, 1.5, 0.25])},
       lambda v: math.asin(v) if -1 <= v <= 1 else NaN, "outside [-1, 1] invalid")
    ew("acos_domain", "acos('X')", {"X": sig([-1.5, -1, -0.5, 0, 0.5, 1, 1.5, 0.25])},
       lambda v: math.acos(v) if -1 <= v <= 1 else NaN, "outside [-1, 1] invalid")
    ew("atan_signed", "atan('X')", X, math.atan, "radians")
    ew("atan2_quadrants", "atan2('Y', 'X')", {"Y": sig([1, 1, -1, -1, 0, 2, 0, -3]),
                                              "X": sig([1, -1, -1, 1, -2, 0, 3, 0])},
       math.atan2, "four-quadrant (release note 1.0.20)")
    ew("sinh_signed", "sinh('X')", X, math.sinh, "hyperbolic")
    ew("cosh_signed", "cosh('X')", X, math.cosh, "hyperbolic")
    ew("tanh_signed", "tanh('X')", X, math.tanh, "hyperbolic")
    ew("asinh_signed", "asinh('X')", X, math.asinh, "inverse hyperbolic")
    ew("acosh_domain", "acosh('X')", {"X": sig([0.5, 1, 1.5, 2, 4, -1, 10, 0.99])},
       lambda v: math.acosh(v) if v >= 1 else NaN, "x < 1 invalid")
    ew("atanh_domain", "atanh('X')", U, lambda v: math.atanh(v) if -1 < v < 1 else NaN, "|x| >= 1 invalid")
    I8 = {"A": sig([12, 10, 255, 6, 0, 7, 1023, 5], dtype="i32"), "B": sig([10, 6, 15, 3, 9, 7, 512, 2], dtype="i32")}
    ew("bit_and_ints", "bit_and('A', 'B')", I8, lambda a, b: float(int(a) & int(b)), "integer AND")
    ew("bit_or_ints", "bit_or('A', 'B')", I8, lambda a, b: float(int(a) | int(b)), "integer OR")
    ew("bit_xor_ints", "bit_xor('A', 'B')", I8, lambda a, b: float(int(a) ^ int(b)), "integer XOR (1.1.4 fix)")
    ew("bit_not_32bit", "bit_not('A')", I8,
       lambda a, b=0: float(struct.unpack("<i", struct.pack("<I", (~int(a)) & 0xFFFFFFFF))[0]),
       "32-bit complement (bitwise.not)")
    ew("bit_decode_one_bit", "bit_decode('A', 2)", I8, lambda a, b=0: float((int(a) >> 2) & 1),
       "(x >> bit) & 1 (bit_decode.arity)", fname="bit_decode")
    ew("bit_decode_field", "bit_decode('A', 1, 3)", I8, lambda a, b=0: float((int(a) >> 1) & 7),
       "(x >> low) & (2^count - 1) (bit_decode.arity)", fname="bit_decode")
    cs.append(Case("choose_branches", "choose('X' > 0, 'X', -'X' * 10)", inputs=X, ref=f"{HELP_FN}: choose",
                   note="per sample", expect=lambda e: [v if v > 0 else -v * 10 for v in e.x("X")]))
    cs.append(Case("choose_nested", "choose('X' < -1, -1, choose('X' > 1, 1, 0))", inputs=X,
                   ref=f"{HELP_FN}: choose", note="nestable",
                   expect=lambda e: [-1.0 if v < -1 else (1.0 if v > 1 else 0.0) for v in e.x("X")]))
    cs.append(Case("choose_invalid_condition", "choose('C', 1, 2)",
                   inputs={"C": sig([1, 0, 1, 0, 3, 0, 1, 0], valid=[1, 1, 0, 1, 1, 0, 1, 1])},
                   ref=f"{HELP_FN}: choose", note="an invalid condition gives an invalid sample",
                   expect=lambda e: [NaN if isnan(c) else (1.0 if c != 0 else 2.0) for c in e.x("C")]))
    cs.append(Case("choose_chosen_branch_validity", "choose('C', 'A', 'B')",
                   inputs={"C": sig([1, 0, 1, 0, 1, 0, 1, 0]), "A": sig(X8, valid=[1, 1, 0, 0, 1, 1, 1, 1]),
                           "B": sig([9, 9, 9, 9, 9, 9, 9, 9], valid=[0, 1, 1, 0, 1, 1, 1, 1])},
                   ref=f"{HELP_FN}: choose", note="only the chosen branch's validity counts",
                   expect=lambda e: [a if c != 0 else b for c, a, b in zip(e.x("C"), e.x("A"), e.x("B"))]))
    cs.append(Case("choose_invalid_branch", "choose('X' > 0, invalid(), 'X')", inputs=X,
                   ref=f"{HELP_FN}: invalid", note="choose(cond, invalid(), x) blanks samples",
                   expect=lambda e: [NaN if v > 0 else v for v in e.x("X")]))
    cs.append(Case("choose_unify_scalar_series", "choose('X' > 0, 5, 'X')", inputs=X,
                   ref=f"{DESIGN} 3.2 rule 8", note="a Scalar branch and a Series branch unify to a Series",
                   expect=lambda e: [5.0 if v > 0 else v for v in e.x("X")]))
    cs.append(Case("invalid_propagates", "invalid() + 'X' * 2", inputs=X, ref=f"{HELP_FN}: invalid",
                   note="invalid() in arithmetic gives invalid samples everywhere", expect=lambda e: [NaN] * e.n))
    cs.append(Case("invalid_input_propagates", "('X' + 1) * 2 - abs('X')",
                   inputs={"X": sig(SIGNED, valid=[1, 1, 0, 1, 1, 0, 1, 1])}, ref=f"{DESIGN} 6.6",
                   note="an invalid operand sample gives an invalid output sample",
                   expect=lambda e: e.ew(lambda v: (v + 1) * 2 - abs(v), e.x("X"))))
    # Overflow to infinity is a domain error (D-185): an invalid sample, never a stored infinity.
    ew("pow_zero_negative_invalid", "0 ^ (0 - 'X')", P, lambda v: 0.0 ** -v,
       "0 to a negative power is infinite: invalid (D-185)", fname="power")
    ew("exp_overflow_invalid", "exp(1000 + 'X')", X, lambda v: math.exp(1000 + v),
       "exp beyond the double range is infinite: invalid (D-185)", fname="exp")
    f32_max = 3.4028234663852886e38
    cs.append(Case("f32_overflow_invalid", "'X' * 1e38", inputs={"X": sig([0, 1, 2, 3, 4, -4, 0.5, 10])},
                   ref=f"{DESIGN} 6.6", note="an F32 result beyond the F32 range is invalid (D-185)",
                   expect=lambda e: [v * 1e38 if abs(v * 1e38) <= f32_max else NaN for v in e.x("X")]))
    return cs


def operator_cases() -> list[Case]:
    cs: list[Case] = []
    A = {"A": sig([1, 2, 3, 4, 5, 6, 7, 8]), "B": sig([8, 7, 6, 5, 4, 3, 2, 1])}

    def op(cid: str, expr: str, fn: Callable, note: str, inputs: Optional[dict] = None, ref: str = HELP_OPS,
           **kw: Any) -> None:
        ins = inputs or A
        names = list(ins)
        cs.append(Case(cid, expr, inputs=ins, ref=ref, note=note,
                       expect=lambda e: e.ew(fn, *[e.x(k) for k in names]), **kw))

    op("op_add_sub", "'A' + 'B' - 1", lambda a, b: a + b - 1, "+ and -")
    op("op_mul_div", "'A' * 'B' / 4", lambda a, b: a * b / 4, "* and /")
    op("op_integer_division_is_real", "7 / 2 + 'A' * 0", lambda a, b: 3.5, "integer / integer is real (2.4 rule 1)",
       ref=f"{DESIGN} 2.4")
    op("op_divide_by_zero_invalid", "'A' / ('B' - 4)", lambda a, b: a / (b - 4) if b != 4 else NaN,
       "division by zero gives an invalid sample")
    op("op_power_right_assoc", "2^3^2 + 'A' * 0", lambda a, b: 512.0, "2^3^2 = 2^9", ref=f"{DESIGN} 2.3")
    op("op_unary_minus_power", "-2^2 + 'A' * 0", lambda a, b: -4.0, "-2^2 = -4 (lang.pow-unary)",
       ref=f"{DESIGN} 2.3", diagnostics=[("I-COMPAT-POW", 1)])
    op("op_unary_minus_power_compat", "-2^2 + 'A' * 0", lambda a, b: 4.0,
       "unaryMinusAbovePower = true: -2^2 = 4", ref=f"{DESIGN} 2.3", compat={"unaryMinusAbovePower": True})
    op("op_unary_minus_channel_power", "-'A'^2", lambda a, b: -(a * a), "-x^2 = -(x^2)", ref=f"{DESIGN} 2.3")
    op("op_power_negative_exponent", "2^-'A'", lambda a, b: 2.0 ** -a, "the exponent may carry a sign",
       ref=f"{DESIGN} 2.2")
    op("op_relational", "('A' < 'B') + ('A' <= 4) * 2 + ('A' > 6) * 4 + ('A' >= 'B') * 8",
       lambda a, b: float(a < b) + float(a <= 4) * 2 + float(a > 6) * 4 + float(a >= b) * 8, "< <= > >=")
    op("op_equality", "('A' == 4) + ('A' != 'B') * 2", lambda a, b: float(a == 4) + float(a != b) * 2, "== !=")
    op("op_logical", "('A' > 2 && 'B' > 2) + ('A' < 2 || 'B' < 2) * 2 + !('A' > 4) * 4",
       lambda a, b: float(a > 2 and b > 2) + float(a < 2 or b < 2) * 2 + float(not a > 4) * 4, "&& || !")
    op("op_word_aliases", "('A' GT 2 AND 'B' gt 2) + ('A' LT 2 Or 'B' LTE 1) * 2 + (NOT ('A' GTE 5)) * 4 + "
       "('A' EQ 3) * 8 + ('A' neq 'B') * 16",
       lambda a, b: float(a > 2 and b > 2) + float(a < 2 or b <= 1) * 2 + float(not a >= 5) * 4 +
       float(a == 3) * 8 + float(a != b) * 16, "word operators in any letter case (lang.word-operators)")
    op("op_not_precedence", "(NOT 'Z' GTE 1) + (!'Z' + 1) * 2",
       lambda z: float(float(z == 0) >= 1) + (float(z == 0) + 1) * 2,
       "NOT and ! bind tighter than the relational operators: NOT 'Z' GTE 1 is (NOT 'Z') GTE 1 (2.3)",
       inputs={"Z": sig([0, 1, 0, 2, -1, 0, 0.5, 3])}, ref=f"{DESIGN} 2.3")
    ints = {"I": sig([12, 10, 255, 6, 0, 7, 3, 5], dtype="i32"), "J": sig([10, 6, 15, 3, 9, 7, 2, 4], dtype="i32")}
    op("op_bitwise", "('I' & 'J') + ('I' | 'J') * 1000", lambda a, b: float((int(a) & int(b)) + (int(a) | int(b)) * 1000),
       "& and | on integer channels", inputs=ints)
    op("op_bitwise_not", "~'I'", lambda a, b: float(struct.unpack("<i", struct.pack("<I", (~int(a)) & 0xFFFFFFFF))[0]),
       "~ is a 32-bit complement", inputs=ints)
    op("op_band_vs_equality", "'I' & 'J' == 'J'", lambda a, b: float(int(a) & int(a == a and b == b)),
       "a & b == c parses as a & (b == c)", inputs=ints, ref=f"{DESIGN} 2.3")
    op("op_comment_and_whitespace", "'A' /* block */ * 2 // trailing\n + 1", lambda a, b: a * 2 + 1,
       "comments are dropped (lang.comments)", ref=HELP_EXPR)
    op("op_scientific_notation", "'A' * 1e3 + 2.5E-1", lambda a, b: a * 1000 + 0.25, "exponent literals",
       ref=HELP_EXPR)
    op("op_function_case_insensitive", "ABS('A' - 'B') + Max('A', 'B')", lambda a, b: abs(a - b) + max(a, b),
       "function names in any case (W-STYLE-CASE)", diagnostics=[("W-STYLE-CASE", 2)], ref=HELP_EXPR)
    cs.append(Case("unit_tag_conversion", "'S'[km/h]", inputs={"S": sig([0, 10, 20, 27.5], unit="m/s")},
                   ref=HELP_EXPR, note="[unit] converts the base-unit value before calculation (2.4 rule 5)",
                   expect=lambda e: [v * 3.6 for v in e.x("S")], diagnostics=[("W-UNIT-MISSING", 0)]))
    cs.append(Case("unit_tag_affine", "'T'[deg C] * 2", inputs={"T": sig([273.15, 283.15, 300, 373.15], unit="K")},
                   ref=HELP_EXPR, note="affine conversion of an absolute temperature",
                   expect=lambda e: [(v - 273.15) * 2 for v in e.x("T")], tol=(1e-4, 1e-6)))
    cs.append(Case("unit_missing_reads_base", "'T' - 273.15", inputs={"T": sig([273.15, 283.15, 300, 373.15], unit="K")},
                   ref=f"{DESIGN} 2.4 rule 5", note="no [unit]: the stored base unit, with W-UNIT-MISSING",
                   expect=lambda e: [v - 273.15 for v in e.x("T")], diagnostics=[("W-UNIT-MISSING", 1)],
                   tol=(1e-4, 1e-6)))
    cs.append(Case("unit_result_to_base", "'S'[km/h] * 2", inputs={"S": sig([0, 10, 20, 27.5], unit="m/s")},
                   ref=f"{DESIGN} 6.1", note="a declared result unit: the values are stored in its base unit",
                   result_unit="km/h", expect=lambda e: [v * 2 for v in e.x("S")]))
    cs.append(Case("constant_with_unit", "'X' * 'Wheelbase'[mm]", inputs={"X": sig([1, 2, 3, 4])},
                   constants={"Wheelbase": (2.7, "m")}, ref="Maths Constants (Pro)",
                   note="a constant requested in another unit of its quantity",
                   expect=lambda e: [v * 2700 for v in e.x("X")], tol=(1e-3, 1e-6)))
    cs.append(Case("constant_unitless", "'X' * 'Gain' + 'Offset'", inputs={"X": sig([1, 2, 3, 4])},
                   constants={"Gain": (2.5, ""), "Offset": (-1, "")}, ref="Maths Constants (Pro)",
                   note="constants are quoted like channels", expect=lambda e: [v * 2.5 - 1 for v in e.x("X")]))
    cs.append(Case("detail_numeric", "'X' / 'Vehicle Wheelbase'[m]", inputs={"X": sig([1, 2, 3, 4])},
                   details={"Vehicle Wheelbase": (2.5, "m")}, ref=f"{DESIGN} 4.1",
                   note="a numeric detail of the source", expect=lambda e: [v / 2.5 for v in e.x("X")]))
    for opname, fn in (("eq", lambda c: c == 0), ("ne", lambda c: c != 0), ("lt", lambda c: c < 0),
                       ("le", lambda c: c <= 0), ("gt", lambda c: c > 0), ("ge", lambda c: c >= 0)):
        sym = {"eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">="}[opname]
        c = (("Joe" > "Jim") - ("Joe" < "Jim"))
        cs.append(Case(f"text_compare_{opname}", f"('Driver' {sym} \"Jim\") + 'X' * 0", inputs={"X": sig([1, 2, 3, 4])},
                       details={"Driver": "Joe"}, ref=f"{DESIGN} 3.2 rule 6",
                       note="text against text, case-sensitive, by UTF-8 code units (lang.text-compare)",
                       expect=lambda e, f=fn, cc=c: [1.0 if f(cc) else 0.0] * e.n))
    cs.append(Case("text_compare_case_sensitive", "('Driver' == \"joe\") + 'X' * 0", inputs={"X": sig([1, 2, 3, 4])},
                   details={"Driver": "Joe"}, ref=f"{DESIGN} 3.2 rule 6", note="== is exact and case-sensitive",
                   expect=lambda e: [0.0] * e.n))
    cs.append(Case("mixed_rates_linear", "'A' + 'B'", inputs={"A": sig(X8), "B": sig([0, 10, 20, 30], R5)},
                   ref=f"{DESIGN} 6.4 rule 2", note="auto rate = the highest input rate; B resampled Linear",
                   expect=lambda e: [a + b for a, b in zip(e.x("A"), e.x("B"))]))
    cs.append(Case("mixed_rates_previous", "'A' + 'G'",
                   inputs={"A": sig(X8), "G": sig([1, 2, 3, 4], R5, interp="previous", dtype="i32")},
                   ref=f"{DESIGN} 6.4 rule 6", note="each input with its own interpolation (Previous for a gear)",
                   expect=lambda e: [a + b for a, b in zip(e.x("A"), e.x("G"))]))
    cs.append(Case("explicit_rate_downsample", "'A' * 1", inputs={"A": sig([0, 1, 2, 3, 4, 5, 6, 7, 8, 9], R20)},
                   rate=R5, ref=f"{DESIGN} 6.3", note="an explicit lower rate point-samples (no anti-alias filter)",
                   expect=lambda e: e.x("A")))
    return cs


def windowed_cases() -> list[Case]:
    cs: list[Case] = []
    W = {"X": sig(W12)}
    WI = {"X": sig(W12, valid=[1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1])}
    ref = f"{HELP_FN}: smooth"
    for n in (3, 5, 2, 4, 6, 0, 1, 21):
        cs.append(Case(f"smooth_points_{n}", f"smooth('X', {n})", inputs=W, ref=ref,
                       note=("odd window centred" if n % 2 == 1 and n > 1 else
                             "n of 0 or 1 is 2" if n < 2 else "even window biased one point earlier") +
                       "; edges truncated (smooth.edges)",
                       expect=lambda e, k=n: smooth(e.x("X"), ("points", k), e.rate)))
    for t, note in ((0.3, "0.3 s at 10 Hz: 3 points"), (0.25, "0.25 s at 10 Hz: 2*floor(1.25)+1 = 3 points"),
                    (0.5, "0.5 s: 5 points"), (0.1, "t*rate < 2: one point, widened to 2"),
                    (1.0, "1.0 s: 11 points")):
        cs.append(Case(f"smooth_seconds_{str(t).replace('.', 'p')}", f"smooth('X', {t})", inputs=W, ref=ref,
                       note=f"a decimal window is seconds converted to an odd count; {note}",
                       expect=lambda e, s=t: smooth(e.x("X"), ("seconds", s), e.rate)))
    cs.append(Case("smooth_seconds_scientific", "smooth('X', 3e-1)", inputs=W, ref=ref,
                   note="an exponent literal is decimal: seconds (D-082)",
                   expect=lambda e: smooth(e.x("X"), ("seconds", 0.3), e.rate)))
    cs.append(Case("smooth_invalid_excluded", "smooth('X', 3)", inputs=WI, ref=ref,
                   note="invalid samples are left out of the average; a window without a valid sample is invalid",
                   expect=lambda e: smooth(e.x("X"), ("points", 3), e.rate)))
    cs.append(Case("smooth_even_window_bias", "smooth('X'[], 4)",
                   inputs={"X": sig(X8, valid=[1, 1, 1, 1, 0, 1, 1, 1])}, ref=ref, kind="help",
                   note="the 03 section 14.1 example: window [i-2, i+1], truncated edges, invalid excluded",
                   expect=lambda e: smooth(e.x("X"), ("points", 4), e.rate)))
    cs.append(Case("smooth_constant_is_constant", "smooth('X', 5)", inputs={"X": sig([2.5] * 12)}, ref=ref,
                   note="window identity: the mean of a constant", expect=lambda e: [2.5] * e.n))
    cs.append(Case("smooth_linear_resampling", "smooth('G', 3)",
                   inputs={"G": sig([1, 2, 4, 8, 16, 32], R5, interp="previous")}, rate=R10, ref=ref,
                   note="smooth resamples its argument Linear whatever its interpolation (6.4 rule 6)",
                   expect=lambda e: smooth(e.x("G", "linear"), ("points", 3), e.rate)))
    cs.append(Case("smooth_expression_argument", "smooth('X' * 2 + 1, 3)", inputs=W, ref=ref,
                   note="the argument may be an expression of channels",
                   expect=lambda e: smooth([v * 2 + 1 for v in e.x("X")], ("points", 3), e.rate)))
    cs.append(Case("smooth_nested", "smooth(smooth('X', 3), 3)", inputs=W, ref=ref, note="chained windows",
                   expect=lambda e: smooth(smooth(e.x("X"), ("points", 3), e.rate), ("points", 3), e.rate)))
    dref = f"{HELP_FN}: derivative"
    cs.append(Case("derivative_default", "derivative('X')", inputs=W, ref=dref,
                   note="default 2 points: (x[i] - x[i-1]) * rate; the first sample is invalid (derivative.edges)",
                   expect=lambda e: derivative(e.x("X"), e.rate)))
    for n in (3, 4, 5, 0, 1, 11):
        cs.append(Case(f"derivative_points_{n}", f"derivative('X', {n})", inputs=W, ref=dref,
                       note="least-squares slope over the window; n of 0 or 1 is 2",
                       expect=lambda e, k=n: derivative(e.x("X"), e.rate, ("points", k))))
    cs.append(Case("derivative_seconds", "derivative('X', 0.3)", inputs=W, ref=dref,
                   note="0.3 s at 10 Hz: 3 points", expect=lambda e: derivative(e.x("X"), e.rate, ("seconds", 0.3))))
    cs.append(Case("derivative_invalid_excluded", "derivative('X', 3)", inputs=WI, ref=dref,
                   note="invalid points are left out; fewer than 2 points gives an invalid sample",
                   expect=lambda e: derivative(e.x("X"), e.rate, ("points", 3))))
    cs.append(Case("derivative_of_line", "derivative('X', 5)", inputs={"X": sig([3 + 0.5 * i for i in range(12)])},
                   ref=dref, note="window identity: the slope of a line is its gradient per second",
                   expect=lambda e: [5.0] * e.n))
    cs.append(Case("derivative_wrt_channel", "derivative('X', 'Y')",
                   inputs={"X": sig(W12), "Y": sig([0, 1, 3, 4, 4, 6, 9, 10, 12, 12, 15, 16])}, ref=dref,
                   note="with respect to a channel (release note 1.0.5); dy = 0 invalid (derivative.wrt)",
                   expect=lambda e: derivative(e.x("X"), e.rate, None, e.x("Y"))))
    cs.append(Case("derivative_wrt_channel_window", "derivative('X', 'Y', 3)",
                   inputs={"X": sig(W12), "Y": sig([0, 1, 3, 4, 4, 6, 9, 10, 12, 12, 15, 16])}, ref=dref,
                   note="least-squares slope of x against y over 3 points",
                   expect=lambda e: derivative(e.x("X"), e.rate, ("points", 3), e.x("Y"))))
    cs.append(Case("derivative_wrt_channel_wide", "derivative('X', 'Y', 11)",
                   inputs={"X": sig(W12), "Y": sig([0, 1, 3, 4, 4, 6, 9, 10, 12, 12, 15, 16])}, ref=dref,
                   note="a wide window (the kernel's sliding sums)",
                   expect=lambda e: derivative(e.x("X"), e.rate, ("points", 11), e.x("Y"))))
    long_wave = [math.sin(i / 7.0) * 10 + i * 0.1 for i in range(64)]
    cs.append(Case("derivative_wide_window_long", "derivative('X', 15)", inputs={"X": sig(long_wave, R20)},
                   ref=dref, note="15 points over 64 samples (sliding least squares)",
                   expect=lambda e: derivative(e.x("X"), e.rate, ("points", 15))))
    cs.append(Case("smooth_wide_window_long", "smooth('X', 2.0)", inputs={"X": sig(long_wave, R20)}, ref=ref,
                   note="2 s at 20 Hz: 41 points (sliding sums)",
                   expect=lambda e: smooth(e.x("X"), ("seconds", 2.0), e.rate)))
    return cs


# Zeros on each side of the symmetric pulse of the zero-phase cases: enough for the priming at both ends to
# decay below 1e-9 at 100 Hz for RC -2 and Chebyshev -4 and -8 (the 12th-order form needs four times as much;
# self_test checks all of them).
SYMMETRY_PAD = 200


def iir_cases() -> list[Case]:
    cs: list[Case] = []
    fs = 100.0
    X = {"X": sig(wave(120, R100, 40), R100)}
    ref = f"{HELP_FN}: filter_lp"
    cref_lp = f"{HELP_FN}: filter_cheby_lp"
    tol = (2e-6, 2e-6)
    for order in (1, 2, 3, -1, -2):
        cs.append(Case(f"filter_lp_order_{str(order).replace('-', 'm')}", f"filter_lp('X', 5, {order})", inputs=X,
                       ref=ref, note="RC low pass at 5 Hz; 1-3 forward passes, -1 backward, -2 zero phase "
                       "(iir.rc-form, iir.priming)", tol=tol,
                       expect=lambda e, o=order: filter_rc(e.x("X"), False, 5.0, fs, o)))
    cs.append(Case("filter_lp_default_order", "filter_lp('X', 5)", inputs=X, ref=ref, note="order omitted: 1",
                   tol=tol, expect=lambda e: filter_rc(e.x("X"), False, 5.0, fs, 1)))
    for order in (1, 3, -2):
        cs.append(Case(f"filter_hp_order_{str(order).replace('-', 'm')}", f"filter_hp('X', 2, {order})", inputs=X,
                       ref=f"{HELP_FN}: filter_hp", note="RC high pass at 2 Hz", tol=tol,
                       expect=lambda e, o=order: filter_rc(e.x("X"), True, 2.0, fs, o)))
    cs.append(Case("filter_lp_constant_no_transient", "filter_lp('X', 3, 2)", inputs={"X": sig([4.5] * 30, R100)},
                   ref=ref, note="primed with the first valid sample: a constant passes unchanged (iir.priming)",
                   expect=lambda e: [4.5] * e.n))
    # 2 s of zeros on each side, so the priming at both ends has decayed and symmetry holds well inside the
    # tolerance (the self-test checks it; iir.priming in docs/maths-compat.md records the end effect).
    pulse = [0.0] * SYMMETRY_PAD + [1.0] * 11 + [0.0] * SYMMETRY_PAD
    cs.append(Case("filter_lp_zero_phase_symmetry", "filter_lp('X', 4, -2)",
                   inputs={"X": sig(pulse, R100)}, ref=ref,
                   note="zero phase: a symmetric pulse gives a symmetric output", tol=tol,
                   expect=lambda e: filter_rc(e.x("X"), False, 4.0, fs, -2)))
    cs.append(Case("filter_cheby_lp_zero_phase_symmetry", "filter_cheby_lp('X', 6, -8, 0.5)",
                   inputs={"X": sig(pulse, R100)}, ref=cref_lp,
                   note="zero phase Chebyshev: a symmetric pulse gives a symmetric output", tol=(1e-5, 1e-5),
                   expect=lambda e: filter_cheby(e.x("X"), "lp", fs, 6.0, 0.0, -8, 0.5)))
    cs.append(Case("filter_hp_default_order", "filter_hp('X', 2)", inputs=X, ref=f"{HELP_FN}: filter_hp",
                   note="order omitted: 1", tol=tol, expect=lambda e: filter_rc(e.x("X"), True, 2.0, fs, 1)))
    gap_vals = wave(80, R20, 30)
    short = [1] * 80
    for k in range(20, 30):
        short[k] = 0
    long_ = [1] * 80
    for k in range(20, 50):
        long_[k] = 0
    cs.append(Case("filter_lp_short_gap_holds", "filter_lp('X', 1)", inputs={"X": sig(gap_vals, R20, valid=short)},
                   ref=ref, note="a gap of 0.5 s: invalid samples stay invalid and the state holds (iir.priming)",
                   tol=tol, expect=lambda e: filter_rc(e.x("X"), False, 1.0, 20.0, 1)))
    cs.append(Case("filter_lp_long_gap_reprimes", "filter_lp('X', 1)", inputs={"X": sig(gap_vals, R20, valid=long_)},
                   ref=ref, note="a gap of 1.5 s re-primes the filter at the next valid sample (iir.priming)",
                   tol=tol, expect=lambda e: filter_rc(e.x("X"), False, 1.0, 20.0, 1)))
    cs.append(Case("filter_lp_nyquist_error", "filter_lp('X', 50)", inputs=X, ref=f"{DESIGN} 5.3",
                   note="a cutoff at half the result rate is E-ARG-VALUE", error="E-ARG-VALUE"))
    cs.append(Case("filter_lp_order_unsupported", "filter_lp('X', 5, 4)", inputs=X, ref=f"{DESIGN} 5.3",
                   note="orders other than 1, 2, 3, -1, -2 are E-ARG-VALUE", error="E-ARG-VALUE"))
    cref = cref_lp
    for order, ripple in ((2, 0), (4, 0.5), (6, 1.0), (-4, 0), (-8, 0.5), (-12, 0)):
        cs.append(Case(f"filter_cheby_lp_{str(order).replace('-', 'm')}_r{str(ripple).replace('.', 'p')}",
                       f"filter_cheby_lp('X', 6, {order}, {ripple})", inputs=X, ref=cref,
                       note="Chebyshev type I (ripple 0: Butterworth); negative orders zero phase with half the "
                       "order each way (iir.cheby-design)", tol=(1e-5, 1e-5),
                       expect=lambda e, o=order, r=ripple: filter_cheby(e.x("X"), "lp", fs, 6.0, 0.0, o, r)))
    cs.append(Case("filter_cheby_lp_default_order", "filter_cheby_lp('X', 6)", inputs=X, ref=cref,
                   note="order omitted: 2 (the help's default), ripple 0", tol=(1e-5, 1e-5),
                   expect=lambda e: filter_cheby(e.x("X"), "lp", fs, 6.0, 0.0, 2, 0.0)))
    cs.append(Case("filter_cheby_lp_order_only", "filter_cheby_lp('X', 6, 4)", inputs=X, ref=cref,
                   note="ripple omitted: 0 (Butterworth)", tol=(1e-5, 1e-5),
                   expect=lambda e: filter_cheby(e.x("X"), "lp", fs, 6.0, 0.0, 4, 0.0)))
    for order, ripple in ((2, 0), (4, 0.5), (-4, 0.5)):
        cs.append(Case(f"filter_cheby_hp_{str(order).replace('-', 'm')}_r{str(ripple).replace('.', 'p')}",
                       f"filter_cheby_hp('X', 3, {order}, {ripple})", inputs=X, ref=f"{HELP_FN}: filter_cheby_hp",
                       note="Chebyshev high pass, unit gain at Nyquist", tol=(1e-5, 1e-5),
                       expect=lambda e, o=order, r=ripple: filter_cheby(e.x("X"), "hp", fs, 3.0, 0.0, o, r)))
    for order, ripple in ((2, 0), (4, 0.5), (-4, 0)):
        cs.append(Case(f"filter_cheby_bp_{str(order).replace('-', 'm')}_r{str(ripple).replace('.', 'p')}",
                       f"filter_cheby_bp('X', 4, 9, {order}, {ripple})", inputs=X, ref=f"{HELP_FN}: filter_cheby_bp",
                       note="band pass 4-9 Hz, centre sqrt(f_lo f_hi) of the pre-warped edges", tol=(1e-5, 1e-5),
                       expect=lambda e, o=order, r=ripple: filter_cheby(e.x("X"), "bp", fs, 4.0, 9.0, o, r)))
    cs.append(Case("filter_cheby_bp_default", "filter_cheby_bp('X', 4, 9)", inputs=X, ref=f"{HELP_FN}: filter_cheby_bp",
                   note="order omitted: 2", tol=(1e-5, 1e-5),
                   expect=lambda e: filter_cheby(e.x("X"), "bp", fs, 4.0, 9.0, 2, 0.0)))
    cs.append(Case("filter_cheby_bp_order_only", "filter_cheby_bp('X', 4, 9, 4)", inputs=X,
                   ref=f"{HELP_FN}: filter_cheby_bp", note="ripple omitted: 0 (Butterworth)", tol=(1e-5, 1e-5),
                   expect=lambda e: filter_cheby(e.x("X"), "bp", fs, 4.0, 9.0, 4, 0.0)))
    cs.append(Case("filter_cheby_hp_default", "filter_cheby_hp('X', 3)", inputs=X, ref=f"{HELP_FN}: filter_cheby_hp",
                   note="order omitted: 2, ripple 0", tol=(1e-5, 1e-5),
                   expect=lambda e: filter_cheby(e.x("X"), "hp", fs, 3.0, 0.0, 2, 0.0)))
    cs.append(Case("filter_cheby_hp_order_only", "filter_cheby_hp('X', 3, -4)", inputs=X,
                   ref=f"{HELP_FN}: filter_cheby_hp", note="ripple omitted: 0; zero phase", tol=(1e-5, 1e-5),
                   expect=lambda e: filter_cheby(e.x("X"), "hp", fs, 3.0, 0.0, -4, 0.0)))
    cs.append(Case("filter_cheby_bs_default", "filter_cheby_bs('X', 5, 9)", inputs=X, ref=f"{HELP_FN}: filter_cheby_bs",
                   note="order omitted: 2, ripple 0", tol=(1e-5, 1e-5),
                   expect=lambda e: filter_cheby(e.x("X"), "bs", fs, 5.0, 9.0, 2, 0.0)))
    cs.append(Case("filter_cheby_bs_order_only", "filter_cheby_bs('X', 5, 9, 4)", inputs=X,
                   ref=f"{HELP_FN}: filter_cheby_bs", note="ripple omitted: 0", tol=(1e-5, 1e-5),
                   expect=lambda e: filter_cheby(e.x("X"), "bs", fs, 5.0, 9.0, 4, 0.0)))
    for order, ripple in ((2, 0), (4, 0.5)):
        cs.append(Case(f"filter_cheby_bs_{order}_r{str(ripple).replace('.', 'p')}",
                       f"filter_cheby_bs('X', 5, 9, {order}, {ripple})", inputs=X, ref=f"{HELP_FN}: filter_cheby_bs",
                       note="band stop 5-9 Hz, unit gain at DC", tol=(1e-5, 1e-5),
                       expect=lambda e, o=order, r=ripple: filter_cheby(e.x("X"), "bs", fs, 5.0, 9.0, o, r)))
    cs.append(Case("filter_cheby_order_unsupported", "filter_cheby_lp('X', 5, 3)", inputs=X, ref=f"{DESIGN} 5.3",
                   note="Chebyshev orders are 2, 4, 6, -4, -8, -12", error="E-ARG-VALUE"))
    cs.append(Case("filter_cheby_band_nyquist", "filter_cheby_bp('X', 10, 60)", inputs=X, ref=f"{DESIGN} 5.3",
                   note="f_hi above half the rate is E-ARG-VALUE", error="E-ARG-VALUE"))
    return cs


def stateful_cases() -> list[Case]:
    cs: list[Case] = []
    laps = {"Outings": [(0, 1_200_000)], "Outings:Laps": [(0, 400_000), (400_000, 800_000), (800_000, 1_200_000)]}
    X = {"X": sig(W12)}
    XC = {"X": sig(W12), "C": sig([1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1])}
    iref = f"{HELP_FN}: integrate"
    cs.append(Case("integrate_plain", "integrate('X')", inputs=X, ref=iref,
                   note="rectangle rule, sample i included (integrate.form)",
                   expect=lambda e: integrate(e.x("X"), e.rate), tol=(1e-9, 1e-9)))
    cs.append(Case("integrate_constant_is_linear", "integrate(2)", inputs=X, ref=iref,
                   note="window identity: the integral of a constant grows linearly",
                   expect=lambda e: integrate([2.0] * e.n, e.rate), tol=(1e-9, 1e-9)))
    cs.append(Case("integrate_gated", "integrate('X', 'C')", inputs=XC, ref=iref,
                   note="holds while the condition is false", expect=lambda e: integrate(e.x("X"), e.rate, e.x("C")),
                   tol=(1e-9, 1e-9)))
    cs.append(Case("integrate_reset_per_lap", "integrate('X', 1, range_change(\"Outings:Laps\"))", inputs=X,
                   ranges=laps, ref=iref, note="restarts when reset increases (range_change per lap)",
                   expect=lambda e: integrate(e.x("X"), e.rate, None, range_change(e.t, e.ranges("Outings:Laps"))),
                   tol=(1e-9, 1e-9)))
    cs.append(Case("integrate_invalid_adds_zero", "integrate('X')",
                   inputs={"X": sig(W12, valid=[1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1])}, ref=iref,
                   note="an invalid sample contributes 0 and the output stays valid (integrate.invalid)",
                   expect=lambda e: integrate(e.x("X"), e.rate), tol=(1e-9, 1e-9)))
    cs.append(Case("integrate_time_while", "integrate(1, 'X' > 4)", inputs=X, ref=iref,
                   note="integrate(1, cond) is the time the condition holds (lang.constant-first-arg)",
                   expect=lambda e: integrate([1.0] * e.n, e.rate, [float(v > 4) for v in e.x("X")]),
                   tol=(1e-9, 1e-9)))
    dist = [0, 1.5, 3.5, 6, 9, 12.5, 16, 19, 22, 24.5, 27, 30]
    cs.append(Case("integrate_dist_plain", "integrate_dist('X')",
                   inputs={"X": sig(W12), "Corr Dist": sig(dist, dtype="f64", unit="m")},
                   ref=f"{HELP_FN}: integrate_dist", note="sum of x times the Corr Dist increment (integrate_dist.form)",
                   expect=lambda e: integrate(e.x("X"), e.rate, None, None, e.x("Corr Dist")), tol=(1e-9, 1e-9)))
    cs.append(Case("integrate_dist_gated_reset", "integrate_dist('X', 'X' > 3, range_change(\"Outings:Laps\"))",
                   inputs={"X": sig(W12), "Corr Dist": sig(dist, dtype="f64", unit="m")}, ranges=laps,
                   ref=f"{HELP_FN}: integrate_dist", note="integrate_dist shares integrate's arities (D-082)",
                   expect=lambda e: integrate(e.x("X"), e.rate, [float(v > 3) for v in e.x("X")],
                                              range_change(e.t, e.ranges("Outings:Laps")), e.x("Corr Dist")),
                   tol=(1e-9, 1e-9)))
    cs.append(Case("integrate_dist_gated", "integrate_dist('X', 'X' > 3)",
                   inputs={"X": sig(W12), "Corr Dist": sig(dist, dtype="f64", unit="m")},
                   ref=f"{HELP_FN}: integrate_dist", note="gated: holds while the condition is false",
                   expect=lambda e: integrate(e.x("X"), e.rate, [float(v > 3) for v in e.x("X")], None,
                                              e.x("Corr Dist")), tol=(1e-9, 1e-9)))
    cs.append(Case("integrate_dist_no_distance", "integrate_dist('X')", inputs=X, ref=f"{DESIGN} 5.4",
                   note="without Corr Dist: E-NO-DISTANCE", error="E-NO-DISTANCE"))
    sref = f"{HELP_FN}: stat_*"
    for kind in ("min", "max", "mean", "std_dev", "start", "end"):
        kk = "std" if kind == "std_dev" else kind
        cs.append(Case(f"stat_{kind}_whole", f"stat_{kind}('X')", inputs=X, ref=sref,
                       note="ungated, no selection: the whole extent (the math window), constant over it",
                       expect=lambda e, q=kk: stat(q, e.x("X"), e.t)))
        cs.append(Case(f"stat_{kind}_reset_per_lap", f"stat_{kind}('X', 1, range_change(\"Outings:Laps\"))",
                       inputs=X, ranges=laps, ref=sref,
                       note="with reset: per segment, written to every sample of it (LapScalar, stat.segments)",
                       expect=lambda e, q=kk: stat(q, e.x("X"), e.t, None, range_change(e.t, e.ranges("Outings:Laps")))))
    cs.append(Case("stat_mean_main_window", "stat_mean('X')", inputs=X, ranges=laps,
                   selection={"main": "Outings:Laps/2"}, ref=sref,
                   note="ungated: over the math window (the Main lap), broadcast over the extent",
                   expect=lambda e: stat("mean", e.x("X"), e.t, None, None, (400_000, 800_000))))
    for kind in ("min", "max", "mean", "std_dev", "start", "end"):
        kk = "std" if kind == "std_dev" else kind
        cs.append(Case(f"stat_{kind}_gated", f"stat_{kind}('X', 'C')", inputs=XC, ref=sref,
                       note="(x, cond): over the math window, only samples where the condition holds",
                       expect=lambda e, q=kk: stat(q, e.x("X"), e.t, e.x("C"))))
    cs.append(Case("stat_start_gated_reset", "stat_start('X', 'X' > 4, range_change(\"Outings:Laps\"))", inputs=X,
                   ranges=laps, ref=sref, note="the first gated value of each lap (the help's corner example shape)",
                   expect=lambda e: stat("start", e.x("X"), e.t, [float(v > 4) for v in e.x("X")],
                                         range_change(e.t, e.ranges("Outings:Laps")))))
    cs.append(Case("stat_min_empty_segment", "stat_min('X', 'X' > 8, range_change(\"Outings:Laps\"))", inputs=X,
                   ranges=laps, ref=sref, note="a segment with no gated sample is invalid",
                   expect=lambda e: stat("min", e.x("X"), e.t, [float(v > 8) for v in e.x("X")],
                                         range_change(e.t, e.ranges("Outings:Laps")))))
    cs.append(Case("stat_mean_invalid_skipped", "stat_mean('X')",
                   inputs={"X": sig(W12, valid=[1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1])}, ref=sref,
                   note="invalid samples are skipped", expect=lambda e: stat("mean", e.x("X"), e.t)))
    rref = f"{DESIGN} 5.4: running_min/max"
    for kind in ("min", "max"):
        cs.append(Case(f"running_{kind}_plain", f"running_{kind}('X')", inputs=X, ref=rref, kind="release-notes",
                       note="cumulative extreme so far (running.names)",
                       expect=lambda e, q=kind: running(q, e.x("X"))))
        cs.append(Case(f"running_{kind}_gated", f"running_{kind}('X', 'C')", inputs=XC, ref=rref,
                       kind="release-notes", note="(x, cond): holds while gated off, one segment",
                       expect=lambda e, q=kind: running(q, e.x("X"), e.x("C"))))
        cs.append(Case(f"running_{kind}_gated_reset", f"running_{kind}('X', 'C', range_change(\"Outings:Laps\"))",
                       inputs=XC, ranges=laps, ref=rref, kind="release-notes",
                       note="holds while gated off; restarts per lap; invalid before the first gated sample",
                       expect=lambda e, q=kind: running(q, e.x("X"), e.x("C"), range_change(e.t, e.ranges("Outings:Laps")))))
    fref = f"{HELP_FN}: flip_flop"
    cs.append(Case("flip_flop_latch", "flip_flop('S', 'R')",
                   inputs={"S": sig([0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0]), "R": sig([0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0])},
                   ref=fref, note="set on a rising edge of set, cleared on a rising edge of reset",
                   expect=lambda e: flip_flop(e.x("S"), e.x("R"))))
    cs.append(Case("flip_flop_reset_wins", "flip_flop('S', 'R')",
                   inputs={"S": sig([1, 0, 1, 0, 1, 0, 0, 0]), "R": sig([0, 0, 1, 0, 0, 0, 0, 0])},
                   ref=fref, note="a high first sample is a rising edge; both edges on one sample: reset wins "
                   "(stateful.before-start)", expect=lambda e: flip_flop(e.x("S"), e.x("R"))))
    eref = f"{HELP_FN}: edge_delay"
    B = {"B": sig([0, 1, 1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0])}
    for t, edge in ((0.3, "rise"), (0.2, "fall"), (0.0, "rise"), (0.25, "rise")):
        cs.append(Case(f"edge_delay_{edge}_{str(t).replace('.', 'p')}", f"edge_delay('B', {t}, {edge})", inputs=B,
                       ref=eref, note="m = ceil(t * rate) samples (edge_delay.form)",
                       expect=lambda e, s=t, r=(edge == "rise"): edge_delay(e.x("B"), s, e.rate, r)))
    cs.append(Case("edge_delay_comparison", "edge_delay('X' > 4, 0.2, rise)", inputs=X, ref=eref,
                   note="a Bool expression as the input (the help's throttle example shape)",
                   expect=lambda e: edge_delay([float(v > 4) for v in e.x("X")], 0.2, e.rate, True)))
    cs.append(Case("time_valid_basic", "time_valid('B', 0.3)", inputs=B, ref=f"{HELP_FN}: time_valid",
                   note="time_valid(x, t) is edge_delay(x, t, rise)",
                   expect=lambda e: edge_delay(e.x("B"), 0.3, e.rate, True)))
    tref = f"{HELP_FN}: time_shift"
    for t in (0.2, -0.3, 0.25, 0.0):
        cs.append(Case(f"time_shift_{str(t).replace('-', 'm').replace('.', 'p')}", f"time_shift('X', {t})", inputs=X,
                       ref=tref, note="positive t delays; round(t * rate) half away from zero (time_shift.sign)",
                       expect=lambda e, s=t: time_shift(e.x("X"), s, e.rate)))
    cs.append(Case("pulse_width_completed", "pulse_width('B')", inputs=B, ref=f"{HELP_FN}: pulse_width",
                   kind="release-notes", note="width of the latest completed high pulse (pulse_width.form)",
                   expect=lambda e: pulse_width(e.x("B"), e.rate)))
    cs.append(Case("previous_sample_default", "previous_sample('X')", inputs=X, ref=f"{HELP_FN}: previous_sample",
                   kind="release-notes", note="x[i-1]; the first sample invalid",
                   expect=lambda e: previous_sample(e.x("X"), 1)))
    cs.append(Case("previous_sample_n", "previous_sample('X', 3)", inputs=X, ref=f"{HELP_FN}: previous_sample",
                   kind="release-notes", note="x[i-n]; i < n invalid", expect=lambda e: previous_sample(e.x("X"), 3)))
    return cs


def range_cases() -> list[Case]:
    cs: list[Case] = []
    X = {"X": sig(W12)}
    laps = {"Outings": [(0, 1_200_000)], "Outings:Laps": [(0, 400_000), (400_000, 800_000), (800_000, 1_200_000)]}
    gaps = {"Outings": [(0, 1_200_000)], "Outings:Laps": [(100_000, 300_000), (500_000, 600_000), (900_000, 1_100_000)]}
    corners = {"Outings": [(0, 1_200_000)], "Outings:Laps": [(0, 1_200_000)],
               "Outings:Laps:Track Sections": [], "Outings:Laps:Track Sections:Default": [(0, 1_200_000)],
               "Outings:Laps:Track Sections:Default:Corners": [(200_000, 400_000), (700_000, 900_000)]}
    ref = f"{HELP_FN}: range_is"
    cs.append(Case("range_is_laps", "range_is(\"Outings:Laps\")", inputs=X, ranges=laps, ref=ref,
                   note="contiguous laps: always 1", expect=lambda e: range_is(e.t, e.ranges("Outings:Laps"))))
    cs.append(Case("range_is_gaps", "range_is(\"Outings:Laps\")", inputs=X, ranges=gaps, ref=ref,
                   note="1 in [start, end) of any range, 0 elsewhere",
                   expect=lambda e: range_is(e.t, e.ranges("Outings:Laps"))))
    cs.append(Case("range_is_track_sections_default_corners",
                   "range_is(\"Outings:Laps:Track Sections:Default:Corners\")", inputs=X, ranges=corners, ref=ref,
                   kind="help", note="the help's path, against the per-category child group (02 section 4.5)",
                   expect=lambda e: range_is(e.t, e.ranges("Outings:Laps:Track Sections:Default:Corners"))))
    cref = f"{HELP_FN}: range_change"
    cs.append(Case("range_change_laps", "range_change(\"Outings:Laps\")", inputs=X, ranges=laps, ref=cref,
                   note="increments at every range start: the lap ordinal",
                   expect=lambda e: range_change(e.t, e.ranges("Outings:Laps"))))
    cs.append(Case("range_change_gaps", "range_change(\"Outings:Laps\")", inputs=X, ranges=gaps, ref=cref,
                   note="0 before the first start; counts starts at or before the sample",
                   expect=lambda e: range_change(e.t, e.ranges("Outings:Laps"))))
    cs.append(Case("range_is_gate_for_stat", "stat_max('X', range_is(\"Outings:Laps\"))", inputs=X, ranges=gaps,
                   ref=ref, note="range_is as a gate", expect=lambda e: stat("max", e.x("X"), e.t,
                                                                              range_is(e.t, e.ranges("Outings:Laps")))))
    cs.append(Case("range_unknown_path", "range_is(\"Outings:Stints\")", inputs=X, ranges=laps, ref=f"{DESIGN} 2.4 rule 3",
                   note="a path the source does not have is E-RANGE-UNKNOWN", error="E-RANGE-UNKNOWN"))
    return cs


def table_cases() -> list[Case]:
    cs: list[Case] = []
    A = {"P": sig([-5, 0, 5, 10, 15, 20, 27.5, 30, 35, 40, 45, 12], unit="m")}
    axis = {"unit": "m", "breakpoints": [0, 10, 20, 30, 40]}
    vals = {"unit": "N", "data": [0, 850, 1700, 2600, 3600]}

    def item(interp: str, oor: str, ax: dict = axis, vv: dict = vals) -> dict:
        return {"kind": "table2d", "axis": ax, "values": vv, "interpolation": interp, "outOfRange": oor}

    for interp in ("linear", "next", "previous"):
        for oor in ("clamp", "invalid"):
            cs.append(Case(f"table2d_{interp}_{oor}", "'P'[m]", inputs=A, item=item(interp, oor),
                           ref=HELP_TABLES, note=f"{interp} lookup, exact breakpoints included; out of range: {oor} "
                           "(table.out-of-range)",
                           expect=lambda e, i=interp, o=oor: table(e.x("P"), axis["breakpoints"], vals["data"], i, o),
                           diagnostics=[("W-UNIT-MISSING", 0)]))
    cs.append(Case("table2d_axis_unit_mm", "'P'[mm]", inputs={"P": sig([0.0078125, 0.015625, 0.0234375, 0.046875], unit="m")},
                   item=item("linear", "clamp", {"unit": "mm", "breakpoints": [0, 10, 20, 30, 40]}, vals),
                   ref=HELP_TABLES, note="the axis expression is written in the axis unit ([mm]; table.axis-units)",
                   expect=lambda e: table([v * 1000 for v in e.x("P")], [0, 10, 20, 30, 40], vals["data"])))
    cs.append(Case("table2d_nonuniform", "'P'[m]", inputs=A,
                   item=item("linear", "clamp", {"unit": "m", "breakpoints": [0, 5, 12, 30, 45]},
                             {"unit": "", "data": [1, 2, 4, 3, 10]}),
                   ref=HELP_TABLES, note="breakpoints need not be evenly spaced",
                   expect=lambda e: table(e.x("P"), [0, 5, 12, 30, 45], [1, 2, 4, 3, 10], "linear", "clamp")))
    cs.append(Case("table2d_values_unit", "'P'[m]", inputs=A,
                   item=item("linear", "clamp", {"unit": "m", "breakpoints": [0, 10, 20, 40]},
                             {"unit": "km/h", "data": [0, 36, 72, 144]}),
                   ref=f"{DESIGN} 4.4", note="the values are stored in their quantity's base unit (km/h -> m/s)",
                   expect=lambda e: [v / 3.6 for v in table(e.x("P"), [0, 10, 20, 40], [0, 36, 72, 144])]))
    cs.append(Case("table2d_invalid_axis_sample", "'P'[m]",
                   inputs={"P": sig([0, 5, 10, 15, 20, 25], unit="m", valid=[1, 0, 1, 1, 0, 1])},
                   item=item("linear", "clamp", {"unit": "m", "breakpoints": [0, 10, 20]},
                             {"unit": "", "data": [0, 1, 4]}),
                   ref=f"{DESIGN} 4.4", note="an invalid axis sample gives an invalid output",
                   expect=lambda e: table(e.x("P"), [0, 10, 20], [0, 1, 4])))
    cs.append(Case("table2d_axis_expression", "abs('P'[m] - 20)", inputs=A,
                   item=item("previous", "clamp", {"unit": "m", "breakpoints": [0, 10, 20]},
                             {"unit": "", "data": [5, 6, 7]}),
                   ref=f"{DESIGN} 4.4", note="the axis is a full expression",
                   expect=lambda e: table([abs(v - 20) for v in e.x("P")], [0, 10, 20], [5, 6, 7], "previous")))
    cs.append(Case("table2d_axis_descending", "'P'[m]", inputs=A,
                   item=item("linear", "clamp", {"unit": "m", "breakpoints": [0, 20, 10]},
                             {"unit": "", "data": [0, 1, 2]}),
                   ref=f"{DESIGN} 4.4", note="breakpoints not ascending: E-TABLE-AXIS", error="E-TABLE-AXIS"))
    return cs


def variance_cases() -> list[Case]:
    cs: list[Case] = []
    rate = R10
    # Two laps of 1 s each at 10 Hz; lap 2 runs faster in distance.
    x = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30]
    dist = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 112.5, 125, 137.5, 150, 162.5, 175, 187.5, 200, 212.5, 225]
    laps = {"Outings": [(0, 2_000_000)], "Outings:Laps": [(0, 1_000_000), (1_000_000, 2_000_000)]}
    vref = f"{HELP_FN}: variance_time"
    cs.append(Case("variance_time_reference", "variance_time('X')", inputs={"X": sig(x, rate)}, ranges=laps,
                   selection={"main": "Outings:Laps/2", "reference": "Outings:Laps/1"}, ref=vref, kind="release-notes",
                   note="x on Main minus x on the reference at the same time from the lap start (D-081)",
                   expect=lambda e: variance(e.x("X"), rate, e.sel("main"), e.sel("reference"), False)))
    cs.append(Case("variance_time_offsets", "variance_time('X')", inputs={"X": sig(x, rate)}, ranges=laps,
                   selection={"main": {"range": "Outings:Laps/2", "offsetUs": 200_000},
                              "reference": {"range": "Outings:Laps/1", "offsetUs": 50_000}},
                   ref=vref, kind="release-notes", note="alignment offsets of both laps (04 section 3.1 mapping)",
                   expect=lambda e: variance(e.x("X"), rate, e.sel("main"), e.sel("reference"), False)))
    cs.append(Case("variance_time_no_reference", "variance_time('X')", inputs={"X": sig(x, rate)}, ranges=laps,
                   selection={"main": "Outings:Laps/2"}, ref=vref, kind="release-notes",
                   note="no reference overlay: invalid everywhere", expect=lambda e: [NaN] * e.n))
    dref = f"{HELP_FN}: variance_dist"

    def vd(e: Env) -> list[float]:
        m, r = e.sel("main"), e.sel("reference")
        times = [time_at(i, rate) for i in range(len(dist))]
        md = LapDistance(times, dist, m["start"], m["end"])
        rd = LapDistance(times, dist, r["start"], r["end"])
        return variance(e.x("X"), rate, m, r, True, md, rd)

    cs.append(Case("variance_dist_reference", "variance_dist('X')",
                   inputs={"X": sig(x, rate), "Corr Dist": sig(dist, rate, dtype="f64", unit="m")}, ranges=laps,
                   selection={"main": "Outings:Laps/2", "reference": "Outings:Laps/1"}, ref=dref, kind="release-notes",
                   note="x on Main minus x on the reference at the same distance from the lap start (D-081)",
                   expect=vd))
    cs.append(Case("variance_dist_stretch_offset", "variance_dist('X')",
                   inputs={"X": sig(x, rate), "Corr Dist": sig(dist, rate, dtype="f64", unit="m")}, ranges=laps,
                   selection={"main": {"range": "Outings:Laps/2", "offsetM": 5.0, "stretch": 0.8},
                              "reference": {"range": "Outings:Laps/1", "offsetM": 2.0}},
                   ref=dref, kind="release-notes", note="distance offsets and the Main lap's stretch", expect=vd))
    return cs


def diagnostic_cases() -> list[Case]:
    X = {"X": sig(X8)}
    cs = [
        Case("err_arity", "smooth('X')", inputs=X, ref=f"{DESIGN} 2.5", note="E-ARITY lists the accepted arities",
             error="E-ARITY"),
        Case("err_arg_type_derivative_constant", "derivative(1)", inputs=X, ref=f"{HELP_FN}: derivative",
             kind="help", note="the help's example of an argument type error", error="E-ARG-TYPE"),
        Case("err_unknown_function", "smoth('X', 3)", inputs=X, ref=f"{DESIGN} 2.5",
             note="E-UNKNOWN-FUNCTION with the nearest name", error="E-UNKNOWN-FUNCTION"),
        Case("err_quote_missing", "X * 2", inputs=X, ref=HELP_EXPR, note="a bare name: E-QUOTE-MISSING",
             error="E-QUOTE-MISSING"),
        Case("err_unit_bracket", "'X' km/h", inputs=X, ref=HELP_EXPR, note="a unit without brackets",
             error="E-UNIT-BRACKET"),
        Case("err_paren_unbalanced", "('X' + 1", inputs=X, ref=HELP_EXPR, note="E-PAREN-UNBALANCED",
             error="E-PAREN-UNBALANCED"),
        Case("err_integer_required", "'X' & 3", inputs={"X": sig([1.5, 2, 3, 4])}, ref=HELP_OPS,
             note="& | ~ need integer operands", error="E-INTEGER-REQUIRED"),
        Case("err_unknown_channel", "'Nope' + 1", inputs=X, ref=f"{DESIGN} 2.5",
             note="an unknown name with data loaded", error="E-UNKNOWN-CHANNEL"),
        Case("err_edge_keyword", "edge_delay('X' > 1, 0.5, up)", inputs=X, ref=f"{HELP_FN}: edge_delay",
             note="the third argument is rise or fall", error="E-QUOTE-MISSING"),
        Case("err_window_negative", "smooth('X', -3)", inputs=X, ref=f"{DESIGN} 2.4",
             note="a negative window", error="E-ARG-VALUE"),
    ]
    return cs


def corpus() -> list[Case]:
    cases = (elementwise_cases() + operator_cases() + windowed_cases() + iir_cases() + stateful_cases() +
             range_cases() + table_cases() + variance_cases() + diagnostic_cases())
    seen: set[str] = set()
    for c in cases:
        if c.id in seen:
            raise ValueError(f"duplicate case id {c.id}")
        seen.add(c.id)
    return cases


def render(case: Case) -> str:
    return dumps(case.to_json()) + "\n"


def differs(a: Any, b: Any, ptr: str = "") -> Optional[str]:
    """The JSON pointer of the first difference (numbers within 1e-9 relative), or None."""
    if isinstance(a, bool) or isinstance(b, bool) or a is None or b is None or isinstance(a, str):
        return None if a == b and type(a) is type(b) else ptr
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return None if abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b)) else ptr
    if isinstance(a, dict) and isinstance(b, dict):
        if list(a) != list(b):
            return ptr
        for k in a:
            d = differs(a[k], b[k], f"{ptr}/{k}")
            if d is not None:
                return d
        return None
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return ptr
        for k, (x, y) in enumerate(zip(a, b)):
            d = differs(x, y, f"{ptr}/{k}")
            if d is not None:
                return d
        return None
    return ptr


# ---- self-test -----------------------------------------------------------------------------------------------
def self_test() -> int:
    failures = 0

    def check(cond: bool, what: str) -> None:
        nonlocal failures
        if not cond:
            failures += 1
            print(f"self-test FAILED: {what}")

    # Time grid: 3 Hz rounds half up; indexAt inverts timeAt.
    check([time_at(i, (3, 1)) for i in range(4)] == [0, 333333, 666667, 1000000], "timeAt at 3 Hz")
    check(all(index_at(time_at(i, (30, 1)), (30, 1)) == i for i in range(100)), "indexAt(timeAt(i)) == i")
    # Windows.
    check(window_points(("seconds", 0.3), R10) == 3 and window_points(("seconds", 0.1), R10) == 2, "seconds -> points")
    check(window_bounds(5, 4, 12) == (3, 6) and window_bounds(5, 3, 12) == (4, 6), "window placement")
    # Butterworth order 2 = the bilinear biquad with Q = 1/sqrt(2) (closed form).
    fs, fc = 100.0, 6.0
    tf = cheby_transfer("lp", 2, 0.0, fc, fc, fs)
    w0 = 2 * math.pi * fc / fs
    alpha = math.sin(w0) / (2 / math.sqrt(2))
    a0 = 1 + alpha
    want_b = [(1 - math.cos(w0)) / 2 / a0, (1 - math.cos(w0)) / a0, (1 - math.cos(w0)) / 2 / a0]
    want_a = [1.0, -2 * math.cos(w0) / a0, (1 - alpha) / a0]
    check(all(abs(p - q) < 1e-12 for p, q in zip(tf.b, want_b)), "Butterworth 2 numerator")
    check(all(abs(p - q) < 1e-12 for p, q in zip(tf.a, want_a)), "Butterworth 2 denominator")
    check(abs(abs(tf.response(w0)) - 1 / math.sqrt(2)) < 1e-12, "Butterworth -3 dB at fc")
    # Chebyshev: DC gain 1, gain 1 at the ripple edge, peak 1 + ripple in the pass band.
    for order in (2, 4, 6):
        tf = cheby_transfer("lp", order, 0.5, fc, fc, fs)
        check(abs(abs(tf.response(0.0)) - 1) < 1e-9, f"Chebyshev {order} DC gain")
        check(abs(abs(tf.response(2 * math.pi * fc / fs)) - 1) < 1e-9, f"Chebyshev {order} edge gain")
        peak = max(abs(tf.response(2 * math.pi * f / fs)) for f in [fc * k / 400 for k in range(401)])
        check(abs(peak - 1.005) < 2e-4, f"Chebyshev {order} ripple peak {peak}")
    hp = cheby_transfer("hp", 4, 0.0, 3.0, 3.0, fs)
    check(abs(abs(hp.response(math.pi)) - 1) < 1e-9 and abs(hp.response(0.0)) < 1e-9, "high pass gains")
    bp = cheby_transfer("bp", 2, 0.0, 4.0, 9.0, fs)
    w_c = 2 * math.atan(math.sqrt(math.tan(math.pi * 4 / fs) * math.tan(math.pi * 9 / fs)))
    check(abs(abs(bp.response(w_c)) - 1) < 1e-9 and abs(bp.response(0.0)) < 1e-9, "band pass gains")
    check(abs(abs(bp.response(2 * math.pi * 4 / fs)) - 1 / math.sqrt(2)) < 1e-9, "band pass -3 dB at f_lo")
    bs = cheby_transfer("bs", 2, 0.0, 5.0, 9.0, fs)
    check(abs(abs(bs.response(0.0)) - 1) < 1e-9 and abs(bs.response(w_c * 0 + 2 * math.atan(
        math.sqrt(math.tan(math.pi * 5 / fs) * math.tan(math.pi * 9 / fs))))) < 1e-9, "band stop gains")
    # RC low pass: the classic recursion y += a (x - y).
    x = [0.0, 1.0, 1.0, 1.0, 0.0]
    y = filter_rc(x, False, 2.0, 20.0, 1)
    a = (1 / 20) / (1 / (2 * math.pi * 2) + 1 / 20)
    want, prev = [], 0.0
    for v in x:
        prev = prev + a * (v - prev)
        want.append(prev)
    check(all(abs(p - q) < 1e-12 for p, q in zip(y, want)), "RC low pass recursion")
    # Zero phase: a symmetric pulse with SYMMETRY_PAD zeros on each side gives a symmetric output for every
    # zero-phase order (the end primings have decayed); the RC and Chebyshev forms alike.
    pulse = [0.0] * SYMMETRY_PAD + [1.0] * 11 + [0.0] * SYMMETRY_PAD
    long_pulse = [0.0] * (4 * SYMMETRY_PAD) + [1.0] * 11 + [0.0] * (4 * SYMMETRY_PAD)
    outputs = {"RC -2": filter_rc(pulse, False, 4.0, 100.0, -2)}
    for order in (-4, -8, -12):
        outputs[f"Chebyshev {order}"] = filter_cheby(long_pulse if order == -12 else pulse, "lp", 100.0, 6.0, 0.0,
                                                     order, 0.5)
    for what, zp in outputs.items():
        asym = max(abs(zp[i] - zp[-1 - i]) for i in range(len(zp)))
        check(asym < 1e-9, f"zero phase symmetry, {what} (asymmetry {asym:.3g})")
    # Variance helpers.
    ld = LapDistance([0, 100_000, 200_000], [0.0, 10.0, 20.0], 0, 200_000)
    check(abs(ld.dist_at(50_000) - 5.0) < 1e-12 and abs(ld.time_at(15.0) - 0.15) < 1e-12, "lap distance")
    count = len(corpus())
    check(count >= 150, f"at least 150 cases ({count})")
    print(f"maths_ref self-test: {failures} failure(s), {count} cases")
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", metavar="DIR")
    ap.add_argument("--check", metavar="DIR")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.list:
        for c in corpus():
            print(c.id)
        return 0
    if args.write:
        out = pathlib.Path(args.write)
        out.mkdir(parents=True, exist_ok=True)
        ids = set()
        for c in corpus():
            ids.add(c.id)
            (out / f"{c.id}.case.json").write_bytes(render(c).encode("utf-8"))
        stale = [p for p in out.glob("*.case.json") if p.name[: -len(".case.json")] not in ids]
        for p in stale:
            print(f"maths_ref: {p.name} is not generated by this script (left in place)")
        print(f"maths_ref: wrote {len(ids)} case files to {out}")
        return 0
    if args.check:
        out = pathlib.Path(args.check)
        bad = 0
        ids = set()
        for c in corpus():
            ids.add(c.id)
            p = out / f"{c.id}.case.json"
            if not p.is_file():
                print(f"maths_ref: {p.name} is missing (rerun --write)")
                bad += 1
                continue
            # Numbers compare within 1e-9 relative: the platform's libm may differ in the last bit of the
            # transcendental functions the inputs and expectations use; anything else must be equal.
            where = differs(json.loads(p.read_text(encoding="utf-8")), json.loads(render(c)))
            if where is not None:
                print(f"maths_ref: {p.name} differs from the reference at {where or '/'} (rerun --write)")
                bad += 1
        for p in out.glob("*.case.json"):
            if p.name[: -len(".case.json")] not in ids:
                print(f"maths_ref: {p.name} has no generator")
                bad += 1
        print(f"maths_ref: {len(ids)} cases, {bad} finding(s)")
        return 1 if bad else 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
