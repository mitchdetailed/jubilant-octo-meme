#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""The application icon (M4 packaging, D-212; 07 section 7.1 "Packaging", 08 section 7).

Our own drawing, from geometry only: a rounded blue tile with a white "S" of two touching arcs (a pair of
bends, as the name says) and an orange dot where the S starts. Every file in this folder is written by this
script from that geometry, so the icon has no other source:

  esses-<n>.png   n = 16, 32, 48, 64, 128, 256, 512 (the window icon, the Linux hicolor theme)
  esses.ico       16, 24, 32, 48 as 32-bit bitmaps, 64, 128, 256 as PNG (the Windows executable and installer)
  esses.icns      16 to 1024 as PNG entries (the macOS bundle)
  esses.svg       the same geometry as a vector (the Linux scalable icon)

Pixels come from signed distances with a one-pixel ramp (no supersampling), straight (not premultiplied) RGBA.

  python -I tools/packaging/icons/make_icons.py --write    regenerate the files
  python -I tools/packaging/icons/make_icons.py --check    compare the committed files with a fresh render
                                                           (pixels within 1 of 255 per channel: PNG bytes
                                                           depend on the zlib build, the pixels do not)
Standard library only. Exit status: 0 clean, 1 a difference, 2 usage error.
"""
from __future__ import annotations

import argparse
import math
import pathlib
import struct
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent
PNG_SIZES = (16, 32, 48, 64, 128, 256, 512)
ICO_BMP_SIZES = (16, 24, 32, 48)
ICO_PNG_SIZES = (64, 128, 256)
# (type, size) of the icns entries: icp4/icp5/icp6 16/32/64, ic07-ic10 128-1024, ic11-ic14 the @2x variants.
ICNS_ENTRIES = (("icp4", 16), ("ic11", 32), ("icp5", 32), ("ic12", 64), ("ic07", 128), ("ic13", 256),
                ("ic08", 256), ("ic14", 512), ("ic09", 512), ("ic10", 1024))

# ---- geometry (unit square, y downwards) ---------------------------------------------------------------
MARGIN = 0.0625          # transparent border around the tile
CORNER = 0.20            # tile corner radius
TOP = (0x2B, 0x5C, 0x8A)     # tile gradient, top
BOTTOM = (0x16, 0x34, 0x55)  # tile gradient, bottom
WHITE = (0xFF, 0xFF, 0xFF)
ORANGE = (0xF2, 0x8C, 0x28)
RADIUS = 0.145           # radius of both arcs (centre line of the stroke)
STROKE = 0.115           # stroke width
UPPER = (0.5, 0.5 - RADIUS)  # centres: the arcs touch at (0.5, 0.5)
LOWER = (0.5, 0.5 + RADIUS)
# Angles in degrees, mathematical sense on screen (0 = right, 90 = up), i.e. y negated.
UPPER_ARC = (25.0, 270.0)    # counter-clockwise from 25 degrees over the top and the left to the bottom
LOWER_ARC = (-155.0, 90.0)   # clockwise from the top over the right and the bottom to -155 degrees
DOT_RADIUS = 0.075


def _angle_in(theta: float, lo: float, hi: float) -> bool:
    """theta (degrees) inside the counter-clockwise sweep lo..hi."""
    span = (hi - lo) % 360.0
    return (theta - lo) % 360.0 <= span


def _arc_distance(x: float, y: float, centre: tuple[float, float], lo: float, hi: float) -> float:
    dx, dy = x - centre[0], centre[1] - y  # y up
    theta = math.degrees(math.atan2(dy, dx))
    if _angle_in(theta, lo, hi):
        return abs(math.hypot(dx, dy) - RADIUS)
    best = math.inf
    for a in (lo, hi):
        ex = centre[0] + RADIUS * math.cos(math.radians(a))
        ey = centre[1] - RADIUS * math.sin(math.radians(a))
        best = min(best, math.hypot(x - ex, y - ey))
    return best


def _start_point() -> tuple[float, float]:
    a = math.radians(UPPER_ARC[0])
    return UPPER[0] + RADIUS * math.cos(a), UPPER[1] - RADIUS * math.sin(a)


def _rounded_box(x: float, y: float) -> float:
    """Signed distance to the tile (negative inside)."""
    half = 0.5 - MARGIN - CORNER
    qx = abs(x - 0.5) - half
    qy = abs(y - 0.5) - half
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    return outside + min(max(qx, qy), 0.0) - CORNER


def _cover(signed: float, n: int) -> float:
    """Coverage of a pixel whose centre lies `signed` (unit square) from an edge: a ramp one pixel wide."""
    return min(1.0, max(0.0, 0.5 - signed * n))


def _over(dst: list[float], colour: tuple[int, int, int], alpha: float) -> None:
    """Source-over of a straight colour with coverage alpha onto dst = [r, g, b, a] (straight, 0..1)."""
    if alpha <= 0.0:
        return
    da = dst[3]
    oa = alpha + da * (1.0 - alpha)
    for i in range(3):
        src = colour[i] / 255.0
        dst[i] = (src * alpha + dst[i] * da * (1.0 - alpha)) / oa if oa > 0.0 else 0.0
    dst[3] = oa


def render(n: int) -> bytes:
    """n x n straight RGBA, rows top to bottom."""
    stroke = max(STROKE, 1.75 / n)  # at least 1.75 px wide, so 16 px stays legible
    sx, sy = _start_point()
    out = bytearray()
    for j in range(n):
        y = (j + 0.5) / n
        t = (y - MARGIN) / (1.0 - 2.0 * MARGIN)
        t = min(1.0, max(0.0, t))
        tile = tuple(round(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))
        for i in range(n):
            x = (i + 0.5) / n
            px = [0.0, 0.0, 0.0, 0.0]
            _over(px, tile, _cover(_rounded_box(x, y), n))
            d = min(_arc_distance(x, y, UPPER, *UPPER_ARC), _arc_distance(x, y, LOWER, *LOWER_ARC))
            _over(px, WHITE, _cover(d - stroke / 2.0, n))
            _over(px, ORANGE, _cover(math.hypot(x - sx, y - sy) - DOT_RADIUS, n))
            out += bytes(min(255, max(0, round(c * 255.0))) for c in px)
    return bytes(out)


def svg_text() -> str:
    def pt(centre: tuple[float, float], a: float) -> str:
        r = math.radians(a)
        return f"{(centre[0] + RADIUS * math.cos(r)) * 256:.2f} {(centre[1] - RADIUS * math.sin(r)) * 256:.2f}"

    def hexc(c: tuple[int, int, int]) -> str:
        return "#%02x%02x%02x" % c

    m, c, r = MARGIN * 256, CORNER * 256, RADIUS * 256
    sx, sy = _start_point()
    # The upper arc runs counter-clockwise on screen (sweep 0), 245 degrees (large); the lower clockwise, 245.
    path = (f"M {pt(UPPER, UPPER_ARC[0])} A {r:.2f} {r:.2f} 0 1 0 {pt(UPPER, UPPER_ARC[1])} "
            f"A {r:.2f} {r:.2f} 0 1 1 {pt(LOWER, LOWER_ARC[0])}")
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        "<!-- SPDX-FileCopyrightText: 2026 the Esses contributors -->\n"
        "<!-- SPDX-License-Identifier: MIT -->\n"
        "<!-- Written by tools/packaging/icons/make_icons.py; edit the geometry there. -->\n"
        '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">\n'
        '  <defs>\n'
        '    <linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">\n'
        f'      <stop offset="0" stop-color="{hexc(TOP)}"/>\n'
        f'      <stop offset="1" stop-color="{hexc(BOTTOM)}"/>\n'
        '    </linearGradient>\n'
        '  </defs>\n'
        f'  <rect x="{m:.2f}" y="{m:.2f}" width="{256 - 2 * m:.2f}" height="{256 - 2 * m:.2f}" rx="{c:.2f}" '
        'fill="url(#tile)"/>\n'
        f'  <path d="{path}" fill="none" stroke="{hexc(WHITE)}" stroke-width="{STROKE * 256:.2f}" '
        'stroke-linecap="round"/>\n'
        f'  <circle cx="{sx * 256:.2f}" cy="{sy * 256:.2f}" r="{DOT_RADIUS * 256:.2f}" fill="{hexc(ORANGE)}"/>\n'
        "</svg>\n")


# ---- file formats -----------------------------------------------------------------------------------------

def png_bytes(n: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    raw = b"".join(b"\x00" + rgba[row * n * 4:(row + 1) * n * 4] for row in range(n))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def png_decode(data: bytes) -> tuple[int, int, bytes]:
    """(width, height, straight RGBA) of an 8-bit RGBA, non-interlaced PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if (depth, colour, interlace) != (8, 6, 0):
                raise ValueError("only 8-bit RGBA without interlace")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    prev = bytearray(stride)
    for row in range(height):
        f = raw[row * (stride + 1)]
        line = bytearray(raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        out += line
        prev = line
    return width, height, bytes(out)


def bmp_entry(n: int, rgba: bytes) -> bytes:
    """An ICO image as a 32-bit DIB: BITMAPINFOHEADER, BGRA rows bottom-up, then the 1-bit AND mask."""
    header = struct.pack("<IiiHHIIiiII", 40, n, 2 * n, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for row in range(n - 1, -1, -1):
        for col in range(n):
            r, g, b, a = rgba[(row * n + col) * 4:(row * n + col) * 4 + 4]
            xor += bytes((b, g, r, a))
    mask_stride = ((n + 31) // 32) * 4
    mask = bytearray()
    for row in range(n - 1, -1, -1):
        line = bytearray(mask_stride)
        for col in range(n):
            if rgba[(row * n + col) * 4 + 3] == 0:
                line[col // 8] |= 0x80 >> (col % 8)
        mask += line
    return header + bytes(xor) + bytes(mask)


def bmp_decode(data: bytes) -> tuple[int, bytes]:
    size, width, height2, _, bits = struct.unpack("<IiiHH", data[:16])
    if size != 40 or bits != 32 or height2 != 2 * width:
        raise ValueError("not a 32-bit icon bitmap")
    n = width
    out = bytearray(n * n * 4)
    for k, row in enumerate(range(n - 1, -1, -1)):
        for col in range(n):
            b, g, r, a = data[40 + (k * n + col) * 4:40 + (k * n + col) * 4 + 4]
            out[(row * n + col) * 4:(row * n + col) * 4 + 4] = bytes((r, g, b, a))
    return n, bytes(out)


def ico_bytes(images: dict[int, bytes]) -> bytes:
    entries = []
    for n in ICO_BMP_SIZES:
        entries.append((n, bmp_entry(n, images[n])))
    for n in ICO_PNG_SIZES:
        entries.append((n, png_bytes(n, images[n])))
    out = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    body = b""
    for n, data in entries:
        dim = 0 if n >= 256 else n
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    return out + body


def ico_decode(data: bytes) -> dict[int, bytes]:
    _, kind, count = struct.unpack("<HHH", data[:6])
    if kind != 1:
        raise ValueError("not an icon file")
    images: dict[int, bytes] = {}
    for k in range(count):
        w, _, _, _, _, bits, length, offset = struct.unpack("<BBBBHHII", data[6 + 16 * k:22 + 16 * k])
        n = 256 if w == 0 else w
        entry = data[offset:offset + length]
        if bits != 32:
            raise ValueError("an entry is not 32-bit")
        if entry[:8] == b"\x89PNG\r\n\x1a\n":
            w2, h2, rgba = png_decode(entry)
            if (w2, h2) != (n, n):
                raise ValueError("PNG entry size differs from its directory entry")
        else:
            n2, rgba = bmp_decode(entry)
            if n2 != n:
                raise ValueError("bitmap entry size differs from its directory entry")
        images[n] = rgba
    return images


def icns_bytes(images: dict[int, bytes]) -> bytes:
    body = b""
    for kind, n in ICNS_ENTRIES:
        data = png_bytes(n, images[n])
        body += kind.encode("ascii") + struct.pack(">I", len(data) + 8) + data
    return b"icns" + struct.pack(">I", len(body) + 8) + body


def icns_decode(data: bytes) -> list[tuple[str, int, bytes]]:
    if data[:4] != b"icns" or struct.unpack(">I", data[4:8])[0] != len(data):
        raise ValueError("not an icns file")
    pos, out = 8, []
    while pos < len(data):
        kind = data[pos:pos + 4].decode("ascii")
        length = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        w, h, rgba = png_decode(data[pos + 8:pos + length])
        if w != h:
            raise ValueError(f"{kind}: not square")
        out.append((kind, w, rgba))
        pos += length
    return out


# ---- driver ---------------------------------------------------------------------------------------------

def all_sizes() -> list[int]:
    return sorted(set(PNG_SIZES) | set(ICO_BMP_SIZES) | set(ICO_PNG_SIZES) | {n for _, n in ICNS_ENTRIES})


def write(folder: pathlib.Path) -> None:
    images = {n: render(n) for n in all_sizes()}
    for n in PNG_SIZES:
        (folder / f"esses-{n}.png").write_bytes(png_bytes(n, images[n]))
    (folder / "esses.ico").write_bytes(ico_bytes(images))
    (folder / "esses.icns").write_bytes(icns_bytes(images))
    (folder / "esses.svg").write_text(svg_text(), encoding="utf-8", newline="\n")


def _close(a: bytes, b: bytes) -> bool:
    return len(a) == len(b) and all(abs(x - y) <= 1 for x, y in zip(a, b))


def check(folder: pathlib.Path) -> list[str]:
    problems: list[str] = []
    images = {n: render(n) for n in all_sizes()}
    for n in PNG_SIZES:
        path = folder / f"esses-{n}.png"
        try:
            w, h, rgba = png_decode(path.read_bytes())
        except (OSError, ValueError, zlib.error) as e:
            problems.append(f"{path.name}: {e}")
            continue
        if (w, h) != (n, n) or not _close(rgba, images[n]):
            problems.append(f"{path.name}: differs from the geometry (run --write)")
    try:
        ico = ico_decode((folder / "esses.ico").read_bytes())
        if sorted(ico) != sorted(ICO_BMP_SIZES + ICO_PNG_SIZES):
            problems.append(f"esses.ico: sizes {sorted(ico)}")
        for n, rgba in ico.items():
            if n in images and not _close(rgba, images[n]):
                problems.append(f"esses.ico: the {n} px entry differs from the geometry (run --write)")
    except (OSError, ValueError, zlib.error) as e:
        problems.append(f"esses.ico: {e}")
    try:
        icns = icns_decode((folder / "esses.icns").read_bytes())
        if [(k, n) for k, n, _ in icns] != list(ICNS_ENTRIES):
            problems.append("esses.icns: unexpected entries")
        for kind, n, rgba in icns:
            if not _close(rgba, images[n]):
                problems.append(f"esses.icns: {kind} differs from the geometry (run --write)")
    except (OSError, ValueError, zlib.error) as e:
        problems.append(f"esses.icns: {e}")
    try:
        if (folder / "esses.svg").read_text(encoding="utf-8") != svg_text():
            problems.append("esses.svg: differs from the geometry (run --write)")
    except OSError as e:
        problems.append(f"esses.svg: {e}")
    return problems


def self_test() -> int:
    ok = True
    # The tile: transparent in the corner, the gradient's top colour opaque near the top edge, the stroke white
    # on its centre line, the dot orange at the start point.
    n = 64
    img = render(n)

    def at(x: float, y: float) -> tuple[int, ...]:
        i, j = min(n - 1, int(x * n)), min(n - 1, int(y * n))
        return tuple(img[(j * n + i) * 4:(j * n + i) * 4 + 4])

    ok &= at(0.0, 0.0)[3] == 0
    ok &= at(0.5, MARGIN + 0.02)[3] == 255
    ok &= at(0.5, 0.5)[:3] == WHITE  # the arcs touch in the middle
    sx, sy = _start_point()
    ok &= at(sx, sy)[:3] == ORANGE
    # Round trips of the encoders.
    w, h, back = png_decode(png_bytes(n, img))
    ok &= (w, h, back) == (n, n, img)
    ok &= bmp_decode(bmp_entry(16, render(16))) == (16, render(16))
    print("make_icons self-test: " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="regenerate the icon files")
    mode.add_argument("--check", action="store_true", help="compare the committed files with a fresh render")
    mode.add_argument("--self-test", action="store_true", help="test the renderer and the encoders")
    parser.add_argument("--dir", type=pathlib.Path, default=HERE, help="the icon folder (default: here)")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.write:
        write(args.dir)
        print(f"make_icons: wrote {len(PNG_SIZES)} PNGs, esses.ico, esses.icns and esses.svg to {args.dir}")
        return 0
    problems = check(args.dir)
    for p in problems:
        print(p)
    print(f"make_icons: {len(problems)} difference(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
