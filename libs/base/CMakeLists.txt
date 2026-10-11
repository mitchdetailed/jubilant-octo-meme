# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_base (layer 0, Qt-free): primitives every other library builds on (01 section 2.1, 02 section 1).

configure_file(src/build_config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/base_build_config.hpp" @ONLY)

esses_add_library(base LAYER 0 QT_FREE
  SOURCES
    src/assert.cpp
    src/atomic_file.cpp
    src/byte_reader.cpp
    src/casefold.cpp
    src/env.cpp
    src/executor.cpp
    src/hash.cpp
    src/int128.cpp
    src/json_depth.cpp
    src/log.cpp
    src/mapped_file.cpp
    src/paths.cpp
    src/priority.cpp
    src/rate.cpp
    src/result.cpp
    src/taskpool.cpp
    src/thread_role.cpp
    src/time.cpp
    src/utf8.cpp
    src/uuid.cpp
    src/version.cpp
  PRIVATE_DEPS
    esses::xxhash)

target_include_directories(${ESSES_PREFIX}_base PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
find_package(Threads REQUIRED)
target_link_libraries(${ESSES_PREFIX}_base PUBLIC Threads::Threads)
if(WIN32)
  # Known-folder lookup for Paths (01 section 9).
  target_link_libraries(${ESSES_PREFIX}_base PRIVATE shell32 ole32 uuid)
endif()

esses_add_test(base_tests QT_FREE
  SOURCES
    tests/atomic_file_tests.cpp
    tests/byte_reader_tests.cpp
    tests/casefold_tests.cpp
    tests/hash_tests.cpp
    tests/int128_tests.cpp
    tests/json_depth_tests.cpp
    tests/log_tests.cpp
    tests/mapped_file_tests.cpp
    tests/paths_tests.cpp
    tests/rate_tests.cpp
    tests/result_tests.cpp
    tests/taskpool_tests.cpp
    tests/utf8_tests.cpp
    tests/uuid_tests.cpp
    tests/version_tests.cpp
  DEPS
    esses::base)
