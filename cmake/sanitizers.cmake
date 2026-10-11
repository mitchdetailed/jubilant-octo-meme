# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# ESSES_SANITIZE (08 sections 3.2 and 6.10): "address,undefined" or "thread" on Clang/GCC.
# "thread" also defines ESSES_TSAN so the TaskPool tests use smaller workloads.
include_guard(GLOBAL)

if(ESSES_SANITIZE)
  if(MSVC)
    if(ESSES_SANITIZE MATCHES "address")
      add_compile_options(/fsanitize=address)
    else()
      message(WARNING "ESSES_SANITIZE=${ESSES_SANITIZE}: only 'address' is available with MSVC; ignored")
    endif()
  else()
    add_compile_options(-fsanitize=${ESSES_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${ESSES_SANITIZE})
    if(ESSES_SANITIZE MATCHES "undefined")
      add_compile_options(-fno-sanitize-recover=undefined)
    endif()
  endif()
  if(ESSES_SANITIZE MATCHES "thread")
    add_compile_definitions(ESSES_TSAN=1)
  endif()
  message(STATUS "Sanitizers: ${ESSES_SANITIZE}")
endif()
