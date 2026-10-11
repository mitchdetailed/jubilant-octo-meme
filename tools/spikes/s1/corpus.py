#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Spike S-1: enumerate the user's real .ld/.ldx files (paths from the S-1 task).

Writes fixtures/private/s1/corpus.json (gitignored). Paths are private data and
never leave fixtures/private.
"""
import glob
import hashlib
import json
import math
import os
import sys


def _strict_json(v):
    """NaN and infinities become null: the spike outputs are strict JSON (RFC 8259), as every JSON file under
    fixtures/ must be for `esses-cli validate` (AC-M0-10)."""
    if isinstance(v, float) and not math.isfinite(v):
        return None
    if isinstance(v, dict):
        return {k: _strict_json(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_strict_json(x) for x in v]
    return v

# Corpus roots are machine-specific and private, so they are not kept in the repository.
# They are read from fixtures/private/s1/roots.json (gitignored), shaped as
#   {"roots": [["<folder>", <recursive: true|false>, "<tag: task|extra>"], ...]}
# and fall back to the default MoTeC logged-data folder when that file is absent.
_ROOTS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                           "fixtures", "private", "s1", "roots.json")


def _load_roots():
    try:
        with open(_ROOTS_FILE, encoding="utf-8") as fh:
            return [tuple(r) for r in json.load(fh)["roots"]]
    except (OSError, ValueError, KeyError):
        return [("C:/MoTeC/Logged Data", True, "task")]


ROOTS = _load_roots()


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as fh:
        for blk in iter(lambda: fh.read(1 << 24), b""):
            h.update(blk)
    return h.hexdigest()


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    out_dir = os.path.join(repo, "fixtures", "private", "s1")
    os.makedirs(out_dir, exist_ok=True)
    files = []
    seen = set()
    for root, rec, origin in ROOTS:
        if not os.path.isdir(root):
            print("missing root:", root, file=sys.stderr)
            continue
        if rec:
            cands = []
            for dp, dn, fn in os.walk(root):
                for f in fn:
                    cands.append(os.path.join(dp, f))
        else:
            cands = [os.path.join(root, f) for f in os.listdir(root)]
        for p in sorted(cands):
            ext = os.path.splitext(p)[1].lower()
            if ext not in (".ld", ".ldx") or not os.path.isfile(p):
                continue
            key = os.path.normcase(os.path.abspath(p))
            if key in seen:
                continue
            seen.add(key)
            files.append({"path": os.path.abspath(p), "ext": ext, "size": os.path.getsize(p),
                          "root": root, "origin": origin})
    by_hash = {}
    for f in files:
        if f["ext"] == ".ld":
            f["sha256"] = sha256(f["path"])
            by_hash.setdefault(f["sha256"], []).append(f["path"])
    for f in files:
        if f.get("sha256"):
            dup = [p for p in by_hash[f["sha256"]] if p != f["path"]]
            if dup:
                f["identical_to"] = dup
    with open(os.path.join(out_dir, "corpus.json"), "w", encoding="utf-8") as fh:
        json.dump(_strict_json(files), fh, indent=1, allow_nan=False)
    lds = [f for f in files if f["ext"] == ".ld"]
    print("ld files: %d (%d distinct contents, %.1f MB), ldx files: %d" % (
        len(lds), len(by_hash), sum(f["size"] for f in lds) / 1e6, len(files) - len(lds)))


if __name__ == "__main__":
    main()
