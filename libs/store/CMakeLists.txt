# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses_store (layer 1, Qt-free): chunked column store, pyramids, cache, series and resampler
# (01 section 2.1, 02 section 3, D-009, D-011, D-012). Links esses_base only (outline section 1.2, D-048).

esses_add_library(store LAYER 1 QT_FREE
  SOURCES
    src/chunk.cpp
    src/chunk_cache.cpp
    src/column.cpp
    src/memory_provider.cpp
    src/pyramid.cpp
    src/range_query.cpp
    src/resampler.cpp
    src/series.cpp
    src/system_memory.cpp
    src/types.cpp
  DEPS
    esses::base)

esses_add_test(store_tests QT_FREE
  SOURCES
    tests/cache_tests.cpp
    tests/chunk_tests.cpp
    tests/column_tests.cpp
    tests/pyramid_property_tests.cpp
    tests/resampler_tests.cpp
  DEPS
    esses::store)
