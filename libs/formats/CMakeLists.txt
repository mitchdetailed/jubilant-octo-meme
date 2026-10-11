# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_formats (layer 2, Qt-free): importers, readers and writers (05_IMPORTERS_AND_PLUGINS.md).
#
# M0: the .ld/.ldx binary core in src/ld/ (namespace esses::formats::ld, internal), and on top of it the
# importer layer of 05 sections 1-2 (ByteSource, ProbeResult/probe_json, IImporter/IReader/IWriter,
# ImporterRegistry and the open pipeline into esses_model) with the motec.ld importer and writer in
# src/importer/. src/ld/ keeps including nothing above esses_store (plus pugixml for .ldx); src/importer/ and
# the files directly under src/ bridge it to esses_units and esses_model (08 section 2 table). Floating-point
# text is parsed with fast_float everywhere in this library, never std::from_chars, because Apple Clang 15's
# libc++ (the macos-14 runner) has no floating-point from_chars.

# Writer identity (05 section 5 item 1): Esses' own neutral constants, derived from cmake/naming.cmake so a
# rename touches one place.
set(ESSES_FORMATS_LD_DEVICE_TYPE "${ESSES_APP_NAME}")
configure_file(src/formats_build_config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/formats_build_config.hpp"
               @ONLY)

esses_add_library(formats LAYER 2 QT_FREE
  SOURCES
    src/import_log.cpp
    src/ld/ld_strings.cpp
    src/ld/ld_codes.cpp
    src/ld/ld_file.cpp
    src/ld/ld_decode.cpp
    src/ld/ld_chunk_provider.cpp
    src/ld/ld_writer.cpp
    src/ld/ldx.cpp
    src/byte_source.cpp
    src/probe_json.cpp
    src/importer.cpp
    src/importer_chunk_provider.cpp
    src/registry.cpp
    src/importer/ld_details.cpp
    src/importer/ldx_companion.cpp
    src/importer/ld_importer.cpp
    src/importer/ld_export.cpp
  DEPS
    esses::base
    esses::units
    esses::store
    esses::model
  PRIVATE_DEPS
    esses::pugixml
    esses::nlohmann_json
    esses::fast_float)
target_include_directories(${ESSES_PREFIX}_formats PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Unit, round-trip, hostile-input and provider tests of the .ld/.ldx core; the [oracle] cases compare the
# reader with tools/ld_ref_decode.py's output for the user's files under fixtures/private/s1 and skip when that
# directory is absent (08 section 6.2).
esses_add_test(formats_ld_tests QT_FREE TIMEOUT 900
  SOURCES
    tests/import_log_tests.cpp
    tests/ld_strings_tests.cpp
    tests/ld_roundtrip_tests.cpp
    tests/ld_hostile_tests.cpp
    tests/ld_review_tests.cpp
    tests/ld_chunk_provider_tests.cpp
    tests/ldx_tests.cpp
    tests/ld_private_corpus_tests.cpp
    tests/ld_oracle_fixtures_tests.cpp
  DEPS
    esses::formats
    esses::nlohmann_json)
if(TARGET ${ESSES_PREFIX}_formats_ld_tests)
  # The tests reach the internal headers of src/ld/ directly.
  target_include_directories(${ESSES_PREFIX}_formats_ld_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(${ESSES_PREFIX}_formats_ld_tests PRIVATE
                             "ESSES_TEST_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
endif()

# The importer layer through its public API (plus the core's writer to make synthetic inputs): registry
# sniffing, the open pipeline into esses_model, probe_json goldens validated against schemas/probe-1, the
# motec.ld writer's round trips (.ldx unknown elements byte-identical) and the [oracle] smoke test over the
# user's real files (skipped when fixtures/private is absent).
esses_add_test(formats_tests QT_FREE TIMEOUT 900
  SOURCES
    tests/byte_source_tests.cpp
    tests/probe_json_tests.cpp
    tests/registry_tests.cpp
    tests/ld_importer_tests.cpp
    tests/ld_export_tests.cpp
    tests/private_corpus_api_tests.cpp
  DEPS
    esses::formats
    esses::nlohmann_json
    esses::json_schema_validator)
if(TARGET ${ESSES_PREFIX}_formats_tests)
  target_include_directories(${ESSES_PREFIX}_formats_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(${ESSES_PREFIX}_formats_tests PRIVATE
                             "ESSES_TEST_REPO_ROOT=\"${CMAKE_SOURCE_DIR}\"")
endif()

# The independent oracle (D-041): the synthetic families written by LdWriter are decoded by
# tools/ld_ref_decode.py and must match the generator's input and the reader's output (AC-M0-03); and the
# reader's Import Log codes share the oracle's names wherever the meaning is the same.
find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET ${ESSES_PREFIX}_formats_ld_tests)
  add_test(NAME formats_ld.oracle_synthetic
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/ld_oracle_synthetic.py"
                   --exe "$<TARGET_FILE:${ESSES_PREFIX}_formats_ld_tests>"
                   --oracle "${CMAKE_SOURCE_DIR}/tools/ld_ref_decode.py"
                   --work "${CMAKE_CURRENT_BINARY_DIR}/oracle-synthetic")
  set_tests_properties(formats_ld.oracle_synthetic PROPERTIES LABELS "unit;oracle-synthetic" TIMEOUT 600)
  add_test(NAME lint.check_ld_codes
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_ld_codes.py"
                   --root "${CMAKE_SOURCE_DIR}")
  set_tests_properties(lint.check_ld_codes PROPERTIES LABELS "lint" TIMEOUT 120)
endif()
