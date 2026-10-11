# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Warning policy (08 section 3.8). /WX and -Werror only when ESSES_WARNINGS_AS_ERRORS is ON, which the ci-*
# presets set; local builds warn without failing (D-034). Third-party targets never get these flags.
include_guard(GLOBAL)

function(esses_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:/W4;/w14242;/w14254;/w14263;/w14265;/w14287;/we4289;/w14296;/w14311;/w14545;/w14546;/w14547;/w14549;/w14555;/w14619;/w14640;/w14826;/w14905;/w14906;/w14928;/wd4251>"
      "$<$<COMPILE_LANGUAGE:C>:/W4>")
    if(ESSES_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wcast-align -Wunused -Wnull-dereference -Wdouble-promotion -Wformat=2
      -Wimplicit-fallthrough
      "$<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor;-Wold-style-cast;-Woverloaded-virtual>")
    if(ESSES_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Not our code: no warnings, never /WX (08 section 3.7.2).
function(esses_silence_third_party target)
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W0)
  else()
    target_compile_options(${target} PRIVATE -w)
  endif()
  set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR OFF FOLDER "third_party")
endfunction()
