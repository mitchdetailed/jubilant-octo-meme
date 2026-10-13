#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Schema self-check (02_DATA_MODEL.md sections 6.5-6.6, D-072; 08_REPO_AND_BUILD.md section 5).

Checks schemas/*.schema.json and the examples under schemas/examples/<schema-id>/:
  1. Files: UTF-8 without BOM, LF line endings, a final newline, strict JSON (no duplicate keys, no NaN or
     Infinity).
  2. Schemas: named <name>-<major>.schema.json; draft-07 "$schema"; "$id" = urn:esses:<name>-<major> (D-072);
     non-empty "title" and "description"; root type object requiring "$schema" and "schemaVersion", whose
     "$schema" pattern accepts the urn:esses:, esses: and https forms of this id only (02 section 6.6 rule 1).
     Built-in maths settings schemas, builtin-<module id>-<version>.schema.json (03 section 9; the module id
     may contain '.' and '_'), validate a member of project.json/sidecar documents rather than a document:
     they follow every rule except the "$schema"/"schemaVersion" envelope.
  3. Portability: only the draft-07 keywords that both the vendored json-schema-validator 2.3.0 and the
     validator below implement (no "format": the C++ validator needs a format checker for it); only local
     "#/definitions/<name>" references, all resolvable, no unused definitions; required members listed in
     "properties" when additional properties are closed; patterns free of constructs that ECMAScript regex
     (std::regex, used by the C++ validator) does not share with Python's re.
  4. Shared definitions: a definition name used in several schemas has the same content in each.
  5. Examples: every valid example validates; every invalid example fails, and fails at the JSON pointer
     its schemas/examples/<schema-id>/expect.json entry names. Every schema has both kinds of example.

The validator here is a draft-07 subset that reports errors at the instance locations the C++ validator uses
(required, additionalProperties, propertyNames, oneOf, anyOf and not at the parent). The C++ test
tests/schema_examples_tests.cpp runs the same examples through json-schema-validator.

Standard library only. `python -I tools/check_schemas.py [--root DIR]`; `--self-test` checks the checker on
built-in cases. Exit status: 0 clean, 1 findings, 2 usage error.
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
import sys
from typing import Any, Callable

DRAFT07 = "http://json-schema.org/draft-07/schema#"
SCHEMA_FILE_RE = re.compile(r"^([a-z][a-z0-9-]*?)-([1-9][0-9]*)\.schema\.json$")
# Built-in maths module settings (03 section 9): builtin-<module id>-<version>, e.g.
# builtin-esses.corrected_speed_distance-1; a settings fragment without the document envelope.
BUILTIN_FILE_RE = re.compile(r"^(builtin-[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*)-([1-9][0-9]*)\.schema\.json$")
REF_RE = re.compile(r"^#/definitions/([A-Za-z0-9_]+)$")

ANNOTATIONS = {"$comment", "title", "description", "default", "examples"}
KEYWORDS = ANNOTATIONS | {
    "type", "enum", "const",
    "properties", "required", "additionalProperties", "patternProperties", "propertyNames",
    "minProperties", "maxProperties",
    "items", "additionalItems", "minItems", "maxItems", "uniqueItems", "contains",
    "minLength", "maxLength", "pattern",
    "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "multipleOf",
    "allOf", "anyOf", "oneOf", "not", "if", "then", "else",
    "$ref",
}
ROOT_ONLY = {"$schema", "$id", "definitions"}
TYPES = {"null", "boolean", "object", "array", "string", "integer", "number"}
INT64_MAX = 2**63 - 1
INT64_MIN = -(2**63)
# Regex constructs that Python's re and ECMAScript std::regex do not share (or that std::regex lacks).
NONPORTABLE_REGEX = [
    (re.compile(r"\(\?"), "inline groups/flags/lookaround '(?'"),
    (re.compile(r"\\[AZzGhHRXpPNkK]"), "escape not shared by ECMAScript and Python"),
    (re.compile(r"[*+?}][+]"), "possessive quantifier"),
    (re.compile(r"[^\x20-\x7e]"), "non-ASCII character (std::regex matches bytes)"),
]


class JsonError(ValueError):
    pass


# ---- strict JSON ------------------------------------------------------------------------------------------
def _no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key, value in pairs:
        if key in out:
            raise JsonError(f"duplicate key '{key}'")
        out[key] = value
    return out


def _no_constants(name: str) -> Any:
    raise JsonError(f"non-finite number {name} is not JSON")


def loads_strict(text: str) -> Any:
    return json.loads(text, object_pairs_hook=_no_duplicates, parse_constant=_no_constants)


def load_file(path: pathlib.Path) -> Any:
    raw = path.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raise JsonError("UTF-8 byte order mark")
    if b"\r" in raw:
        raise JsonError("CR line ending (LF only)")
    if not raw.endswith(b"\n"):
        raise JsonError("no final newline")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise JsonError(f"not UTF-8: {exc}") from None
    try:
        return loads_strict(text)
    except json.JSONDecodeError as exc:
        raise JsonError(f"JSON syntax: {exc}") from None


# ---- draft-07 subset validator ---------------------------------------------------------------------------
def pointer_join(ptr: str, token: Any) -> str:
    return ptr + "/" + str(token).replace("~", "~0").replace("/", "~1")


def json_type(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    return type(value).__name__


def type_matches(value: Any, name: str) -> bool:
    actual = json_type(value)
    # As in json-schema-validator: "integer" accepts JSON integers only (1.0 is a float); "number" accepts both.
    return actual == name or (name == "number" and actual == "integer")


def json_equal(a: Any, b: Any) -> bool:
    """JSON equality: booleans never equal numbers; 1 == 1.0; objects by members; arrays by order."""
    ta, tb = json_type(a), json_type(b)
    if {ta, tb} <= {"integer", "number"}:
        return a == b
    if ta != tb:
        return False
    if ta == "array":
        return len(a) == len(b) and all(json_equal(x, y) for x, y in zip(a, b))
    if ta == "object":
        return a.keys() == b.keys() and all(json_equal(a[k], b[k]) for k in a)
    return bool(a == b)


class Validator:
    def __init__(self, root: dict[str, Any]) -> None:
        self.root = root
        self._regex: dict[str, re.Pattern[str]] = {}

    def resolve(self, ref: str) -> Any:
        m = REF_RE.match(ref)
        if not m or m.group(1) not in self.root.get("definitions", {}):
            raise KeyError(f"unresolvable $ref '{ref}'")
        return self.root["definitions"][m.group(1)]

    def regex(self, pattern: str) -> re.Pattern[str]:
        if pattern not in self._regex:
            self._regex[pattern] = re.compile(pattern)
        return self._regex[pattern]

    def errors(self, instance: Any) -> list[tuple[str, str]]:
        out: list[tuple[str, str]] = []
        self.validate(instance, self.root, "", out)
        return out

    def passes(self, instance: Any, schema: Any, ptr: str) -> bool:
        sub: list[tuple[str, str]] = []
        self.validate(instance, schema, ptr, sub)
        return not sub

    def validate(self, inst: Any, schema: Any, ptr: str, out: list[tuple[str, str]]) -> None:
        if schema is True:
            return
        if schema is False:
            out.append((ptr, "false schema"))
            return
        if "$ref" in schema:  # draft-07: siblings of $ref are ignored
            self.validate(inst, self.resolve(schema["$ref"]), ptr, out)
            return
        if "type" in schema:
            names = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
            if not any(type_matches(inst, n) for n in names):
                out.append((ptr, f"type {json_type(inst)} is not {'/'.join(names)}"))
        if "enum" in schema and not any(json_equal(inst, v) for v in schema["enum"]):
            out.append((ptr, "not one of the enum values"))
        if "const" in schema and not json_equal(inst, schema["const"]):
            out.append((ptr, f"not the constant {json.dumps(schema['const'])}"))
        kind = json_type(inst)
        if kind == "object":
            self._object(inst, schema, ptr, out)
        elif kind == "array":
            self._array(inst, schema, ptr, out)
        elif kind == "string":
            self._string(inst, schema, ptr, out)
        elif kind in ("integer", "number"):
            self._number(inst, schema, ptr, out)
        for sub in schema.get("allOf", []):
            self.validate(inst, sub, ptr, out)
        if "anyOf" in schema and not any(self.passes(inst, s, ptr) for s in schema["anyOf"]):
            out.append((ptr, "no anyOf subschema matches"))
        if "oneOf" in schema:
            count = sum(1 for s in schema["oneOf"] if self.passes(inst, s, ptr))
            if count != 1:
                out.append((ptr, f"{count} oneOf subschemas match, exactly one must"))
        if "not" in schema and self.passes(inst, schema["not"], ptr):
            out.append((ptr, "matches the 'not' subschema"))
        if "if" in schema and ("then" in schema or "else" in schema):
            branch = "then" if self.passes(inst, schema["if"], ptr) else "else"
            if branch in schema:
                self.validate(inst, schema[branch], ptr, out)

    def _object(self, inst: dict[str, Any], schema: dict[str, Any], ptr: str, out: list[tuple[str, str]]) -> None:
        if "maxProperties" in schema and len(inst) > schema["maxProperties"]:
            out.append((ptr, "too many properties"))
        if "minProperties" in schema and len(inst) < schema["minProperties"]:
            out.append((ptr, "too few properties"))
        for name in schema.get("required", []):
            if name not in inst:
                out.append((ptr, f"required property '{name}' missing"))
        props = schema.get("properties", {})
        patterns = schema.get("patternProperties", {})
        for key, value in inst.items():
            if "propertyNames" in schema and not self.passes(key, schema["propertyNames"], ptr):
                out.append((ptr, f"property name '{key}' is invalid"))
            matched = False
            if key in props:
                matched = True
                self.validate(value, props[key], pointer_join(ptr, key), out)
            for pattern, sub in patterns.items():
                if self.regex(pattern).search(key):
                    matched = True
                    self.validate(value, sub, pointer_join(ptr, key), out)
            if not matched and "additionalProperties" in schema:
                if not self.passes(value, schema["additionalProperties"], pointer_join(ptr, key)):
                    out.append((ptr, f"additional property '{key}' is invalid"))

    def _array(self, inst: list[Any], schema: dict[str, Any], ptr: str, out: list[tuple[str, str]]) -> None:
        if "maxItems" in schema and len(inst) > schema["maxItems"]:
            out.append((ptr, "too many items"))
        if "minItems" in schema and len(inst) < schema["minItems"]:
            out.append((ptr, "too few items"))
        if schema.get("uniqueItems"):
            for i in range(len(inst)):
                if any(json_equal(inst[i], inst[j]) for j in range(i)):
                    out.append((ptr, "items are not unique"))
                    break
        items = schema.get("items", True)
        if isinstance(items, list):
            for i, value in enumerate(inst):
                sub = items[i] if i < len(items) else schema.get("additionalItems", True)
                self.validate(value, sub, pointer_join(ptr, i), out)
        else:
            for i, value in enumerate(inst):
                self.validate(value, items, pointer_join(ptr, i), out)
        if "contains" in schema and not any(self.passes(v, schema["contains"], ptr) for v in inst):
            out.append((ptr, "no item matches 'contains'"))

    def _string(self, inst: str, schema: dict[str, Any], ptr: str, out: list[tuple[str, str]]) -> None:
        if "maxLength" in schema and len(inst) > schema["maxLength"]:
            out.append((ptr, "string too long"))
        if "minLength" in schema and len(inst) < schema["minLength"]:
            out.append((ptr, "string too short"))
        if "pattern" in schema and not self.regex(schema["pattern"]).search(inst):
            out.append((ptr, f"does not match pattern {schema['pattern']}"))

    def _number(self, inst: float, schema: dict[str, Any], ptr: str, out: list[tuple[str, str]]) -> None:
        if "maximum" in schema and inst > schema["maximum"]:
            out.append((ptr, "above maximum"))
        if "exclusiveMaximum" in schema and inst >= schema["exclusiveMaximum"]:
            out.append((ptr, "not below exclusiveMaximum"))
        if "minimum" in schema and inst < schema["minimum"]:
            out.append((ptr, "below minimum"))
        if "exclusiveMinimum" in schema and inst <= schema["exclusiveMinimum"]:
            out.append((ptr, "not above exclusiveMinimum"))
        if "multipleOf" in schema and inst != 0:
            q = inst / schema["multipleOf"]
            if not math.isclose(q, round(q), rel_tol=0, abs_tol=1e-9):
                out.append((ptr, "not a multiple"))


# ---- schema lint --------------------------------------------------------------------------------------------
def accepts(pattern: str, text: str) -> bool:
    return re.search(pattern, text) is not None


def schema_file_id(name: str) -> tuple[str, str, bool] | None:
    """(schema id, major, is a built-in settings fragment) of a schema file name, or None."""
    m = BUILTIN_FILE_RE.match(name)
    if m:
        return f"{m.group(1)}-{m.group(2)}", m.group(2), True
    m = SCHEMA_FILE_RE.match(name)
    if m:
        return f"{m.group(1)}-{m.group(2)}", m.group(2), False
    return None


def lint_schema(name: str, schema: Any) -> tuple[list[str], set[str]]:
    """Returns (findings, referenced definition names)."""
    findings: list[str] = []
    refs: set[str] = set()
    ident = schema_file_id(name)
    if ident is None:
        return [f"{name}: file name is not <name>-<major>.schema.json"], refs
    schema_id, major, fragment = ident
    if not isinstance(schema, dict):
        return [f"{name}: the schema is not an object"], refs
    if schema.get("$schema") != DRAFT07:
        findings.append(f"{name}: $schema is not {DRAFT07}")
    if schema.get("$id") != f"urn:esses:{schema_id}":
        findings.append(f"{name}: $id is not urn:esses:{schema_id} (D-072)")
    for key in ("title", "description"):
        if not isinstance(schema.get(key), str) or not schema[key].strip():
            findings.append(f"{name}: no {key}")
    if schema.get("type") != "object":
        findings.append(f"{name}: the root type is not object")
    required = schema.get("required", [])
    props = schema.get("properties", {})
    for member in () if fragment else ("$schema", "schemaVersion"):
        if member not in required or member not in props:
            findings.append(f"{name}: '{member}' is not a required root property (D-072)")
    id_pattern = props.get("$schema", {}).get("pattern") if isinstance(props.get("$schema"), dict) else None
    if fragment:
        pass  # a settings fragment has no document envelope
    elif isinstance(id_pattern, str):
        for good in (f"urn:esses:{schema_id}", f"esses:{schema_id}",
                     f"https://esses.example/schemas/{schema_id}.schema.json",
                     f"https://esses.example/{schema_id}.json"):
            if not accepts(id_pattern, good):
                findings.append(f"{name}: $schema pattern rejects '{good}'")
        stem = schema_id[: -len(major) - 1]
        for bad in (f"urn:esses:{schema_id}0", f"urn:esses:x{schema_id}", "urn:esses:other-1",
                    f"urn:esses:{stem}-{int(major) + 1}", f" urn:esses:{schema_id}"):
            if accepts(id_pattern, bad):
                findings.append(f"{name}: $schema pattern accepts '{bad}'")
    else:
        findings.append(f"{name}: the '$schema' property has no pattern")
    version = props.get("schemaVersion")
    if not fragment and not (isinstance(version, dict) and version.get("type") == "integer"
                             and version.get("minimum") == 1):
        findings.append(f"{name}: schemaVersion is not an integer with minimum 1")
    definitions = schema.get("definitions", {})
    if not isinstance(definitions, dict):
        findings.append(f"{name}: definitions is not an object")
        definitions = {}

    def walk(node: Any, where: str, is_root: bool) -> None:
        if isinstance(node, bool):
            return
        if not isinstance(node, dict):
            findings.append(f"{name}{where}: a subschema is not an object or boolean")
            return
        for key in node:
            if key in ROOT_ONLY and not is_root:
                findings.append(f"{name}{where}: '{key}' outside the root")
            elif key not in KEYWORDS and key not in ROOT_ONLY:
                findings.append(f"{name}{where}: unsupported or misspelt keyword '{key}'")
        if "$ref" in node:
            m_ref = REF_RE.match(str(node["$ref"]))
            if not m_ref or m_ref.group(1) not in definitions:
                findings.append(f"{name}{where}: unresolvable $ref {node['$ref']!r}")
            else:
                refs.add(m_ref.group(1))
            extra = set(node) - {"$ref"} - ANNOTATIONS
            if extra:
                findings.append(f"{name}{where}: keywords beside $ref are ignored: {sorted(extra)}")
        if "type" in node:
            names = node["type"] if isinstance(node["type"], list) else [node["type"]]
            for t in names:
                if t not in TYPES:
                    findings.append(f"{name}{where}: unknown type {t!r}")
        if "enum" in node:
            values = node["enum"]
            if not isinstance(values, list) or not values:
                findings.append(f"{name}{where}: enum is not a non-empty array")
            elif any(json_equal(values[i], values[j]) for i in range(len(values)) for j in range(i)):
                findings.append(f"{name}{where}: enum repeats a value")
        if "required" in node:
            req = node["required"]
            if not isinstance(req, list) or len(set(req)) != len(req):
                findings.append(f"{name}{where}: required is not a list of unique names")
            elif node.get("additionalProperties") is False and not node.get("patternProperties"):
                for r in req:
                    if r not in node.get("properties", {}):
                        findings.append(f"{name}{where}: required '{r}' can never be present (closed object)")
        for key in ("minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"):
            value = node.get(key)
            if isinstance(value, int) and not isinstance(value, bool) and not INT64_MIN <= value <= INT64_MAX:
                findings.append(f"{name}{where}: {key} {value} is outside int64 (the C++ validator compares as int64)")
        patterns = [node["pattern"]] if isinstance(node.get("pattern"), str) else []
        patterns += list(node.get("patternProperties", {}))
        for pattern in patterns:
            try:
                re.compile(pattern)
            except re.error as exc:
                findings.append(f"{name}{where}: pattern {pattern!r} does not compile: {exc}")
            for bad, why in NONPORTABLE_REGEX:
                if bad.search(pattern):
                    findings.append(f"{name}{where}: pattern {pattern!r}: {why}")
        for key in ("properties", "patternProperties"):
            for child, sub in node.get(key, {}).items():
                walk(sub, f"{where}/{key}/{child}", False)
        for key in ("additionalProperties", "propertyNames", "additionalItems", "contains", "not", "if", "then",
                    "else"):
            if key in node:
                walk(node[key], f"{where}/{key}", False)
        if "items" in node:
            if isinstance(node["items"], list):
                for i, sub in enumerate(node["items"]):
                    walk(sub, f"{where}/items/{i}", False)
            else:
                walk(node["items"], f"{where}/items", False)
        for key in ("allOf", "anyOf", "oneOf"):
            if key in node:
                if not isinstance(node[key], list) or not node[key]:
                    findings.append(f"{name}{where}: {key} is not a non-empty array")
                    continue
                for i, sub in enumerate(node[key]):
                    walk(sub, f"{where}/{key}/{i}", False)

    walk(schema, "", True)
    for def_name, sub in definitions.items():
        walk(sub, f"/definitions/{def_name}", False)
    for unused in sorted(set(definitions) - refs):
        findings.append(f"{name}: definition '{unused}' is never referenced")
    return findings, refs


# ---- repository check ---------------------------------------------------------------------------------------
def check(root: pathlib.Path) -> tuple[list[str], dict[str, int]]:
    findings: list[str] = []
    counts = {"schemas": 0, "valid": 0, "invalid": 0}
    schema_dir = root / "schemas"
    if not schema_dir.is_dir():
        return [f"{schema_dir}: no schemas directory"], counts
    schemas: dict[str, dict[str, Any]] = {}
    for path in sorted(schema_dir.glob("*.schema.json")):
        rel = path.relative_to(root).as_posix()
        try:
            doc = load_file(path)
        except (JsonError, OSError) as exc:
            findings.append(f"{rel}: {exc}")
            continue
        lint, _ = lint_schema(path.name, doc)
        findings.extend(f"schemas/{f}" for f in lint)
        ident = schema_file_id(path.name)
        if ident and isinstance(doc, dict):
            schemas[ident[0]] = doc
    counts["schemas"] = len(schemas)

    # Shared definitions must not drift apart.
    seen: dict[str, tuple[str, Any]] = {}
    for schema_id, doc in sorted(schemas.items()):
        for def_name, sub in sorted(doc.get("definitions", {}).items()):
            if def_name in seen and not json_equal(seen[def_name][1], sub):
                findings.append(f"schemas/{schema_id}.schema.json: definition '{def_name}' differs from the one "
                                f"in {seen[def_name][0]}.schema.json")
            seen.setdefault(def_name, (schema_id, sub))

    examples_dir = schema_dir / "examples"
    for schema_id, doc in sorted(schemas.items()):
        if not (examples_dir / schema_id).is_dir():
            findings.append(f"schemas/examples/{schema_id}: missing (every schema has valid and invalid examples)")
    if examples_dir.is_dir():
        for path in sorted(examples_dir.rglob("*")):
            if path.is_file() and path.suffix != ".json":
                findings.append(f"{path.relative_to(root).as_posix()}: not a .json file")
        for sub in sorted(p for p in examples_dir.iterdir() if p.is_dir()):
            if sub.name not in schemas:
                findings.append(f"{sub.relative_to(root).as_posix()}: no schema {sub.name}.schema.json")
                continue
            findings.extend(check_examples(root, sub, schemas[sub.name], counts))
    return findings, counts


def check_examples(root: pathlib.Path, folder: pathlib.Path, schema: dict[str, Any],
                   counts: dict[str, int]) -> list[str]:
    findings: list[str] = []
    rel = folder.relative_to(root).as_posix()
    validator = Validator(schema)
    expect: dict[str, Any] = {}
    try:
        expect = load_file(folder / "expect.json")
    except FileNotFoundError:
        findings.append(f"{rel}/expect.json: missing")
    except (JsonError, OSError) as exc:
        findings.append(f"{rel}/expect.json: {exc}")
    if not isinstance(expect, dict):
        findings.append(f"{rel}/expect.json: not an object")
        expect = {}
    for kind in ("valid", "invalid"):
        files = sorted((folder / kind).glob("*.json")) if (folder / kind).is_dir() else []
        if not files:
            findings.append(f"{rel}/{kind}: no examples")
        for path in files:
            prel = path.relative_to(root).as_posix()
            counts[kind] += 1
            try:
                instance = load_file(path)
            except (JsonError, OSError) as exc:
                findings.append(f"{prel}: {exc}")
                continue
            try:
                errors = validator.errors(instance)
            except (KeyError, re.error) as exc:
                findings.append(f"{prel}: validator failure: {exc}")
                continue
            if kind == "valid":
                for ptr, message in errors:
                    findings.append(f"{prel}: invalid at '{ptr}': {message}")
                continue
            entry = expect.get(path.name)
            if not isinstance(entry, dict) or not isinstance(entry.get("pointer"), str) \
                    or not isinstance(entry.get("why"), str) or not entry["why"].strip():
                findings.append(f"{prel}: no expect.json entry {{pointer, why}}")
                continue
            if not errors:
                findings.append(f"{prel}: validates, but it is an invalid example ({entry['why']})")
            elif not any(ptr == entry["pointer"] for ptr, _ in errors):
                got = ", ".join(sorted({f"'{p}'" for p, _ in errors}))
                findings.append(f"{prel}: fails at {got}, not at '{entry['pointer']}'")
    invalid_names = {p.name for p in (folder / "invalid").glob("*.json")} if (folder / "invalid").is_dir() else set()
    for stale in sorted(set(expect) - invalid_names):
        findings.append(f"{rel}/expect.json: entry '{stale}' has no invalid example")
    return findings


# ---- self-test ----------------------------------------------------------------------------------------------
def self_test() -> int:
    failures: list[str] = []

    def expect_errors(schema: dict[str, Any], instance: Any, pointers: set[str], label: str) -> None:
        got = {p for p, _ in Validator(schema).errors(instance)}
        if got != pointers:
            failures.append(f"{label}: errors at {sorted(got)}, expected {sorted(pointers)}")

    s: dict[str, Any] = {
        "type": "object", "required": ["a"], "additionalProperties": False,
        "properties": {
            "a": {"type": "integer", "minimum": 0},
            "b": {"$ref": "#/definitions/u"},
            "c": {"oneOf": [{"type": "string"}, {"type": "integer"}]},
            "d": {"type": "array", "items": [{"const": "first"}], "additionalItems": {"type": "integer"},
                  "uniqueItems": True},
            "e": {"type": ["object", "null"], "required": ["x"]},
            "f": {"allOf": [{"type": "string"}, {"pattern": "^z"}]},
            "g": {"type": "object", "propertyNames": {"minLength": 1}, "additionalProperties": {"type": "boolean"}},
            "h": {"type": "number", "not": {"const": 0}},
            "k": {"type": "object", "if": {"required": ["t"], "properties": {"t": {"const": 1}}},
                  "then": {"required": ["v"]}, "else": {"properties": {"v": {"type": "string"}}}},
        },
        "definitions": {"u": {"type": "string", "pattern": "^[0-9a-f]{2}$"}},
    }
    expect_errors(s, {"a": 1}, set(), "minimal valid instance")
    expect_errors(s, {}, {""}, "required reported at the parent")
    expect_errors(s, {"a": 1, "zz": 1}, {""}, "additionalProperties reported at the parent")
    expect_errors(s, {"a": 1.0}, {"/a"}, "1.0 is not an integer (json-schema-validator rule)")
    expect_errors(s, {"a": True}, {"/a"}, "a boolean is not an integer")
    expect_errors(s, {"a": -1}, {"/a"}, "minimum")
    expect_errors(s, {"a": 1, "b": "AB"}, {"/b"}, "$ref + pattern")
    expect_errors(s, {"a": 1, "c": 1.5}, {"/c"}, "oneOf reported at the parent")
    expect_errors(s, {"a": 1, "d": ["first", 2, 2]}, {"/d"}, "uniqueItems")
    expect_errors(s, {"a": 1, "d": ["second", 2]}, {"/d/0"}, "tuple items")
    expect_errors(s, {"a": 1, "d": ["first", "x"]}, {"/d/1"}, "additionalItems")
    expect_errors(s, {"a": 1, "e": None}, set(), "type list with null")
    expect_errors(s, {"a": 1, "e": {}}, {"/e"}, "object keywords apply to objects only")
    expect_errors(s, {"a": 1, "f": "a"}, {"/f"}, "allOf passes nested errors through")
    expect_errors(s, {"a": 1, "g": {"": True}}, {"/g"}, "propertyNames reported at the parent")
    expect_errors(s, {"a": 1, "g": {"x": 1}}, {"/g"}, "additionalProperties schema reported at the parent")
    expect_errors(s, {"a": 1, "h": 0.0}, {"/h"}, "not + const: 0.0 equals 0")
    expect_errors(s, {"a": 1, "k": {"t": 1}}, {"/k"}, "if/then")
    expect_errors(s, {"a": 1, "k": {"t": 2, "v": 3}}, {"/k/v"}, "if/else")
    expect_errors({"type": "array", "uniqueItems": True}, [1, True, "1"], set(), "uniqueItems: true is not 1")
    expect_errors({"type": "array", "uniqueItems": True}, [{"x": 1}, {"x": 1.0}], {""}, "uniqueItems: 1 equals 1.0")
    expect_errors({"type": "object", "patternProperties": {"^.+$": {"type": "integer"}}}, {"k": "v"}, {"/k"},
                  "patternProperties reported at the member")
    if pointer_join("/a", "x/y~z") != "/a/x~1y~0z":
        failures.append("pointer escaping")

    for text, label in (('{"a": 1, "a": 2}', "duplicate keys"), ('{"a": NaN}', "NaN")):
        try:
            loads_strict(text)
            failures.append(f"strict JSON accepted {label}")
        except JsonError:
            pass

    good = {
        "$schema": DRAFT07, "$id": "urn:esses:thing-1", "title": "t", "description": "d", "type": "object",
        "required": ["$schema", "schemaVersion"],
        "properties": {
            "$schema": {"type": "string",
                        "pattern": "^(urn:esses:thing-1|esses:thing-1|"
                                   "https://[^/\\s]+(/[^/\\s]+)*/thing-1(\\.schema)?\\.json)$"},
            "schemaVersion": {"type": "integer", "minimum": 1},
            "x": {"$ref": "#/definitions/x"},
        },
        "definitions": {"x": {"type": "string"}},
    }
    lint, _ = lint_schema("thing-1.schema.json", good)
    if lint:
        failures.append(f"lint of a good schema: {lint}")
    cases: list[tuple[str, Callable[[dict[str, Any]], None], str]] = [
        ("wrong id", lambda d: d.update({"$id": "urn:esses:thing-2"}), "$id is not"),
        ("misspelt keyword", lambda d: d["properties"]["x"].update(requird=["y"]), "misspelt keyword 'requird'"),
        ("format", lambda d: d["properties"]["x"].update(format="date-time"), "keyword 'format'"),
        ("dangling ref", lambda d: d["properties"]["x"].update({"$ref": "#/definitions/y"}), "unresolvable $ref"),
        ("unused definition", lambda d: d["definitions"].update(y={}), "'y' is never referenced"),
        ("closed required", lambda d: d.update(additionalProperties=False, required=["$schema", "schemaVersion", "q"]),
         "required 'q' can never be present"),
        ("lookahead", lambda d: d["definitions"]["x"].update(pattern="^(?=a)"), "inline groups"),
        ("loose id pattern", lambda d: d["properties"]["$schema"].update(pattern="thing"), "pattern accepts"),
        ("ref sibling", lambda d: d["properties"]["x"].update(minLength=1), "beside $ref"),
        ("uint64 bound", lambda d: d["definitions"]["x"].update(maximum=2**64 - 1), "outside int64"),
    ]
    for label, mutate, needle in cases:
        bad = json.loads(json.dumps(good))
        mutate(bad)
        lint, _ = lint_schema("thing-1.schema.json", bad)
        if not any(needle in f for f in lint):
            failures.append(f"lint missed {label}: {lint}")
    lint, _ = lint_schema("Thing.schema.json", good)
    if not lint:
        failures.append("lint accepted a bad file name")

    # Built-in settings fragments: no envelope, but every other rule.
    fragment = {
        "$schema": DRAFT07, "$id": "urn:esses:builtin-acme.speed_fix-2", "title": "t", "description": "d",
        "type": "object", "additionalProperties": False, "properties": {"x": {"type": "number"}},
    }
    lint, _ = lint_schema("builtin-acme.speed_fix-2.schema.json", fragment)
    if lint:
        failures.append(f"lint of a good built-in settings schema: {lint}")
    bad_fragment = dict(fragment, **{"$id": "urn:esses:builtin-acme.speed_fix-1"})
    lint, _ = lint_schema("builtin-acme.speed_fix-2.schema.json", bad_fragment)
    if not any("$id is not" in f for f in lint):
        failures.append(f"lint missed a built-in settings schema with the wrong $id: {lint}")
    for bad_name in ("builtin-Acme.speed-2.schema.json", "builtin-.x-1.schema.json", "builtin-acme..x-1.schema.json"):
        if schema_file_id(bad_name) is not None and schema_file_id(bad_name)[2]:
            failures.append(f"accepted the built-in schema name {bad_name}")
    lint, _ = lint_schema("thing.x-1.schema.json", good)
    if not lint:
        failures.append("lint accepted a '.' outside a built-in schema name")

    for f in failures:
        print(f"self-test: {f}")
    print("check_schemas self-test: " + ("passed" if not failures else "FAILED"))
    return 0 if not failures else 1


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=here.parent, help="repository root")
    parser.add_argument("--self-test", action="store_true", help="test the checker on built-in cases")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    findings, counts = check(args.root.resolve())
    for f in findings:
        print(f)
    print(f"check_schemas: {counts['schemas']} schema(s), {counts['valid']} valid and {counts['invalid']} invalid "
          f"example(s), {len(findings)} finding(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
