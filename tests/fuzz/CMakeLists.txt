# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# libFuzzer targets (08 section 6.8, 05 section 9.3; AC-M0-06), built with ESSES_BUILD_FUZZ=ON:
#   Clang  preset linux-clang-fuzz: -fsanitize=fuzzer on the targets, -fsanitize=fuzzer-no-link on everything
#          else (cmake/fuzzing.cmake), ASan + UBSan (ESSES_SANITIZE).
#   MSVC   preset win-msvc-fuzz (VS 2022, x64): /fsanitize=fuzzer on the targets, the /fsanitize-coverage
#          instrumentation on everything else, ASan.
# Seeds: tools/fuzz_seeds.py builds them from the synthetic fixture inputs (and the user's files when present);
# the .ldx target has a dictionary of the format's tags and attributes.

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
