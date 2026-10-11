# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Vendored third-party libraries (08 section 3.7.2), built from third_party/ without their upstream CMake files.
# Provenance and versions: third_party/README.md; licences: NOTICES.md and REUSE.toml.
#
#   esses::pugixml                (MIT, STATIC)
#   esses::xxhash                 (BSD-2-Clause, INTERFACE; a consumer defines XXH_INLINE_ALL in one private TU)
#   esses::nlohmann_json          (MIT, INTERFACE; also spelled nlohmann_json::nlohmann_json)
#   esses::json_schema_validator  (MIT, STATIC, links esses::nlohmann_json)
#   esses::cli11                  (BSD-3-Clause, INTERFACE)
#   esses::fast_float             (MIT OR Apache-2.0 OR BSL-1.0, INTERFACE)
#   esses::catch2                 (BSL-1.0, STATIC, Catch2 v3 amalgamated with its default main; tests only)
#   esses::catch2_nomain          (the same without main(), for a test that supplies its own; built on demand)
#
# Third-party include directories are SYSTEM (MSVC /external:I with /external:W0, -isystem elsewhere), so the
# project's warning flags and /WX never apply to their headers; compiled third-party code is built with
# esses_silence_third_party() (no warnings, never warnings-as-errors).
include_guard(GLOBAL)
include(warnings)

set(ESSES_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH "Vendored third-party sources")
mark_as_advanced(ESSES_THIRD_PARTY_DIR)

function(_esses_require_file path what)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "third_party: ${what} not found at ${path}")
  endif()
endfunction()

# ---- pugixml 1.13 (MIT) --------------------------------------------------------------------------------
_esses_require_file("${ESSES_THIRD_PARTY_DIR}/pugixml/src/pugixml.cpp" "pugixml")
add_library(${ESSES_PREFIX}_tp_pugixml STATIC "${ESSES_THIRD_PARTY_DIR}/pugixml/src/pugixml.cpp")
target_include_directories(${ESSES_PREFIX}_tp_pugixml SYSTEM PUBLIC "${ESSES_THIRD_PARTY_DIR}/pugixml/src")
target_compile_features(${ESSES_PREFIX}_tp_pugixml PUBLIC cxx_std_11)
esses_silence_third_party(${ESSES_PREFIX}_tp_pugixml)
add_library(${ESSES_NAMESPACE}::pugixml ALIAS ${ESSES_PREFIX}_tp_pugixml)

# ---- xxHash 0.8 (BSD-2-Clause), header-only ---------------------------------------------------------------
_esses_require_file("${ESSES_THIRD_PARTY_DIR}/xxhash/xxhash.h" "xxHash")
add_library(${ESSES_PREFIX}_tp_xxhash INTERFACE)
target_include_directories(${ESSES_PREFIX}_tp_xxhash SYSTEM INTERFACE "${ESSES_THIRD_PARTY_DIR}/xxhash")
add_library(${ESSES_NAMESPACE}::xxhash ALIAS ${ESSES_PREFIX}_tp_xxhash)

# ---- nlohmann/json 3.11.3 (MIT), header-only -------------------------------------------------------------
# Upstream defaults (implicit conversions stay on); JsonDoc (01 section 2.1) wraps it.
_esses_require_file("${ESSES_THIRD_PARTY_DIR}/nlohmann_json/include/nlohmann/json.hpp" "nlohmann/json")
add_library(${ESSES_PREFIX}_tp_nlohmann_json INTERFACE)
target_include_directories(${ESSES_PREFIX}_tp_nlohmann_json SYSTEM INTERFACE
                           "${ESSES_THIRD_PARTY_DIR}/nlohmann_json/include")
target_compile_features(${ESSES_PREFIX}_tp_nlohmann_json INTERFACE cxx_std_11)
add_library(${ESSES_NAMESPACE}::nlohmann_json ALIAS ${ESSES_PREFIX}_tp_nlohmann_json)
if(NOT TARGET nlohmann_json::nlohmann_json)
  add_library(nlohmann_json::nlohmann_json ALIAS ${ESSES_PREFIX}_tp_nlohmann_json)   # upstream spelling
endif()

# ---- json-schema-validator 2.3.0 (MIT), compiled (08 section 3.7.2) ----------------------------------------
# A static library from the six upstream sources; the upstream src/CMakeLists.txt is never used.
set(_esses_jsv_dir "${ESSES_THIRD_PARTY_DIR}/json-schema-validator/src")
set(_esses_jsv_sources json-patch.cpp json-schema-draft7.json.cpp json-uri.cpp json-validator.cpp
                       smtp-address-validator.cpp string-format-check.cpp)
list(TRANSFORM _esses_jsv_sources PREPEND "${_esses_jsv_dir}/")
foreach(_esses_src IN LISTS _esses_jsv_sources)
  _esses_require_file("${_esses_src}" "json-schema-validator")
endforeach()
add_library(${ESSES_PREFIX}_tp_json_schema_validator STATIC ${_esses_jsv_sources})
target_include_directories(${ESSES_PREFIX}_tp_json_schema_validator SYSTEM PUBLIC "${_esses_jsv_dir}")
target_link_libraries(${ESSES_PREFIX}_tp_json_schema_validator PUBLIC ${ESSES_NAMESPACE}::nlohmann_json)
target_compile_features(${ESSES_PREFIX}_tp_json_schema_validator PUBLIC cxx_std_11)
esses_silence_third_party(${ESSES_PREFIX}_tp_json_schema_validator)
add_library(${ESSES_NAMESPACE}::json_schema_validator ALIAS ${ESSES_PREFIX}_tp_json_schema_validator)
unset(_esses_jsv_dir)
unset(_esses_jsv_sources)

# ---- CLI11 2.4.2 (BSD-3-Clause), single header ------------------------------------------------------------
_esses_require_file("${ESSES_THIRD_PARTY_DIR}/cli11/include/CLI/CLI11.hpp" "CLI11")
add_library(${ESSES_PREFIX}_tp_cli11 INTERFACE)
target_include_directories(${ESSES_PREFIX}_tp_cli11 SYSTEM INTERFACE "${ESSES_THIRD_PARTY_DIR}/cli11/include")
target_compile_features(${ESSES_PREFIX}_tp_cli11 INTERFACE cxx_std_11)
add_library(${ESSES_NAMESPACE}::cli11 ALIAS ${ESSES_PREFIX}_tp_cli11)

# ---- fast_float 6.1.6 (MIT OR Apache-2.0 OR BSL-1.0), single header --------------------------------------
_esses_require_file("${ESSES_THIRD_PARTY_DIR}/fast_float/include/fast_float/fast_float.h" "fast_float")
add_library(${ESSES_PREFIX}_tp_fast_float INTERFACE)
target_include_directories(${ESSES_PREFIX}_tp_fast_float SYSTEM INTERFACE
                           "${ESSES_THIRD_PARTY_DIR}/fast_float/include")
target_compile_features(${ESSES_PREFIX}_tp_fast_float INTERFACE cxx_std_11)
add_library(${ESSES_NAMESPACE}::fast_float ALIAS ${ESSES_PREFIX}_tp_fast_float)

# ---- Catch2 3.7.1 amalgamated (BSL-1.0); tests only ---------------------------------------------------------
# Tests include the modular v3 headers (<catch2/catch_test_macros.hpp>, <catch2/catch_approx.hpp>,
# <catch2/matchers/...>, <catch2/generators/...>): the forwarders generated below map each of them onto
# catch_amalgamated.hpp, so a later switch to the non-amalgamated Catch2 needs no change in the tests.
if(ESSES_BUILD_TESTS)
  set(_esses_catch2_dir "${ESSES_THIRD_PARTY_DIR}/catch2v3/src")
  _esses_require_file("${_esses_catch2_dir}/catch_amalgamated.hpp" "Catch2 v3")
  _esses_require_file("${_esses_catch2_dir}/catch_amalgamated.cpp" "Catch2 v3")

  set(_esses_catch2_fwd_dir "${CMAKE_BINARY_DIR}/generated/catch2v3/include")
  set(_esses_catch2_public_headers
      catch_all.hpp catch_approx.hpp catch_config.hpp catch_get_random_seed.hpp catch_message.hpp
      catch_section_info.hpp catch_session.hpp catch_template_test_macros.hpp catch_test_case_info.hpp
      catch_test_macros.hpp catch_test_spec.hpp catch_tostring.hpp catch_totals.hpp
      catch_translate_exception.hpp catch_version.hpp catch_version_macros.hpp
      benchmark/catch_benchmark.hpp benchmark/catch_benchmark_all.hpp benchmark/catch_chronometer.hpp
      generators/catch_generator_exception.hpp generators/catch_generators.hpp
      generators/catch_generators_adapters.hpp generators/catch_generators_all.hpp
      generators/catch_generators_random.hpp generators/catch_generators_range.hpp
      interfaces/catch_interfaces_capture.hpp interfaces/catch_interfaces_config.hpp
      interfaces/catch_interfaces_reporter.hpp
      matchers/catch_matchers.hpp matchers/catch_matchers_all.hpp
      matchers/catch_matchers_container_properties.hpp matchers/catch_matchers_contains.hpp
      matchers/catch_matchers_exception.hpp matchers/catch_matchers_floating_point.hpp
      matchers/catch_matchers_predicate.hpp matchers/catch_matchers_quantifiers.hpp
      matchers/catch_matchers_range_equals.hpp matchers/catch_matchers_string.hpp
      matchers/catch_matchers_templated.hpp matchers/catch_matchers_vector.hpp
      reporters/catch_reporter_event_listener.hpp reporters/catch_reporter_registrars.hpp
      reporters/catch_reporter_streaming_base.hpp)
  string(CONCAT _esses_catch2_fwd_text
         "// Generated by cmake/third_party.cmake: Catch2 v3 is vendored as the amalgamated distribution.\n"
         "#pragma once\n"
         "#include <catch_amalgamated.hpp>\n")
  foreach(_esses_hdr IN LISTS _esses_catch2_public_headers)
    # file(CONFIGURE) writes only on a change, so a reconfigure does not rebuild every test.
    file(CONFIGURE OUTPUT "${_esses_catch2_fwd_dir}/catch2/${_esses_hdr}" CONTENT "${_esses_catch2_fwd_text}"
         @ONLY NEWLINE_STYLE UNIX)
  endforeach()

  # CATCH_CONFIG_* must agree between the library and every test TU, so they are PUBLIC. With MSVC the private
  # UNICODE/_UNICODE select Catch2's wmain(), which hands the command line to Catch2 as UTF-8 (main() would get
  # the ANSI code page): a test name outside ASCII then still matches the test spec that ctest passes.
  function(_esses_add_catch2 target)
    cmake_parse_arguments(A "NO_MAIN" "" "" ${ARGN})
    add_library(${target} STATIC "${_esses_catch2_dir}/catch_amalgamated.cpp")
    target_include_directories(${target} SYSTEM PUBLIC "${_esses_catch2_dir}" "${_esses_catch2_fwd_dir}")
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_definitions(${target} PUBLIC CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS)
    if(MSVC)
      target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)
    endif()
    if(A_NO_MAIN)
      target_compile_definitions(${target} PRIVATE CATCH_AMALGAMATED_CUSTOM_MAIN)
      set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL ON)   # compiled only when a test links it
    elseif(MSVC)
      # The linker infers mainCRTStartup unless an object file defines wmain; name the entry point so that it
      # takes wmain from this library.
      target_link_options(${target} INTERFACE /ENTRY:wmainCRTStartup)
    endif()
    esses_silence_third_party(${target})
  endfunction()

  _esses_add_catch2(${ESSES_PREFIX}_tp_catch2)
  add_library(${ESSES_NAMESPACE}::catch2 ALIAS ${ESSES_PREFIX}_tp_catch2)
  _esses_add_catch2(${ESSES_PREFIX}_tp_catch2_nomain NO_MAIN)
  add_library(${ESSES_NAMESPACE}::catch2_nomain ALIAS ${ESSES_PREFIX}_tp_catch2_nomain)
  unset(_esses_catch2_public_headers)
  unset(_esses_catch2_fwd_text)
endif()
