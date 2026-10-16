#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Independent least-squares reference for the scatter trend lines (AC-M3-06, 04 section 10.1 item 4, D-041).

Writes libs/analysis/tests/data/trend_fit_cases.json: deterministic (x, y) point sets with the least-squares
coefficients of a linear or polynomial (order 2-6) trend, with and without the through-zero constraint, and
the coefficient of determination. The expectations never come from the C++ under test: this script solves the
normal equations in exact rational arithmetic (fractions.Fraction over the exact binary values of the inputs),
so the reference has no rounding at all until the final conversion of each coefficient to the nearest double.
The C++ side (analysis::TrendFit, QR by Givens rotations on scaled x) must agree within 1e-6 relative.

Usage (standard library only, run with python -I):
  python -I tools/trend_ref.py --write libs/analysis/tests/data/trend_fit_cases.json
  python -I tools/trend_ref.py --check libs/analysis/tests/data/trend_fit_cases.json   exit 1 when it differs
  python -I tools/trend_ref.py --self-test                                              checks of the solver
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys
from fractions import Fraction

COMMENT = ("Written by tools/trend_ref.py (exact rational least squares; AC-M3-06). Do not edit: regenerate "
           "with python -I tools/trend_ref.py --write <this file>.")


class Rng:
    """splitmix64: deterministic on every platform and Python version."""

    def __init__(self, seed: int) -> None:
        self.state = seed & 0xFFFFFFFFFFFFFFFF

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        return z ^ (z >> 31)

    def uniform(self) -> float:
        """[0, 1) with 53 random bits."""
        return (self.next() >> 11) / float(1 << 53)


def solve_exact(matrix: list[list[Fraction]], rhs: list[Fraction]) -> list[Fraction]:
    """Gauss-Jordan elimination in exact arithmetic; raises ValueError for a singular system."""
    n = len(rhs)
    a = [row[:] + [rhs[i]] for i, row in enumerate(matrix)]
    for col in range(n):
        pivot = next((r for r in range(col, n) if a[r][col] != 0), None)
        if pivot is None:
            raise ValueError("singular normal equations")
        a[col], a[pivot] = a[pivot], a[col]
        p = a[col][col]
        a[col] = [v / p for v in a[col]]
        for r in range(n):
            if r != col and a[r][col] != 0:
                f = a[r][col]
                a[r] = [vr - f * vc for vr, vc in zip(a[r], a[col])]
    return [a[i][n] for i in range(n)]


def least_squares(xs: list[float], ys: list[float], degree: int,
                  through_zero: bool) -> tuple[list[Fraction], Fraction]:
    """Exact coefficients c[0..degree] (c[0] = 0 through zero) and r^2 = 1 - SSres / SStot."""
    powers = list(range(1 if through_zero else 0, degree + 1))
    fx = [Fraction(v) for v in xs]
    fy = [Fraction(v) for v in ys]
    rows = [[x ** p for p in powers] for x in fx]
    gram = [[sum(r[i] * r[j] for r in rows) for j in range(len(powers))] for i in range(len(powers))]
    rhs = [sum(r[i] * y for r, y in zip(rows, fy)) for i in range(len(powers))]
    solution = solve_exact(gram, rhs)
    coefficients = [Fraction(0)] * (degree + 1)
    for p, c in zip(powers, solution):
        coefficients[p] = c
    mean = sum(fy) / len(fy)
    ss_tot = sum((y - mean) ** 2 for y in fy)
    ss_res = Fraction(0)
    for x, y in zip(fx, fy):
        fit = sum(c * x ** k for k, c in enumerate(coefficients))
        ss_res += (y - fit) ** 2
    r2 = Fraction(1) - ss_res / ss_tot if ss_tot != 0 else Fraction(1)
    return coefficients, r2


# (id, trend type, order, through zero, x range, true polynomial c[0..n], noise amplitude, points, seed)
CASES = [
    ("linear", "linear", 1, False, (-5.0, 5.0), [-0.5, 1.75], 0.3, 60, 1),
    ("linear_zero", "linear", 1, True, (-5.0, 5.0), [-0.5, 1.75], 0.3, 60, 2),
    ("poly2", "poly", 2, False, (-3.0, 4.0), [2.0, -1.2, 0.8], 0.25, 60, 3),
    ("poly2_zero", "poly", 2, True, (-3.0, 4.0), [2.0, -1.2, 0.8], 0.25, 60, 4),
    ("poly3", "poly", 3, False, (-4.0, 6.0), [-3.0, 1.0, -0.4, 0.05], 0.2, 80, 5),
    ("poly3_zero", "poly", 3, True, (-4.0, 6.0), [-3.0, 1.0, -0.4, 0.05], 0.2, 80, 6),
    ("poly4", "poly", 4, False, (-2.5, 3.0), [1.5, -2.0, 0.6, 0.3, -0.08], 0.1, 90, 7),
    ("poly4_zero", "poly", 4, True, (-2.5, 3.0), [1.5, -2.0, 0.6, 0.3, -0.08], 0.1, 90, 8),
    ("poly5", "poly", 5, False, (-2.0, 2.5), [0.7, 1.1, -0.9, 0.4, 0.15, -0.06], 0.05, 100, 9),
    ("poly5_zero", "poly", 5, True, (-2.0, 2.5), [0.7, 1.1, -0.9, 0.4, 0.15, -0.06], 0.05, 100, 10),
    ("poly6", "poly", 6, False, (-1.8, 2.2), [-1.0, 0.5, 1.3, -0.7, -0.2, 0.12, 0.03], 0.04, 120, 11),
    ("poly6_zero", "poly", 6, True, (-1.8, 2.2), [-1.0, 0.5, 1.3, -0.7, -0.2, 0.12, 0.03], 0.04, 120, 12),
    # Speed-like x (km/h) with a lateral-G-like y: the columns of the raw design matrix differ by 10^14.
    ("speed_poly3", "poly", 3, False, (20.0, 280.0), [0.1, 0.012, 2.5e-5, -6.0e-8], 0.05, 150, 13),
    ("speed_poly6_zero", "poly", 6, True, (20.0, 280.0),
     [0.0, 0.02, -8.0e-5, 4.0e-7, -1.0e-9, 1.5e-12, -6.0e-16], 0.02, 150, 14),
    # Engine speed in a narrow band far from 0, through zero: plain powers of x are nearly collinear there.
    ("rpm_band_poly6_zero", "poly", 6, True, (7900.0, 8000.0), [0.0, 1.0e-4], 0.01, 150, 15),
]


def make_case(case: tuple) -> dict:
    cid, kind, order, through_zero, (lo, hi), truth, noise, n, seed = case
    rng = Rng(0x7E9D_F17_0000 + seed)
    xs: list[float] = []
    ys: list[float] = []
    for i in range(n):
        # Spread over the range with jitter; values rounded to 6 decimals as a logger would write them.
        x = lo + (hi - lo) * (i + 0.5 * rng.uniform()) / n
        x = round(x, 6)
        y = sum(c * x ** k for k, c in enumerate(truth)) + noise * (2.0 * rng.uniform() - 1.0)
        xs.append(x)
        ys.append(round(y, 6))
    degree = 1 if kind == "linear" else order
    coefficients, r2 = least_squares(xs, ys, degree, through_zero)
    return {
        "id": cid,
        "type": kind,
        "order": order,
        "throughZero": through_zero,
        "x": xs,
        "y": ys,
        "coefficients": [float(c) for c in coefficients],
        "r2": float(r2),
        "samples": n,
    }


def document() -> str:
    doc = {"$comment": COMMENT, "cases": [make_case(c) for c in CASES]}
    return json.dumps(doc, indent=1) + "\n"


def self_test() -> int:
    failures = 0
    # Points exactly on y = 3 - 2x + 0.5x^2: the fit returns the polynomial and r^2 = 1.
    xs = [-2.0, -1.0, 0.0, 0.5, 1.0, 3.0]
    ys = [3.0 - 2.0 * x + 0.5 * x * x for x in xs]
    c, r2 = least_squares(xs, ys, 2, False)
    if c != [Fraction(3), Fraction(-2), Fraction(1, 2)] or r2 != 1:
        print(f"self-test: exact quadratic gave {c}, r2 {r2}")
        failures += 1
    # y = 2x + 1 through zero on x = 1, 2: slope = (1*3 + 2*5) / (1 + 4) = 13/5.
    c, _ = least_squares([1.0, 2.0], [3.0, 5.0], 1, True)
    if c != [Fraction(0), Fraction(13, 5)]:
        print(f"self-test: through-zero slope gave {c}")
        failures += 1
    # Singular: one distinct x for a line.
    try:
        least_squares([1.0, 1.0, 1.0], [1.0, 2.0, 3.0], 1, False)
        print("self-test: a singular system was not refused")
        failures += 1
    except ValueError:
        pass
    # The generator is deterministic.
    if document() != document():
        print("self-test: the document is not deterministic")
        failures += 1
    print("self-test: " + ("ok" if failures == 0 else f"{failures} failure(s)"))
    return 0 if failures == 0 else 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", type=pathlib.Path)
    group.add_argument("--check", type=pathlib.Path)
    group.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    text = document()
    if args.write:
        args.write.parent.mkdir(parents=True, exist_ok=True)
        args.write.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {args.write} ({len(CASES)} cases)")
        return 0
    try:
        current = args.check.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"{args.check}: {exc}")
        return 1
    if current != text:
        print(f"{args.check}: differs from what tools/trend_ref.py writes; regenerate it with --write")
        return 1
    print(f"{args.check}: current ({len(CASES)} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
