# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# libFuzzer targets (08 section 6.8, 05 section 9.3; AC-M0-06), built with ESSES_BUILD_FUZZ=ON:
#   Clang  preset linux-clang-fuzz: -fsanitize=fuzzer on the targets, -fsanitize=fuzzer-no-link on everything
#          else (cmake/fuzzing.cmake), ASan + UBSan (ESSES_SANITIZE).
#   MSVC   preset win-msvc-fuzz (VS 2022, x64): /fsanitize=fuzzer on the targets, the /fsanitize-coverage
#          instrumentation on everything else, ASan.
# Seeds: tools/fuzz_seeds.py builds them from the synthetic fixture inputs (and the user's files when present);
# the .ldx target has a dictionary of the format's tags and attributes; the text targets one of delimiters,
# sentinels and header words.

function(_esses_add_fuzz_target name)
  cmake_parse_arguments(A "" "" "SOURCES;DEPS;INCLUDES" ${ARGN})
  add_executable(${name} ${A_SOURCES})
  target_link_libraries(${name} PRIVATE ${A_DEPS})
  if(A_INCLUDES)
    target_include_directories(${name} PRIVATE ${A_INCLUDES})
  endif()
  esses_apply_warnings(${name})
  if(MSVC)
    target_compile_options(${name} PRIVATE /fsanitize=fuzzer)
  else()
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  endif()
  set_target_properties(${name} PROPERTIES FOLDER "tests/fuzz")
  esses_mark_qt_free(${name})
endfunction()

_esses_add_fuzz_target(fuzz_ld_header SOURCES fuzz_ld_header.cpp DEPS ${ESSES_NAMESPACE}::formats)
# The .ldx parser is internal to esses_formats (src/ld/ldx.hpp), as for the library's own tests.
_esses_add_fuzz_target(fuzz_ldx SOURCES fuzz_ldx.cpp DEPS ${ESSES_NAMESPACE}::formats
                       INCLUDES "${CMAKE_SOURCE_DIR}/libs/formats/src")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/ldx.dict" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fuzz_ldx.dict" COPYONLY)
# M2 text importers (05 section 9.3 text_sniff and text_profile, AC-M2-13): the shipped profiles through the public
# API; seeds are the committed text fixtures (tools/fuzz_seeds.py --text-fixtures), the dictionary their tokens.
_esses_add_fuzz_target(fuzz_csv_sniff SOURCES fuzz_csv_sniff.cpp DEPS ${ESSES_NAMESPACE}::formats)
_esses_add_fuzz_target(fuzz_text_profile SOURCES fuzz_text_profile.cpp DEPS ${ESSES_NAMESPACE}::formats)
foreach(_t fuzz_csv_sniff fuzz_text_profile)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/text.dict" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_t}.dict" COPYONLY)
endforeach()

# The maths language (03 section 14.3): Parser::parse, print round trip, TypeChecker and compile().
_esses_add_fuzz_target(fuzz_maths_parser SOURCES fuzz_maths_parser.cpp DEPS ${ESSES_NAMESPACE}::maths)
target_compile_definitions(fuzz_maths_parser PRIVATE "ESSES_FUZZ_UNITS_JSON=\"${CMAKE_SOURCE_DIR}/data/units.json\"")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/maths.dict" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fuzz_maths_parser.dict" COPYONLY)
