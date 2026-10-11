#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Spike S-1: self-test of tools/ld_ref_decode.py on hand-built synthetic files.

The files are packed here directly from the public layout tables (docs/formats/ld.md),
written to a temporary directory and deleted afterwards. Each case asserts decoded
values and the warning codes the decoder must emit.

  python -I tools/spikes/s1/selftest_synthetic.py
"""
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import ld_ref_decode as ref  # noqa: E402

HDR = 0x6E2


def s(text, n):
    b = text.encode("cp1252")[:n]
    return b + b"\x00" * (n - len(b))


def build(path, channels, *, num_channels=None, event=True, cycle=False, entry=124,
          terminator=False, truncate=0, pad_before_event=0):
    """channels: list of dicts with name, short, unit, dtype_a, dtype, freq, shift, mul, scale, dec,
    raw(bytes), count(optional override)."""
    buf = bytearray(HDR)
    pos = HDR + pad_before_event
    event_ptr = venue_ptr = vehicle_ptr = 0
    blocks = bytearray()
    if event:
        event_ptr = pos
        venue_ptr = event_ptr + 1156
        vehicle_ptr = venue_ptr + 1102
        ev = bytearray(1156)
        ev[0:64] = s("Synthetic Event", 64)
        ev[0x40:0x80] = s("Session 1", 64)
        ev[0x80:0x480] = s("long comment", 1024)
        struct.pack_into("<I", ev, 0x480, venue_ptr)
        ve = bytearray(1102)
        ve[0:64] = s("Test Venue", 64)
        struct.pack_into("<I", ve, 0x44A, vehicle_ptr)
        vh = bytearray(260)
        vh[0:64] = s("Car 7", 64)
        struct.pack_into("<I", vh, 0xC0, 1234)
        vh[0xC4:0xE4] = s("Sedan", 32)
        vh[0xE4:0x104] = s("vcomment", 32)
        blocks = ev + ve + vh
        pos = vehicle_ptr + 260
    meta_ptr = pos
    nrec = len(channels) + (1 if terminator else 0)
    data_ptr = meta_ptr + nrec * entry
    data = bytearray()
    recs = bytearray()
    dptr = data_ptr
    for i, c in enumerate(channels):
        off = meta_ptr + i * entry
        r = bytearray(entry)
        prev = 0 if i == 0 else off - entry
        nxt = off + entry if (i + 1 < nrec) else 0
        if cycle and i == len(channels) - 1:
            nxt = meta_ptr
        count = c.get("count", len(c["raw"]) // max(1, c["dtype"]))
        struct.pack_into("<IIIIHHHHhhhh", r, 0, prev, nxt, dptr, count, 0x2EE1 + i, c["dtype_a"], c["dtype"],
                         c["freq"], c.get("shift", 0), c.get("mul", 1), c.get("scale", 1), c.get("dec", 0))
        r[0x20:0x40] = s(c["name"], 32) if len(c["name"]) < 32 else c["name"].encode()[:32]
        r[0x40:0x48] = s(c.get("short", ""), 8)
        r[0x48:0x54] = s(c.get("unit", ""), 12)
        if entry >= 212 and "enum_id" in c:
            struct.pack_into("<HH", r, 0xD0, 2, c["enum_id"])
        recs += r
        data += c["raw"]
        dptr += len(c["raw"])
    if terminator:
        recs += bytearray(entry)
    struct.pack_into("<I", buf, 0x00, 0x40)
    struct.pack_into("<I", buf, 0x08, meta_ptr)
    struct.pack_into("<I", buf, 0x0C, data_ptr)
    struct.pack_into("<I", buf, 0x24, event_ptr)
    struct.pack_into("<I", buf, 0x46, 4242)
    buf[0x4A:0x52] = s("SYN", 8)
    struct.pack_into("<H", buf, 0x52, 100)
    struct.pack_into("<I", buf, 0x56, len(channels) if num_channels is None else num_channels)
    buf[0x5E:0x6E] = s("09/10/2026", 16)
    buf[0x7E:0x8E] = s("13:14:15", 16)
    buf[0x9E:0xDE] = s("Driver", 64)
    buf[0xDE:0x11E] = s("Vehicle", 64)
    buf[0x15E:0x19E] = s("Venue", 64)
    buf[0x624:0x664] = s("short", 64)
    out = bytes(buf) + b"\x00" * pad_before_event + bytes(blocks) + bytes(recs) + bytes(data)
    if truncate:
        out = out[:-truncate]
    with open(path, "wb") as fh:
        fh.write(out)


def ch(name, a, w, fmt, values, **kw):
    d = {"name": name, "dtype_a": a, "dtype": w, "freq": kw.pop("freq", 10),
         "raw": struct.pack("<%d%s" % (len(values), fmt), *values)}
    d.update(kw)
    return d


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


def codes(r):
    return set(r["warning_codes"])


@case
def basic_types(tmp):
    p = os.path.join(tmp, "basic.ld")
    build(p, [ch("I16 dec1", 3, 2, "h", [0, 199, -5], dec=1, unit="km/h"),
              ch("I32 scale2", 5, 4, "i", [10, 11, -7], scale=2),
              ch("F32", 7, 4, "f", [1.5, -2.25, 3.0]),
              ch("F16", 7, 2, "e", [0.5, -1.0, 65504.0]),
              ch("F64", 8, 8, "d", [-27.6901783, 153.0, 0.1]),
              ch("I16 legacy0", 0, 2, "h", [1, 2, 3])],
          pad_before_event=70000)            # event block beyond 64 KiB: u32 pointers needed
    r = ref.decode_file(p)
    v = {c["name"]: c["stats"]["beforeMul"]["first"] for c in r["channels"]}
    assert v["I16 dec1"] == [0.0, 19.9, -0.5], v["I16 dec1"]
    assert v["I32 scale2"] == [5.0, 5.5, -3.5]
    assert v["F32"] == [1.5, -2.25, 3.0]
    assert v["F16"] == [0.5, -1.0, 65504.0]
    assert v["F64"] == [-27.6901783, 153.0, 0.1]
    assert v["I16 legacy0"] == [1.0, 2.0, 3.0]
    assert r["blocks"]["venue"]["name"] == "Test Venue" and r["blocks"]["vehicle"]["weight"] == 1234
    assert r["blocks"]["vehicle"]["type"] == "Sedan"
    assert "ld.ptr_width_ambiguous" in codes(r)
    assert r["header"]["datetime_local"] == "2026-10-09T13:14:15"
    assert r["layout"]["counter_pattern"] == "0x2EE1+i"
    assert "ld.count_mismatch" not in codes(r)


@case
def shift_mul_orders(tmp):
    p = os.path.join(tmp, "shiftmul.ld")
    build(p, [ch("Temp F", 3, 2, "h", [250, 0, -100], shift=32, mul=9, scale=5, dec=1, unit="F")])
    r = ref.decode_file(p)
    c = r["channels"][0]
    assert c["stats"]["beforeMul"]["first"] == [333.0, 288.0, 270.0], c["stats"]["beforeMul"]
    assert c["stats"]["afterMul"]["first"] == [77.0, 32.0, 14.0], c["stats"]["afterMul"]
    assert {"ld.shift_nonzero", "ld.shift_mul_order"} <= codes(r)
    vals = ref.read_channels(p, [0], order="afterMul")[0][1]
    assert list(vals) == [77.0, 32.0, 14.0]


@case
def dumps(tmp):
    p = os.path.join(tmp, "dump.ld")
    build(p, [ch("A", 3, 2, "h", [1, 2, 3], dec=1, freq=3)])
    d = os.path.join(tmp, "dumpdir")
    ref.main([p, "--dump-channel", "A", "--dump-format", "both", "--dump-dir", d, "-o", os.path.join(tmp, "o.txt")])
    with open(os.path.join(d, "000_A.f64"), "rb") as fh:
        assert struct.unpack("<3d", fh.read()) == (0.1, 0.2, 0.3)
    with open(os.path.join(d, "000_A.csv")) as fh:
        lines = fh.read().splitlines()
    assert lines[0] == "index,timeUs,value,valid"
    assert lines[2] == "1,333333,0.2,1", lines[2]          # timeAt(1) at 3 Hz = 333,333 us


@case
def cycle_and_counts(tmp):
    p = os.path.join(tmp, "cycle.ld")
    build(p, [ch("A", 3, 2, "h", [1]), ch("B", 3, 2, "h", [2])], cycle=True, num_channels=(7 << 16) | 2)
    r = ref.decode_file(p)
    assert "ld.list_cycle" in codes(r) and len(r["channels"]) == 2
    assert "ld.count_upper_bits" in codes(r)


@case
def truncated_and_zero_rate(tmp):
    p = os.path.join(tmp, "trunc.ld")
    build(p, [ch("Z", 3, 2, "h", [5, 6], freq=0), ch("T", 3, 4, "i", [1, 2, 3, 4])], truncate=6)
    r = ref.decode_file(p)
    t = r["channels"][1]
    assert t["count"] == 4 and t["count_decoded"] == 2, t
    assert {"ld.data_out_of_range", "ld.zero_rate"} <= codes(r)


@case
def unknown_and_guessed_dtypes(tmp):
    p = os.path.join(tmp, "dtypes.ld")
    build(p, [ch("Odd", 0x0F, 2, "h", [1, -1]), ch("State", 6, 4, "i", [140926, 3])])
    r = ref.decode_file(p)
    assert {"ld.unknown_dtype", "ld.dtype_guessed"} <= codes(r)
    assert r["channels"][1]["stats"]["beforeMul"]["first"] == [140926.0, 3.0]


@case
def m1_records(tmp):
    p = os.path.join(tmp, "m1.ld")
    build(p, [ch("Gear", 3, 2, "h", [0, 1, 2], enum_id=5, short="", unit=""),
              ch("A 32 character channel name here", 7, 4, "f", [1.0], short="x", unit="")],
          entry=212, terminator=True)
    r = ref.decode_file(p)
    assert {"ld.entry_stride", "ld.terminator_entry", "ld.name_unterminated",
            "ld.name_continuation_candidate"} <= codes(r), codes(r)
    assert len(r["channels"]) == 2 and r["channels"][0]["enum_id"] == 5
    assert r["channels"][1]["name_continuation_candidate"].endswith("herex")


@case
def ldx_markers(tmp):
    p = os.path.join(tmp, "laps.ld")
    build(p, [ch("Speed", 3, 2, "h", list(range(100)), freq=1)])
    with open(os.path.join(tmp, "laps.LDX"), "w", encoding="ascii") as fh:
        fh.write('<?xml version="1.0"?>\n<LDXFile Locale="English_United Kingdom.1252" DefaultLocale="C" Version="1.6">'
                 '<Layers><Layer><MarkerBlock><MarkerGroup Name="Beacons" Index="2">'
                 '<Marker Version="100" ClassName="BCN" Name="Manual.1" Flags="77" Time="30000000.00"/>'
                 '<Marker Version="100" ClassName="SOR" Name="x" Flags="77" Time="20000000.50"/>'
                 '<Marker Version="100" ClassName="BCN" Name="Manual.2" Flags="77" Time="150000000.00"/>'
                 '</MarkerGroup></MarkerBlock><Mystery a="1"/></Layer>'
                 '<Details><String Id="Total Laps" Value="3"/><String Id="Fastest Time" Value="0:30.000"/></Details>'
                 '</Layers></LDXFile>')
    r = ref.decode_file(p)
    m = r["ldx"]["markers"]
    assert [x["time_us_rounded"] for x in m] == [30000000, 20000001, 150000000]
    assert m[0]["group"] == "Beacons" and m[0]["flags"] == "77"
    assert {"ldx.unknown_marker_class", "ldx.unknown_element", "ldx.marker_unsorted",
            "ldx.marker_beyond_end"} <= codes(r), codes(r)
    assert r["ldx"]["details"][0]["Id"] == "Total Laps"


@case
def ldx_malformed(tmp):
    p = os.path.join(tmp, "bad.ld")
    build(p, [ch("A", 3, 2, "h", [1])])
    with open(os.path.join(tmp, "bad.ldx"), "w") as fh:
        fh.write("<LDXFile><Layers>")
    r = ref.decode_file(p)
    assert "ldx.parse_error" in codes(r)


@case
def strided_stats(tmp):
    p = os.path.join(tmp, "big.ld")
    n = 50_001
    build(p, [ch("Ramp", 5, 4, "i", list(range(n)))])
    r = ref.decode_file(p, max_samples=1000)
    c = r["channels"][0]
    assert c["raw_stats"]["stride"] > 1 and "ld.stats_sampled" in codes(r)
    assert c["raw_stats"]["first"] == [0, 1, 2, 3, 4] and c["raw_stats"]["last"][-1] == n - 1
    exact = ref.decode_file(p, max_samples=0)["channels"][0]["raw_stats"]
    assert exact["min"] == 0 and exact["max"] == n - 1 and exact["mean"] == (n - 1) / 2


def main():
    tmp = tempfile.mkdtemp(prefix="ldref-selftest-")
    passed = 0
    failed = []
    try:
        for fn in CASES:
            try:
                fn(tmp)
                passed += 1
                print("PASS", fn.__name__)
            except AssertionError as ex:
                failed.append(fn.__name__)
                print("FAIL", fn.__name__, ex)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d/%d passed" % (passed, len(CASES)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
