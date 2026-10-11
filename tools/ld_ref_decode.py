#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent reference decoder for MoTeC-format .ld logs and .ldx companions.

Purpose (D-041): the writer-independent oracle for Esses' synthetic .ld fixture
families and the spike S-1 probe of the user's real files. It is deliberately
written ONLY from public layout notes, never from esses_formats and never from
any other private converter code:

  * Friss/i3rs  docs/ld-file-format.md and crates/i3rs-core (MIT)  -- primary
  * afonso360/motec-i2  src/reader.rs, structs.rs, writer.rs (MIT)
  * tobi/motorsport-telemetry-rs  crates/motec-telemetry (MIT)
  * gotzl/ldparser (GPL-3): read for understanding only; no code or struct
    format strings are reproduced here.

Python standard library only (struct, mmap, xml.etree, json, argparse, ...).
Run with ``python -I``. Large files are memory-mapped; samples are decoded in
bounded chunks and never materialised as one Python list.

Every heuristic emits a warning with a stable code (see WARNING_CODES); the
codes are documented in docs/formats/ld.md.

Scaling (D-037). For a raw sample r of a channel with fields
(shift, mul, scale, dec):  s = scale or 1, m = mul or 1, and
  base(r)      = r / s * 10**-dec
  beforeMul(r) = (base(r) + shift) * m          (gotzl, tobi reading; design default)
  afterMul(r)  = base(r) * m + shift            (i3rs reading; S-1 real-file evidence)
Each value is the exact rational result rounded once to F64 (make_scaler), so the
oracle does not depend on an implementation's operation order. Both orders are
reported whenever shift != 0 and mul not in (0, 1).

Usage:
  python -I tools/ld_ref_decode.py FILE.ld [FILE.ld ...] [--json] [-o OUT]
  python -I tools/ld_ref_decode.py FILE.ld --dump-channel "Ground Speed" \
      --dump-format csv --dump-dir out/
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import mmap
import os
import re
import struct
import sys
import time
import xml.etree.ElementTree as ET

TOOL_VERSION = "0.1.0"

HEADER_SIZE = 0x6E2            # 1762 bytes (all public sources agree)
MAGIC = 0x40                   # u32 at 0x00 (i3rs, motec-i2, tobi)
ENTRY_BASE = 0x7C              # 124-byte channel header (motec-i2, gotzl, tobi)
ENTRY_M1 = 0xD4                # 212-byte channel record (i3rs, M1 loggers)
EVENT_SIZE_U16 = 1154          # event block with u16 venue pointer
VENUE_SIZE = 1100
VEHICLE_SIZE = 260
COUNTER_BASE = 0x2EE1          # gotzl: counter = 0x2EE1 + index on its writer
STATS_CHUNK = 1 << 20          # samples per decode chunk

# Stable warning codes. severity: info | warning | error.
WARNING_CODES = {
    "ld.too_small": ("error", "file shorter than the 1,762-byte header"),
    "ld.magic_unexpected": ("warning", "u32 at 0x00 is not 0x40"),
    "ld.meta_ptr_out_of_range": ("error", "header meta_ptr outside the file"),
    "ld.data_ptr_out_of_range": ("warning", "header data_ptr outside the file"),
    "ld.data_ptr_mismatch": ("info", "header data_ptr differs from the lowest channel data_ptr"),
    "ld.block_out_of_range": ("info", "event/venue/vehicle pointer is 0 or the block does not fit"),
    "ld.ptr_width_ambiguous": ("info", "venue/vehicle pointer needs more than 16 bits; read as u32 (i3rs), a u16 reader would misplace the block"),
    "ld.count_mismatch": ("warning", "header num_channels differs from the walked channel count"),
    "ld.count_upper_bits": ("info", "num_channels read as u32 has non-zero upper 16 bits; the lower 16 bits match the walk (with or without a terminator record)"),
    "ld.list_cycle": ("error", "channel list revisits an offset; walk aborted"),
    "ld.list_backwards": ("info", "next pointer is lower than the current entry"),
    "ld.list_prev_mismatch": ("info", "prev pointer does not name the previous entry"),
    "ld.terminator_entry": ("info", "trailing all-zero record (name empty, count 0, rate 0, dtype 0) treated as list terminator (i3rs)"),
    "ld.entry_out_of_range": ("error", "channel header extends past the end of the file; walk stopped"),
    "ld.entry_stride": ("info", "the most common distance between consecutive channel headers is not 124 bytes (per-channel extension data, or 212-byte records)"),
    "ld.entry_stride_irregular": ("info", "channel headers are not evenly spaced (variable per-channel extension data); the walk follows next pointers only"),
    "ld.data_out_of_range": ("warning", "sample array runs past the end of the file; count clamped"),
    "ld.data_overlap": ("warning", "sample arrays overlap each other or the channel headers"),
    "ld.zero_rate": ("warning", "channel frequency is 0; samples not timed (decoded anyway)"),
    "ld.empty_channel": ("info", "channel has zero samples"),
    "ld.unknown_dtype": ("warning", "unrecognised (dtype_a, dtype) pair; decoded as raw signed integer if width allows"),
    "ld.dtype_guessed": ("info", "(dtype_a, dtype) pair decoded by a guess that public sources do not agree on"),
    "ld.zero_scale": ("info", "scale is 0; treated as 1 (all public sources)"),
    "ld.zero_mul": ("warning", "mul is 0; treated as 1 (tobi) - gotzl would yield all zeros"),
    "ld.negative_scaling": ("info", "scale or mul is negative"),
    "ld.shift_nonzero": ("info", "channel has shift != 0"),
    "ld.shift_mul_order": ("warning", "shift != 0 and mul != 1: beforeMul and afterMul give different values; both reported"),
    "ld.float_scaling_nonidentity": ("warning", "float channel with non-identity scaling; tobi bypasses scaling for floats, gotzl/i3rs apply it"),
    "ld.nonfinite_samples": ("info", "float channel contains NaN or Inf samples"),
    "ld.duplicate_name": ("info", "two channels share a name"),
    "ld.name_unterminated": ("info", "32-byte name field has no NUL terminator (possible truncation)"),
    "ld.name_continuation_candidate": ("info", "i3rs M1 heuristic: short name may continue a truncated name"),
    "ld.unit_from_short_name_candidate": ("info", "unit empty while short name is set (M1/TDS-style unit in short name)"),
    "ld.string_non_ascii": ("info", "string field contains bytes >= 0x80; decoded as cp1252"),
    "ld.string_trailing_bytes": ("info", "non-zero bytes after the NUL terminator of a string field"),
    "ld.counter_sequence": ("info", "channel counters are not 0x2EE1 + index"),
    "ld.date_unparsable": ("info", "header date/time not DD/MM/YYYY HH:MM[:SS]"),
    "ld.enum_tables": ("info", "enum tables found by the i3rs signature scan"),
    "ld.enum_scan_failed": ("info", "a channel names an enum table that the scan did not find"),
    "ld.pro_logging_nonzero": ("info", "u32 at 0x5DE ('pro logging') is non-zero"),
    "ld.stats_sampled": ("info", "statistics computed on a strided subsample (exact first/last values)"),
    "ldx.missing": ("info", "no .ldx companion beside the .ld"),
    "ldx.parse_error": ("warning", ".ldx is not well-formed XML"),
    "ldx.unknown_element": ("info", ".ldx element outside the known structure (kept, not interpreted)"),
    "ldx.unknown_marker_class": ("info", "marker ClassName outside SOL EOL BCN SPLTBCN IGRDBCN RESET STGSTRT STGEND"),
    "ldx.marker_time_unparsable": ("warning", "marker Time is missing or not a finite number"),
    "ldx.marker_beyond_end": ("info", "marker Time is after the end of the longest channel"),
    "ldx.marker_unsorted": ("info", "markers are not in ascending Time order in file order"),
}

KNOWN_MARKER_CLASSES = {"SOL", "EOL", "BCN", "SPLTBCN", "IGRDBCN", "RESET", "STGSTRT", "STGEND"}
KNOWN_LDX_TAGS = {"LDXFile", "Layers", "Layer", "MarkerBlock", "MarkerGroup", "Marker",
                  "Details", "String", "DateTime", "Numeric", "MarkerPair", "RangeBlock"}


# --------------------------------------------------------------------------- helpers

class Log:
    """Collects warnings; one entry per (code, channel/context) occurrence, aggregated."""

    def __init__(self):
        self.items = []
        self._agg = {}

    def add(self, code, msg, channel=None, aggregate=False, **detail):
        sev = WARNING_CODES.get(code, ("warning", ""))[0]
        if aggregate:
            key = code
            if key in self._agg:
                e = self._agg[key]
                e["count"] += 1
                if len(e["channels"]) < 20 and channel is not None:
                    e["channels"].append(channel)
                return
            e = {"code": code, "severity": sev, "message": msg, "count": 1,
                 "channels": [channel] if channel is not None else []}
            if detail:
                e["detail"] = detail
            self._agg[key] = e
            self.items.append(e)
            return
        e = {"code": code, "severity": sev, "message": msg}
        if channel is not None:
            e["channel"] = channel
        if detail:
            e["detail"] = detail
        self.items.append(e)


def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def i16(b, o):
    return struct.unpack_from("<h", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _cp1252(raw: bytes) -> str:
    out = []
    for byte in raw:
        if byte < 0x80:
            out.append(chr(byte))
        else:
            try:
                out.append(bytes([byte]).decode("cp1252"))
            except UnicodeDecodeError:          # 0x81 0x8D 0x8F 0x90 0x9D
                out.append(chr(byte))
    return "".join(out)


def read_str(b, o, n, log=None, field="", channel=None, evidence=None):
    """Fixed-width NUL-padded single-byte string -> (text, info).

    Cut at the first NUL, decode cp1252, right-trim spaces (05 section 3).
    """
    raw = bytes(b[o:o + n])
    nul = raw.find(b"\x00")
    terminated = nul >= 0
    body = raw if nul < 0 else raw[:nul]
    text = _cp1252(body).rstrip(" ")
    info = {"terminated": terminated}
    if any(c >= 0x80 for c in body):
        info["non_ascii"] = True
        if log is not None:
            log.add("ld.string_non_ascii", "non-ASCII bytes decoded as cp1252", channel=channel,
                    aggregate=True)
    if terminated and any(raw[nul + 1:]):
        info["trailing_hex"] = raw[nul + 1:].rstrip(b"\x00").hex()[:64]
        if log is not None:
            log.add("ld.string_trailing_bytes", "non-zero bytes after NUL in string fields",
                    channel=channel if channel is not None else field, aggregate=True)
    return text, info


def nonzero_hex(b, o, n, limit=96):
    raw = bytes(b[o:o + n])
    nz = sum(1 for c in raw if c)
    if not nz:
        return None
    return {"offset": o, "len": n, "nonzero_bytes": nz, "hex": raw.hex()[:limit * 2]}


# --------------------------------------------------------------------------- dtypes

def classify_dtype(a, w):
    """(dtype_a, dtype width) -> (kind, struct code, flags) per the public notes.

    kind in {'f16','f32','f64','i8','i16','i32','i64', None}.
    Returns (kind, code, note) where note is None when every source agrees.
    """
    if a == 0x07 and w == 4:
        return "f32", "f", None
    if a == 0x07 and w == 2:
        return "f16", "e", None                     # i3rs (half); gotzl float16
    if a == 0x08 and w == 8:
        return "f64", "d", None                     # i3rs, gotzl, tobi
    if a in (0x00, 0x03, 0x05):
        if w == 2:
            return "i16", "h", None
        if w == 4:
            return "i32", "i", None
        if w == 1:
            return "i8", "b", "width-1 integer: i3rs only"
    if a == 0x07 and w == 8:
        return "f64", "d", "(0x07, 8) read as F64: tobi only"
    if a in (0x04, 0x06) and w in (1, 2, 4):
        return {1: "i8", 2: "i16", 4: "i32"}[w], {1: "b", 2: "h", 4: "i"}[w], \
            "0x04/0x06 read as signed integer (i3rs includes 0x06); design notes call them state/bitfield"
    if a == 0x08 and w in (2, 4):
        return {2: "i16", 4: "i32"}[w], {2: "h", 4: "i"}[w], "(0x08, 2|4) read as integer: tobi only"
    return None, None, None


def raw_width(code):
    return struct.calcsize("<" + code)


# --------------------------------------------------------------------------- scaling

def scaling_params(ch):
    s = ch["scale"] if ch["scale"] != 0 else 1
    m = ch["mul"] if ch["mul"] != 0 else 1
    d = ch["dec"]
    return s, m, d, ch["shift"]


def make_scaler(ch, order):
    """raw -> F64 engineering value, rounded ONCE from the exact rational value.

    With den = scale * 10**dec (dec >= 0) or num = 10**-dec (dec < 0):
      beforeMul: ((raw * num + shift * den) * mul) / den
      afterMul:  (raw * num * mul + shift * den) / den
    Integer raws use Python int arithmetic and int/int true division, which CPython
    rounds correctly; float raws go through fractions.Fraction. The oracle therefore
    does not depend on an operation order chosen by any implementation.
    """
    from fractions import Fraction
    s, m, d, sh = scaling_params(ch)
    if s == 1 and m == 1 and d == 0 and sh == 0:
        return lambda r: float(r)
    num, den = (1, s * 10 ** d) if d >= 0 else (10 ** (-d), s)
    before = order == "beforeMul"

    def f(r):
        if isinstance(r, int):
            n = (r * num + sh * den) * m if before else r * num * m + sh * den
            return n / den
        if not math.isfinite(r):
            return r
        q = Fraction(r)
        n = (q * num + sh * den) * m if before else q * num * m + sh * den
        return float(n / den)
    return f


def is_identity(ch):
    s, m, d, sh = scaling_params(ch)
    return s == 1 and m == 1 and d == 0 and sh == 0


# --------------------------------------------------------------------------- statistics

def raw_stats(mm, ch, max_samples):
    """Min/max/sum over raw values in bounded chunks; strided when count > max_samples."""
    code = ch["_code"]
    w = raw_width(code)
    n = ch["count_decoded"]
    ptr = ch["data_ptr"]
    res = {"n": n, "nonfinite": 0, "nan": 0}
    if n == 0:
        return res
    stride = 1
    if max_samples and n > max_samples:
        stride = -(-n // max_samples)
    res["stride"] = stride
    is_float = code in ("e", "f", "d")
    vmin = None
    vmax = None
    total = 0.0 if is_float else 0
    used = 0
    with memoryview(mm) as base:
        view = base[ptr:ptr + n * w].cast(code)
        try:
            res["first"] = view[:5].tolist()
            res["last"] = view[max(0, n - 5):].tolist()
            step = STATS_CHUNK * stride
            for i in range(0, n, step):
                chunk = view[i:min(n, i + step):stride].tolist()
                if is_float:
                    s = sum(chunk)
                    if not math.isfinite(s):
                        fin = [x for x in chunk if math.isfinite(x)]
                        res["nan"] += sum(1 for x in chunk if x != x)
                        res["nonfinite"] += len(chunk) - len(fin)
                        chunk = fin
                        s = sum(chunk)
                    if not chunk:
                        continue
                else:
                    s = sum(chunk)
                cmin = min(chunk)
                cmax = max(chunk)
                vmin = cmin if vmin is None or cmin < vmin else vmin
                vmax = cmax if vmax is None or cmax > vmax else vmax
                total += s
                used += len(chunk)
        finally:
            view.release()
    res["used"] = used
    res["min"] = vmin
    res["max"] = vmax
    res["mean"] = (total / used) if used else None
    return res


def scaled_stats(rs, ch, order):
    f = make_scaler(ch, order)
    out = {"order": order}
    if rs.get("min") is None:
        out.update({"min": None, "max": None, "mean": None})
    else:
        a, b = f(rs["min"]), f(rs["max"])
        out["min"], out["max"] = (a, b) if a <= b else (b, a)
        out["mean"] = f(rs["mean"])
    out["first"] = [f(x) for x in rs.get("first", [])]
    out["last"] = [f(x) for x in rs.get("last", [])]
    return out


def _json_float(x):
    if isinstance(x, float) and not math.isfinite(x):
        return repr(x)
    return x


# --------------------------------------------------------------------------- .ld

HEADER_FIELDS = [
    # offset, kind, length, name   (sources: i3rs doc, motec-i2 reader.rs, gotzl)
    (0x000, "u32", 4, "magic"),
    (0x004, "u32", 4, "unknown_0x004"),
    (0x008, "u32", 4, "meta_ptr"),
    (0x00C, "u32", 4, "data_ptr"),
    (0x024, "u32", 4, "event_ptr"),
    (0x040, "u16", 2, "static_0x040"),
    (0x042, "u16", 2, "static_0x042"),
    (0x044, "u16", 2, "static_0x044"),
    (0x046, "u32", 4, "device_serial"),
    (0x04A, "str", 8, "device_type"),
    (0x052, "u16", 2, "device_version"),
    (0x054, "u16", 2, "static_0x054"),
    (0x056, "u32", 4, "num_channels"),
    (0x05A, "u32", 4, "unknown_0x05A"),
    (0x05E, "str", 16, "date"),
    (0x07E, "str", 16, "time"),
    (0x09E, "str", 64, "driver"),
    (0x0DE, "str", 64, "vehicle_id"),
    (0x15E, "str", 64, "venue"),
    (0x5DE, "u32", 4, "pro_logging"),
    (0x5E2, "u16", 2, "unknown_0x5E2"),
    (0x5E4, "str", 64, "session"),          # motec-i2 reader.rs
    (0x624, "str", 64, "short_comment"),
]
# Regions the public notes call unknown/padding; non-zero bytes are reported as evidence.
HEADER_UNKNOWN_REGIONS = [(0x010, 20), (0x028, 24), (0x06E, 16), (0x08E, 16), (0x11E, 64),
                          (0x19E, 64), (0x1DE, 1024), (0x664, 126)]


def parse_header(b, size, log):
    h = {}
    strinfo = {}
    for off, kind, n, name in HEADER_FIELDS:
        if kind == "u32":
            h[name] = u32(b, off)
        elif kind == "u16":
            h[name] = u16(b, off)
        else:
            h[name], info = read_str(b, off, n, log, field="header." + name)
            if info.get("trailing_hex") or info.get("non_ascii"):
                strinfo[name] = info
    if strinfo:
        h["_string_info"] = strinfo
    unk = [x for x in (nonzero_hex(b, o, n) for o, n in HEADER_UNKNOWN_REGIONS) if x]
    h["unknown_nonzero_regions"] = unk
    return h


def parse_datetime(date, tm, log):
    m = re.fullmatch(r"\s*(\d{1,2})/(\d{1,2})/(\d{2,4})\s*", date or "")
    t = re.fullmatch(r"\s*(\d{1,2}):(\d{2})(?::(\d{2}))?\s*", tm or "")
    if not m or not t:
        if date or tm:
            log.add("ld.date_unparsable", "date/time not parseable as DD/MM/YYYY HH:MM[:SS]",
                    date=date, time=tm)
        return None
    d, mo, y = (int(x) for x in m.groups())
    if not (1 <= mo <= 12 and 1 <= d <= 31):
        log.add("ld.date_unparsable", "date fields out of range", date=date, time=tm)
        return None
    hh, mi, ss = int(t.group(1)), int(t.group(2)), int(t.group(3) or 0)
    return "%04d-%02d-%02dT%02d:%02d:%02d" % (y, mo, d, hh, mi, ss)


def parse_blocks(b, size, event_ptr, log):
    out = {"event": None, "venue": None, "vehicle": None}
    if event_ptr == 0 or event_ptr + EVENT_SIZE_U16 > size:
        log.add("ld.block_out_of_range", "event block absent or out of range", ptr=event_ptr)
        return out
    ev = {"ptr": event_ptr}
    ev["name"], _ = read_str(b, event_ptr, 64, log, field="event.name")
    ev["session"], _ = read_str(b, event_ptr + 0x40, 64, log, field="event.session")
    ev["long_comment"], _ = read_str(b, event_ptr + 0x80, 1024, log, field="event.comment")
    vp16 = u16(b, event_ptr + 0x480)
    vp32 = u32(b, event_ptr + 0x480) if event_ptr + 0x484 <= size else vp16
    ev["venue_ptr_u16"] = vp16
    ev["venue_ptr_u32"] = vp32
    if vp16 != vp32:
        log.add("ld.ptr_width_ambiguous", "event venue pointer differs as u16 vs u32",
                u16=vp16, u32=vp32)
    out["event"] = ev
    vptr = vp32 if (vp32 and vp32 + VENUE_SIZE <= size) else vp16
    if vptr == 0 or vptr + VENUE_SIZE > size:
        log.add("ld.block_out_of_range", "venue block absent or out of range", ptr=vptr)
        return out
    ve = {"ptr": vptr}
    ve["name"], _ = read_str(b, vptr, 64, log, field="venue.name")
    hp16 = u16(b, vptr + 0x44A)
    hp32 = u32(b, vptr + 0x44A) if vptr + 0x44E <= size else hp16
    ve["vehicle_ptr_u16"] = hp16
    ve["vehicle_ptr_u32"] = hp32
    if hp16 != hp32:
        log.add("ld.ptr_width_ambiguous", "venue vehicle pointer differs as u16 vs u32",
                u16=hp16, u32=hp32)
    unk = nonzero_hex(b, vptr + 0x40, 0x44A - 0x40)
    if unk:
        ve["unknown_nonzero"] = unk
    out["venue"] = ve
    hptr = hp32 if (hp32 and hp32 + VEHICLE_SIZE <= size) else hp16
    if hptr == 0 or hptr + VEHICLE_SIZE > size:
        log.add("ld.block_out_of_range", "vehicle block absent or out of range", ptr=hptr)
        return out
    vh = {"ptr": hptr}
    vh["id"], _ = read_str(b, hptr, 64, log, field="vehicle.id")
    vh["weight"] = u32(b, hptr + 0xC0)
    vh["type"], _ = read_str(b, hptr + 0xC4, 32, log, field="vehicle.type")
    vh["comment"], _ = read_str(b, hptr + 0xE4, 32, log, field="vehicle.comment")
    unk = nonzero_hex(b, hptr + 0x40, 0x80)
    if unk:
        vh["unknown_nonzero"] = unk
    out["vehicle"] = vh
    return out


def walk_channels(b, size, meta_ptr, log, entry_size_opt="auto"):
    entries = []
    seen = set()
    off = meta_ptr
    prev = 0
    limit = size // ENTRY_BASE + 1
    while off:
        if off in seen:
            log.add("ld.list_cycle", "channel list revisits offset", offset=off)
            break
        if off + ENTRY_BASE > size:
            log.add("ld.entry_out_of_range", "channel header past end of file", offset=off)
            break
        if len(entries) >= limit:
            log.add("ld.list_cycle", "channel walk exceeded size/124 iterations", offset=off)
            break
        seen.add(off)
        e = {"offset": off,
             "prev_ptr": u32(b, off + 0x00),
             "next_ptr": u32(b, off + 0x04),
             "data_ptr": u32(b, off + 0x08),
             "count": u32(b, off + 0x0C),
             "counter": u16(b, off + 0x10),
             "dtype_a": u16(b, off + 0x12),
             "dtype": u16(b, off + 0x14),
             "freq": u16(b, off + 0x16),
             "shift": i16(b, off + 0x18),
             "mul": i16(b, off + 0x1A),
             "scale": i16(b, off + 0x1C),
             "dec": i16(b, off + 0x1E)}
        idx = len(entries)
        e["name"], ni = read_str(b, off + 0x20, 32, log, channel=idx)
        e["short_name"], si = read_str(b, off + 0x40, 8, log, channel=idx)
        e["unit"], ui = read_str(b, off + 0x48, 12, log, channel=idx)
        e["_name_terminated"] = ni["terminated"]
        e["_name_raw_last"] = b[off + 0x3F]
        if e["prev_ptr"] != prev:
            log.add("ld.list_prev_mismatch", "prev pointer does not match previous entry",
                    channel=idx, aggregate=True)
        entries.append(e)
        nxt = e["next_ptr"]
        if nxt and nxt < off:
            log.add("ld.list_backwards", "next pointer points backwards", channel=idx,
                    aggregate=True)
        prev = off
        off = nxt
    # stride detection
    strides = [entries[i + 1]["offset"] - entries[i]["offset"] for i in range(len(entries) - 1)]
    stride_counts = {}
    for s in strides:
        stride_counts[s] = stride_counts.get(s, 0) + 1
    if entry_size_opt != "auto":
        stride = int(entry_size_opt)
    elif stride_counts:
        stride = max(stride_counts.items(), key=lambda kv: kv[1])[0]
    else:
        stride = ENTRY_BASE
    if stride != ENTRY_BASE and stride_counts:
        log.add("ld.entry_stride", "channel header stride is %d bytes" % stride, stride=stride)
    if len(stride_counts) > 1:
        log.add("ld.entry_stride_irregular", "channel header strides vary",
                strides={str(k): v for k, v in sorted(stride_counts.items())})
    # per-entry record size (distance to the next entry when it follows directly), the
    # tail bytes after the unit field, and the i3rs M1 enum fields of 212-byte records
    for k, e in enumerate(entries):
        off = e["offset"]
        rs = None
        if k + 1 < len(entries):
            d = entries[k + 1]["offset"] - off
            if ENTRY_BASE <= d <= 0x400:
                rs = d
        if rs is None:
            rs = stride if ENTRY_BASE <= stride <= 0x400 else ENTRY_BASE
            e["record_size_inferred"] = True
        e["record_size"] = rs
        tl = max(0, min(rs - 0x54, size - (off + 0x54)))
        tail = bytes(b[off + 0x54: off + 0x54 + tl])
        e["tail_hex"] = tail.hex() if any(tail) else ""
        # Observation aid (not in the public notes): u32 values in the 40-byte tail that point
        # into this file; real files carry per-channel extension data reached this way.
        ptrs = []
        for t in range(0x54, min(0x7C, size - off - 3), 4):
            v = u32(b, off + t)
            if HEADER_SIZE <= v < size and v != e["data_ptr"] and abs(v - off) <= 0x10000:
                ptrs.append({"at": t, "ptr": v, "rel": v - off})
        if ptrs:
            e["tail_ptrs"] = ptrs
        if rs == ENTRY_M1 and off + ENTRY_M1 <= size:
            # i3rs: 212-byte records carry enum_type u16 @0xD0, enum_id u16 @0xD2
            e["enum_type"] = u16(b, off + 0xD0)
            e["enum_id"] = u16(b, off + 0xD2)
    return entries, {"stride": stride, "stride_counts": {str(k): v for k, v in sorted(stride_counts.items())}}


def scan_enum_tables(b, start, end, log):
    """i3rs signature scan: u16 count, u16 type == 2, u16 id, then entries starting with 03 00."""
    tables = {}
    if end <= start:
        return tables
    region = bytes(b[start:end])
    pat = re.compile(rb"(?=(?s:(..)\x02\x00(..)\x03\x00))")
    for m in pat.finditer(region):
        pos = m.start()
        count = struct.unpack_from("<H", region, pos)[0]
        eid = struct.unpack_from("<H", region, pos + 4)[0]
        if not (1 <= count <= 500) or eid in tables:
            continue
        e = pos + 6
        entries = []
        lim = min(len(region), pos + 6 + 20000)
        ok = True
        while len(entries) < count:
            while e + 2 <= lim and region[e:e + 2] == b"\x06\x00":
                e += 2
            if e + 10 > lim or region[e:e + 2] != b"\x03\x00":
                ok = False
                break
            sz = struct.unpack_from("<H", region, e + 6)[0]
            if sz < 2 or sz - 2 >= 200 or e + 8 + sz > lim:
                ok = False
                break
            val = struct.unpack_from("<H", region, e + 8)[0]
            lab = region[e + 10:e + 8 + sz]
            nul = lab.find(b"\x00")
            lab = lab if nul < 0 else lab[:nul]
            v = -1 if val == 0xFFFF else (-2 if val == 0xFFFE else val)
            entries.append((v, _cp1252(lab)))
            e = e + 8 + sz
        if ok and len(entries) == count:
            tables[eid] = {"offset": start + pos, "count": count, "entries": entries}
    if tables:
        log.add("ld.enum_tables", "%d enum tables found by signature scan" % len(tables),
                ids=sorted(tables)[:50])
    return tables


def decode_file(path, max_samples=4_000_000, entry_size="auto", want_hash=True,
                stats=True, dump=None, dump_dir=None, dump_format="f64", shift_order="beforeMul",
                ldx=True, enum_labels=False):
    """Decode one .ld file; returns a JSON-serialisable dict."""
    log = Log()
    res = {"tool": "ld_ref_decode", "tool_version": TOOL_VERSION, "file": os.path.abspath(path)}
    st = os.stat(path)
    size = st.st_size
    res["size"] = size
    t0 = time.perf_counter()
    if size < HEADER_SIZE:
        log.add("ld.too_small", "file shorter than header", size=size)
        res["warnings"] = log.items
        return res
    with open(path, "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            res.update(_decode_mapped(mm, size, log, path, t0, max_samples, entry_size, stats,
                                      dump, dump_dir, dump_format, shift_order, enum_labels))
            if want_hash:
                th = time.perf_counter()
                h = hashlib.sha256()
                with memoryview(mm) as mv:
                    for i in range(0, size, 1 << 24):
                        h.update(mv[i:i + (1 << 24)])
                res["sha256"] = h.hexdigest()
                res["timing_ms"]["sha256"] = round((time.perf_counter() - th) * 1000, 2)
        finally:
            mm.close()
    if ldx:
        tl = time.perf_counter()
        res["ldx"] = parse_ldx_for(path, log, res.get("duration_s"))
        res["timing_ms"]["ldx"] = round((time.perf_counter() - tl) * 1000, 2)
    res["warnings"] = log.items
    res["warning_codes"] = sorted({w["code"] for w in log.items})
    return res


def _decode_mapped(mm, size, log, path, t0, max_samples, entry_size, stats, dump, dump_dir,
                   dump_format, shift_order, enum_labels):
    b = mm
    out = {}
    hdr = parse_header(b, size, log)
    out["header"] = hdr
    hdr["datetime_local"] = parse_datetime(hdr["date"], hdr["time"], log)
    sniff = {"magic_ok": hdr["magic"] == MAGIC,
             "meta_ptr_ok": HEADER_SIZE <= hdr["meta_ptr"] < size,
             "data_ptr_ok": HEADER_SIZE <= hdr["data_ptr"] < size,
             "event_ptr_ok": hdr["event_ptr"] == 0 or HEADER_SIZE <= hdr["event_ptr"] <= size - EVENT_SIZE_U16}
    if not sniff["magic_ok"]:
        log.add("ld.magic_unexpected", "u32@0x00 is 0x%X, expected 0x40" % hdr["magic"])
    if not sniff["data_ptr_ok"]:
        log.add("ld.data_ptr_out_of_range", "header data_ptr outside file", data_ptr=hdr["data_ptr"])
    if hdr["pro_logging"]:
        log.add("ld.pro_logging_nonzero", "u32@0x5DE = 0x%X" % hdr["pro_logging"],
                value=hdr["pro_logging"])
    out["blocks"] = parse_blocks(b, size, hdr["event_ptr"], log)
    if not sniff["meta_ptr_ok"]:
        log.add("ld.meta_ptr_out_of_range", "meta_ptr outside file", meta_ptr=hdr["meta_ptr"])
        sniff["confidence"] = 60 if sniff["magic_ok"] else 0
        out["sniff"] = sniff
        out["channels"] = []
        out["timing_ms"] = {"probe": round((time.perf_counter() - t0) * 1000, 2)}
        return out
    entries, layout = walk_channels(b, size, hdr["meta_ptr"], log, entry_size)
    # i3rs: M1 lists end with an all-zero terminator record (next 0, count 0); drop it
    if entries:
        t = entries[-1]
        if (t["next_ptr"] == 0 and t["count"] == 0 and t["freq"] == 0 and t["dtype"] == 0
                and t["dtype_a"] == 0 and not t["name"]):
            entries.pop()
            out["terminator_record"] = {"offset": t["offset"], "counter": t["counter"]}
            log.add("ld.terminator_entry", "trailing all-zero terminator record dropped from the channel list",
                    offset=t["offset"])
    first = entries[0] if entries else None
    sniff["first_prev_zero"] = bool(first and first["prev_ptr"] == 0)
    sniff["confidence"] = 95 if all(sniff[k] for k in ("magic_ok", "meta_ptr_ok", "data_ptr_ok",
                                                         "event_ptr_ok", "first_prev_zero")) else (
        60 if sniff["magic_ok"] else 0)
    out["sniff"] = sniff
    out["layout"] = layout
    hdr["num_channels_lo16"] = hdr["num_channels"] & 0xFFFF
    hdr["num_channels_hi16"] = hdr["num_channels"] >> 16
    walked = {len(entries), len(entries) + (1 if "terminator_record" in out else 0)}
    if hdr["num_channels"] not in walked:
        if hdr["num_channels_lo16"] in walked:
            log.add("ld.count_upper_bits", "num_channels upper 16 bits non-zero (0x%X); lower 16 bits match"
                    % hdr["num_channels_hi16"], header=hdr["num_channels"], walked=len(entries))
        else:
            log.add("ld.count_mismatch", "num_channels %d, walked %d" % (hdr["num_channels"], len(entries)),
                    header=hdr["num_channels"], walked=len(entries))
    # counters
    ctr_ok = all(e["counter"] == COUNTER_BASE + i for i, e in enumerate(entries))
    ctrs = [e["counter"] for e in entries]
    layout["counter_pattern"] = ("0x2EE1+i" if ctr_ok else
                                 ("constant:%d" % ctrs[0] if ctrs and len(set(ctrs)) == 1 else "other"))
    if entries and not ctr_ok:
        log.add("ld.counter_sequence", "counters are %s" % layout["counter_pattern"],
                first=ctrs[:4])
    # data layout checks
    meta_lo = min((e["offset"] for e in entries), default=0)
    meta_hi = max((e["offset"] for e in entries), default=0) + ENTRY_BASE
    ranges = []
    names = {}
    duration = 0.0
    for i, e in enumerate(entries):
        e["index"] = i
        kind, code, note = classify_dtype(e["dtype_a"], e["dtype"])
        e["sample_type"] = kind
        e["_code"] = code
        if kind is None:
            if e["dtype"] in (1, 2, 4, 8):
                code = {1: "b", 2: "h", 4: "i", 8: "q"}[e["dtype"]]
                e["_code"] = code
                e["sample_type"] = "raw_i%d" % (8 * e["dtype"])
            log.add("ld.unknown_dtype", "unknown (dtype_a, dtype)", channel=i, aggregate=True,
                    pairs="(0x%X,%d)" % (e["dtype_a"], e["dtype"]))
        elif note:
            e["dtype_note"] = note
            log.add("ld.dtype_guessed", "dtype pair decoded by a guess", channel=i, aggregate=True)
        if e["_code"]:
            w = raw_width(e["_code"])
            fit = max(0, (size - e["data_ptr"]) // w) if e["data_ptr"] < size else 0
            e["count_decoded"] = min(e["count"], fit)
            if e["count_decoded"] < e["count"]:
                log.add("ld.data_out_of_range", "sample array past EOF; clamped", channel=i,
                        count=e["count"], fits=e["count_decoded"])
            if e["count_decoded"]:
                ranges.append((e["data_ptr"], e["data_ptr"] + e["count_decoded"] * w, i))
        else:
            e["count_decoded"] = 0
        if e["count"] == 0:
            log.add("ld.empty_channel", "channel has 0 samples", channel=i, aggregate=True)
        if e["freq"] == 0:
            log.add("ld.zero_rate", "frequency 0", channel=i, aggregate=True)
        else:
            duration = max(duration, e["count"] / e["freq"])
        if e["scale"] == 0:
            log.add("ld.zero_scale", "scale 0 treated as 1", channel=i, aggregate=True)
        if e["mul"] == 0:
            log.add("ld.zero_mul", "mul 0 treated as 1", channel=i, aggregate=True)
        if e["scale"] < 0 or e["mul"] < 0:
            log.add("ld.negative_scaling", "negative scale or mul", channel=i, aggregate=True)
        if e["shift"] != 0:
            log.add("ld.shift_nonzero", "shift != 0", channel=i, aggregate=True)
            if e["mul"] not in (0, 1):
                log.add("ld.shift_mul_order", "shift != 0 and mul != 1", channel=i, aggregate=True)
        if kind in ("f16", "f32", "f64") and not is_identity(e):
            log.add("ld.float_scaling_nonidentity", "float channel with non-identity scaling",
                    channel=i, aggregate=True)
        if not e["_name_terminated"]:
            log.add("ld.name_unterminated", "32-char name without NUL", channel=i, aggregate=True)
            sn = e["short_name"]
            last = chr(e["_name_raw_last"])
            if sn and len(sn) <= 2 and last.isalnum() and sn[0].isalnum():
                e["name_continuation_candidate"] = e["name"] + sn
                log.add("ld.name_continuation_candidate", "short name may continue the name",
                        channel=i, aggregate=True)
        if not e["unit"] and e["short_name"]:
            log.add("ld.unit_from_short_name_candidate", "unit empty, short name set",
                    channel=i, aggregate=True)
        key = e["name"]
        if key in names:
            log.add("ld.duplicate_name", "duplicate channel name", channel=i, aggregate=True,
                    first=names[key])
        else:
            names[key] = i
    out["duration_s"] = duration
    data_ptrs = [r[0] for r in ranges]
    if data_ptrs and hdr["data_ptr"] != min(data_ptrs):
        log.add("ld.data_ptr_mismatch", "header data_ptr != lowest channel data_ptr",
                header=hdr["data_ptr"], lowest=min(data_ptrs))
    ranges.sort()
    # overlap check: every sample array against every other array and every 124-byte header
    spans = [(a0, a1, "data", ia) for (a0, a1, ia) in ranges] +             [(e["offset"], e["offset"] + ENTRY_BASE, "header", e["index"]) for e in entries]
    spans.sort()
    overlaps = []
    hi = None
    for sp in spans:
        if hi is not None and sp[0] < hi[1]:
            overlaps.append({"a": [hi[2], hi[3]], "b": [sp[2], sp[3]]})
        if hi is None or sp[1] > hi[1]:
            hi = sp
    if overlaps:
        log.add("ld.data_overlap", "%d overlapping sample/header ranges" % len(overlaps),
                count=len(overlaps), first=overlaps[:3])
    layout["meta_region"] = [meta_lo, meta_hi]
    layout["data_region"] = [ranges[0][0], max(r[1] for r in ranges)] if ranges else None
    layout["bytes_after_data"] = (size - layout["data_region"][1]) if ranges else None
    # enum tables (M1 records only)
    enum_tables = {}
    if any(e["record_size"] >= ENTRY_M1 for e in entries):
        lo = HEADER_SIZE
        hi = min(data_ptrs) if data_ptrs else size
        enum_tables = scan_enum_tables(b, lo, min(hi, size), log)
        for e in entries:
            if e.get("enum_type") == 2:
                t = enum_tables.get(e.get("enum_id"))
                if t is None:
                    log.add("ld.enum_scan_failed", "enum table not found", channel=e["index"],
                            aggregate=True, enum_id=e.get("enum_id"))
                else:
                    e["enum_count"] = t["count"]
    out["enum_tables"] = {str(k): ({"offset": v["offset"], "count": v["count"], "entries": v["entries"]}
                                   if enum_labels else {"offset": v["offset"], "count": v["count"]})
                          for k, v in sorted(enum_tables.items())}
    out["timing_ms"] = {"probe": round((time.perf_counter() - t0) * 1000, 2)}
    # sample decode
    ts = time.perf_counter()
    sampled_any = False
    for e in entries:
        e.pop("_name_raw_last", None)
        e.pop("_name_terminated", None)
        if stats and e["_code"]:
            rs = raw_stats(b, e, max_samples)
            if rs.get("stride", 1) > 1:
                sampled_any = True
            e["raw_stats"] = {k: _json_float(v) if not isinstance(v, list) else [_json_float(x) for x in v]
                              for k, v in rs.items()}
            e["stats"] = {}
            orders = ["beforeMul", "afterMul"] if (e["shift"] != 0 and e["mul"] not in (0, 1)) else ["beforeMul"]
            for o in orders:
                ss = scaled_stats(rs, e, o)
                e["stats"][o] = {k: _json_float(v) if not isinstance(v, list) else [_json_float(x) for x in v]
                                 for k, v in ss.items()}
            if rs.get("nonfinite"):
                log.add("ld.nonfinite_samples", "NaN/Inf samples", channel=e["index"], aggregate=True)
        if dump is not None and e["_code"] and (dump == ["*"] or e["name"] in dump or str(e["index"]) in dump):
            dump_channel(b, e, dump_dir, dump_format, shift_order)
    if sampled_any:
        log.add("ld.stats_sampled", "statistics strided for channels above %d samples" % max_samples)
    for e in entries:
        e.pop("_code", None)
    out["timing_ms"]["stats"] = round((time.perf_counter() - ts) * 1000, 2)
    out["channels"] = entries
    return out


def time_us(i, freq):
    """02 section 1 behaviour 1: timeAt(i) = floorDiv(2*i*1e6 + num, 2*num) for Rate{freq,1}."""
    return (2 * i * 1_000_000 + freq) // (2 * freq) if freq else 0


def _safe(name):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_") or "unnamed"


def dump_channel(mm, e, dump_dir, fmt, shift_order):
    os.makedirs(dump_dir, exist_ok=True)
    code = e["_code"]
    w = raw_width(code)
    n = e["count_decoded"]
    orders = [shift_order]
    if e["shift"] != 0 and e["mul"] not in (0, 1):
        orders = ["beforeMul", "afterMul"]
    stem = os.path.join(dump_dir, "%03d_%s" % (e["index"], _safe(e["name"])))
    meta = {"name": e["name"], "index": e["index"], "count": n, "freq": e["freq"],
            "dtype_a": e["dtype_a"], "dtype": e["dtype"], "shift": e["shift"], "mul": e["mul"],
            "scale": e["scale"], "dec": e["dec"], "unit": e["unit"], "files": {}}
    with memoryview(mm) as base:
        view = base[e["data_ptr"]:e["data_ptr"] + n * w].cast(code)
        try:
            for o in orders:
                f = make_scaler(e, o)
                suffix = "" if len(orders) == 1 else "." + o
                if fmt in ("f64", "both"):
                    p = stem + suffix + ".f64"
                    with open(p, "wb") as fh:
                        for i in range(0, n, STATS_CHUNK):
                            vals = [f(x) for x in view[i:min(n, i + STATS_CHUNK)].tolist()]
                            fh.write(struct.pack("<%dd" % len(vals), *vals))
                    meta["files"][o + ".f64"] = os.path.basename(p)
                if fmt in ("csv", "both"):
                    p = stem + suffix + ".csv"
                    with open(p, "w", encoding="ascii", newline="\n") as fh:
                        fh.write("index,timeUs,value,valid\n")
                        for i0 in range(0, n, STATS_CHUNK):
                            vals = view[i0:min(n, i0 + STATS_CHUNK)].tolist()
                            lines = []
                            for k, x in enumerate(vals):
                                i = i0 + k
                                v = f(x)
                                ok = 1 if math.isfinite(v) else 0
                                lines.append("%d,%d,%r,%d\n" % (i, time_us(i, e["freq"]), v, ok))
                            fh.write("".join(lines))
                    meta["files"][o + ".csv"] = os.path.basename(p)
        finally:
            view.release()
    with open(stem + ".json", "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=1)


def read_channels(path, indices=None, order="beforeMul", start=0, stop=None, entry_size="auto"):
    """Decoded values for oracle use: {index: (entry, array('d'))}.

    Re-walks the channel list (cheap) and decodes samples [start, stop) of each requested
    channel with the given scaling order. Values are F64, computed exactly as in the
    statistics and dumps.
    """
    import array
    out = {}
    with open(path, "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            size = len(mm)
            entries, _ = walk_channels(mm, size, u32(mm, 0x08), Log(), entry_size)
            for e in entries:
                kind, code, _ = classify_dtype(e["dtype_a"], e["dtype"])
                if kind is None and e["dtype"] in (1, 2, 4, 8):
                    code = {1: "b", 2: "h", 4: "i", 8: "q"}[e["dtype"]]
                e["_code"] = code
            sel = range(len(entries)) if indices is None else indices
            for i in sel:
                e = entries[i]
                vals = array.array("d")
                if e["_code"]:
                    w = raw_width(e["_code"])
                    fit = max(0, (size - e["data_ptr"]) // w) if e["data_ptr"] < size else 0
                    n = min(e["count"], fit)
                    a = max(0, start)
                    z = n if stop is None else min(n, stop)
                    f = make_scaler(e, order)
                    with memoryview(mm) as base:
                        view = base[e["data_ptr"]:e["data_ptr"] + n * w].cast(e["_code"])
                        try:
                            for k in range(a, z, STATS_CHUNK):
                                vals.extend(f(x) for x in view[k:min(z, k + STATS_CHUNK)].tolist())
                        finally:
                            view.release()
                out[i] = (e, vals)
        finally:
            mm.close()
    return out


# --------------------------------------------------------------------------- .ldx

def find_companion(path):
    d, base = os.path.split(os.path.abspath(path))
    stem = os.path.splitext(base)[0]
    try:
        for f in os.listdir(d):
            fs, fe = os.path.splitext(f)
            if fs.lower() == stem.lower() and fe.lower() == ".ldx":
                return os.path.join(d, f)
    except OSError:
        pass
    return None


def parse_ldx_for(ld_path, log, duration_s=None):
    p = find_companion(ld_path)
    if not p:
        log.add("ldx.missing", "no .ldx companion")
        return None
    return parse_ldx(p, log, duration_s)


def parse_ldx(path, log, duration_s=None):
    out = {"file": os.path.abspath(path), "size": os.path.getsize(path)}
    with open(path, "rb") as fh:
        data = fh.read()
    out["sha256"] = hashlib.sha256(data).hexdigest()
    try:
        root = ET.fromstring(data)
    except ET.ParseError as ex:
        log.add("ldx.parse_error", "XML parse error: %s" % ex)
        out["parse_error"] = str(ex)
        return out
    out["root"] = {"tag": root.tag, "attrib": dict(root.attrib)}
    unknown = {}
    markers = []
    details = []
    pairs = []
    unknown_classes = set()

    def walk(el, path, group):
        for ch in el:
            tag = ch.tag
            p = path + "/" + tag
            if tag not in KNOWN_LDX_TAGS:
                unknown[p] = unknown.get(p, 0) + 1
            g = group
            if tag == "MarkerGroup":
                g = {"name": ch.get("Name"), "index": ch.get("Index")}
            if tag == "Marker":
                m = {"group": group.get("name") if group else None,
                     "group_index": group.get("index") if group else None,
                     "class": ch.get("ClassName"), "name": ch.get("Name"),
                     "flags": ch.get("Flags"), "version": ch.get("Version"),
                     "time_raw": ch.get("Time")}
                extra = {k: v for k, v in ch.attrib.items()
                         if k not in ("ClassName", "Name", "Flags", "Version", "Time")}
                if extra:
                    m["extra_attrib"] = extra
                try:
                    t = float(ch.get("Time"))
                    if not math.isfinite(t):
                        raise ValueError
                    m["time_us"] = t
                    m["time_us_rounded"] = int(math.floor(t + 0.5))
                except (TypeError, ValueError):
                    log.add("ldx.marker_time_unparsable", "marker Time unparsable",
                            time=ch.get("Time"))
                if m["class"] not in KNOWN_MARKER_CLASSES:
                    unknown_classes.add(m["class"])
                markers.append(m)
            elif tag == "MarkerPair":
                pairs.append(dict(ch.attrib))
            elif tag in ("String", "DateTime", "Numeric") and path.endswith("Details"):
                d = {"type": tag}
                d.update(ch.attrib)
                details.append(d)
            walk(ch, p, g)

    walk(root, root.tag, None)
    for c in sorted(x for x in unknown_classes if x is not None):
        log.add("ldx.unknown_marker_class", "unknown marker class %s" % c, cls=c)
    for p, n in sorted(unknown.items()):
        log.add("ldx.unknown_element", "unknown element %s" % p, path=p, count=n)
    times = [m["time_us"] for m in markers if "time_us" in m]
    if times != sorted(times):
        log.add("ldx.marker_unsorted", "markers not in ascending time order")
    if duration_s and times and max(times) > duration_s * 1e6 + 1:
        log.add("ldx.marker_beyond_end", "marker after end of data",
                max_time_us=max(times), duration_us=duration_s * 1e6)
    out["markers"] = markers
    out["marker_pairs"] = pairs
    out["details"] = details
    out["unknown_elements"] = unknown
    return out


# --------------------------------------------------------------------------- text report

def render_text(r, out):
    w = out.write
    w("== %s (%d bytes)\n" % (r["file"], r["size"]))
    h = r.get("header")
    if not h:
        w("  not decodable\n")
        return
    w("  magic=0x%X meta_ptr=0x%X data_ptr=0x%X event_ptr=0x%X num_channels=%d\n" % (
        h["magic"], h["meta_ptr"], h["data_ptr"], h["event_ptr"], h["num_channels"]))
    w("  device: type=%r serial=%d version=%d  statics=0x%04X,0x%04X,0x%04X,0x%04X  pro_logging=0x%X\n" % (
        h["device_type"], h["device_serial"], h["device_version"], h["static_0x040"], h["static_0x042"],
        h["static_0x044"], h["static_0x054"], h["pro_logging"]))
    w("  date=%r time=%r driver=%r vehicle=%r venue=%r session=%r short_comment=%r\n" % (
        h["date"], h["time"], h["driver"], h["vehicle_id"], h["venue"], h["session"], h["short_comment"]))
    bl = r.get("blocks", {})
    if bl.get("event"):
        ev = bl["event"]
        w("  event: name=%r session=%r comment=%r venue_ptr=%d\n" % (
            ev["name"], ev["session"], ev["long_comment"][:60], ev["venue_ptr_u32"]))
    if bl.get("venue"):
        w("  venue: name=%r vehicle_ptr=%d\n" % (bl["venue"]["name"], bl["venue"]["vehicle_ptr_u32"]))
    if bl.get("vehicle"):
        vh = bl["vehicle"]
        w("  vehicle: id=%r weight=%d type=%r comment=%r\n" % (vh["id"], vh["weight"], vh["type"], vh["comment"]))
    lay = r.get("layout", {})
    w("  channels=%d stride=%s counters=%s duration=%.3fs  timing_ms=%s\n" % (
        len(r.get("channels", [])), lay.get("stride"), lay.get("counter_pattern"),
        r.get("duration_s", 0.0), r.get("timing_ms")))
    w("  %-4s %-32s %-8s %-10s %5s %-9s %6s %5s %5s %4s %10s %10s  %s\n" % (
        "idx", "name", "short", "unit", "Hz", "dtype", "shift", "mul", "scale", "dec", "count",
        "data_ptr", "min/max/mean (beforeMul)"))
    for c in r.get("channels", []):
        st = c.get("stats", {}).get("beforeMul", {})
        w("  %-4d %-32s %-8s %-10s %5d %-9s %6d %5d %5d %4d %10d %10d  %s/%s/%s\n" % (
            c["index"], c["name"][:32], c["short_name"], c["unit"], c["freq"],
            "%X/%d" % (c["dtype_a"], c["dtype"]), c["shift"], c["mul"], c["scale"], c["dec"],
            c["count"], c["data_ptr"], _fmt(st.get("min")), _fmt(st.get("max")), _fmt(st.get("mean"))))
        if "afterMul" in c.get("stats", {}):
            sa = c["stats"]["afterMul"]
            w("       afterMul: %s/%s/%s\n" % (_fmt(sa.get("min")), _fmt(sa.get("max")), _fmt(sa.get("mean"))))
    lx = r.get("ldx")
    if lx:
        w("  ldx: %s markers=%d details=%s\n" % (lx["file"], len(lx.get("markers", [])),
                                                 {d.get("Id"): d.get("Value") for d in lx.get("details", [])}))
        for m in lx.get("markers", [])[:50]:
            w("    marker %s %s %s t=%sus\n" % (m["group"], m["class"], m["name"], m.get("time_us")))
    for x in r.get("warnings", []):
        w("  [%s] %s: %s%s\n" % (x["severity"], x["code"], x["message"],
                                 (" (x%d)" % x["count"]) if x.get("count", 1) > 1 else ""))


def _fmt(x):
    if x is None:
        return "-"
    if isinstance(x, float):
        return "%.6g" % x
    return str(x)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Independent reference decoder for .ld/.ldx (D-041).")
    ap.add_argument("files", nargs="*", help=".ld files to decode")
    ap.add_argument("--json", action="store_true", help="emit JSON (one object, or a list for several files)")
    ap.add_argument("-o", "--out", help="write output to this file instead of stdout")
    ap.add_argument("--max-stat-samples", type=int, default=4_000_000,
                    help="per channel; above this, statistics use a strided subsample (0 = exact always)")
    ap.add_argument("--entry-size", default="auto", help="auto | 124 | 212: channel record stride")
    ap.add_argument("--shift-order", choices=["beforeMul", "afterMul"], default="beforeMul",
                    help="order used for dumps of channels with shift == 0 or mul == 1 (both always dumped otherwise)")
    ap.add_argument("--dump-channel", action="append",
                    help="channel name or index to dump (repeatable); '*' dumps all")
    ap.add_argument("--dump-format", choices=["f64", "csv", "both"], default="f64")
    ap.add_argument("--dump-dir", default=".")
    ap.add_argument("--no-stats", action="store_true")
    ap.add_argument("--no-hash", action="store_true")
    ap.add_argument("--no-ldx", action="store_true")
    ap.add_argument("--enum-labels", action="store_true", help="include enum labels in the output")
    ap.add_argument("--list-codes", action="store_true", help="print the warning code table and exit")
    a = ap.parse_args(argv)
    if a.list_codes:
        for k, (sev, d) in sorted(WARNING_CODES.items()):
            print("%s\t%s\t%s" % (k, sev, d))
        return 0
    if not a.files:
        ap.error("no input files")
    results = []
    for f in a.files:
        results.append(decode_file(f, max_samples=a.max_stat_samples, entry_size=a.entry_size,
                                   want_hash=not a.no_hash, stats=not a.no_stats,
                                   dump=a.dump_channel, dump_dir=a.dump_dir,
                                   dump_format=a.dump_format, shift_order=a.shift_order,
                                   ldx=not a.no_ldx, enum_labels=a.enum_labels))
    fh = open(a.out, "w", encoding="utf-8") if a.out else sys.stdout
    try:
        if a.json:
            json.dump(results[0] if len(results) == 1 else results, fh, indent=1, ensure_ascii=False)
            fh.write("\n")
        else:
            for r in results:
                render_text(r, fh)
    finally:
        if a.out:
            fh.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
