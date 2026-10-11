# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# ESSES_BUILD_FUZZ (08 sections 3.2 and 6.8): coverage instrumentation for every target, so the libFuzzer
# targets of tests/fuzz see the edges of the libraries they call. The targets themselves add
# -fsanitize=fuzzer (Clang) or /fsanitize=fuzzer (MSVC, x64, VS 2022), which also links the libFuzzer runtime.
include_guard(GLOBAL)

if(NOT ESSES_BUILD_FUZZ)
  return()
endif()
if(MSVC)
  add_compile_options(/fsanitize-coverage=inline-8bit-counters /fsanitize-coverage=edge
                      /fsanitize-coverage=trace-cmp /fsanitize-coverage=trace-div)
  add_link_options(/INCREMENTAL:NO) # ASan and SanitizerCoverage do not support incremental linking
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-fsanitize=fuzzer-no-link)
else()
  message(FATAL_ERROR "ESSES_BUILD_FUZZ needs Clang (linux-clang-fuzz) or MSVC (win-msvc-fuzz)")
endif()
message(STATUS "Fuzzing instrumentation: on")
