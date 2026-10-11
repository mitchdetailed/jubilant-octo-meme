# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_units (layer 0, Qt-free): Dimension, Quantity, Unit, Converter and the UnitRegistry loaded from
# data/units.json (schema units-1) plus the per-user units.user.json (01 section 2.1, 02 section 2; D-010, D-085,
# D-141). Links esses_base; nlohmann-json is a private dependency of the loader.

esses_add_library(units LAYER 0 QT_FREE
  SOURCES
    src/converter.cpp
    src/dimension.cpp
    src/registry_loader.cpp
    src/spelling.cpp
    src/unit_registry.cpp
  DEPS
    esses::base
  PRIVATE_DEPS
    esses::nlohmann_json)

# AC-M0-09 and the unit tests. The shipped-table cases read data/units.json and validate it against
# schemas/units-1.schema.json; the [oracle] case checks the unit spellings of the private S-1 corpus
# (fixtures/private/s1) and skips when that directory is absent (02 section 8 item 2, U-13 style).
esses_add_test(units_tests QT_FREE
  SOURCES
    tests/converter_tests.cpp
    tests/dimension_tests.cpp
    tests/private_corpus_tests.cpp
    tests/registry_load_tests.cpp
    tests/registry_lookup_tests.cpp
    tests/shipped_table_tests.cpp
  DEPS
    esses::units
    esses::nlohmann_json
    esses::json_schema_validator)
