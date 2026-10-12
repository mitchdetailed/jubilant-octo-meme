# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# ctest fixtures.laps_golden (07_ROADMAP.md section 4.2, AC-M1-07): for every public fixture with beacons (a BCN
# in expected.markers.json), `esses-cli laps --json --no-sidecar` on the generated input equals the committed
# expected.laps.json byte for byte: beacon-pair laps with out/in/partial classes, RESET, IGRDBCN and Start Lap
# numbering, and the phase-2 stint and pitStop annotations. The Outings:Laps groups of those goldens are written
# from hand-checked lap tables by esses-gen-fixtures, not by the code under test.
#   cmake -DCLI=<esses-cli> -DFIXTURES=<fixtures/motec.ld> -DINPUTS=<dir of generated families> -P laps_golden.cmake
foreach(_var CLI FIXTURES INPUTS)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "laps_golden.cmake: -D${_var}=... is required")
  endif()
endforeach()

file(GLOB _dirs LIST_DIRECTORIES true "${FIXTURES}/synthetic-*")
set(_checked 0)
set(_failed "")
foreach(_dir IN LISTS _dirs)
  if(NOT IS_DIRECTORY "${_dir}" OR NOT EXISTS "${_dir}/expected.markers.json")
    continue()
  endif()
  file(READ "${_dir}/expected.markers.json" _markers)
  if(NOT _markers MATCHES "\"class\": \"BCN\"")
    continue()
  endif()
  get_filename_component(_name "${_dir}" NAME)
  set(_input "${INPUTS}/${_name}/input.ld")
  if(NOT EXISTS "${_input}")
    message(FATAL_ERROR "${_input} missing: run fixtures.generate_inputs first")
  endif()
  execute_process(COMMAND "${CLI}" laps "${_input}" --json --no-sidecar
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  file(READ "${_dir}/expected.laps.json" _expected)
  math(EXPR _checked "${_checked} + 1")
  if(NOT _rc EQUAL 0)
    message(SEND_ERROR "${_name}: esses-cli laps failed (${_rc}): ${_err}")
    list(APPEND _failed "${_name}")
  elseif(NOT _out STREQUAL _expected)
    message(SEND_ERROR "${_name}: esses-cli laps --json differs from expected.laps.json")
    list(APPEND _failed "${_name}")
  else()
    message(STATUS "ok: ${_name}")
  endif()
endforeach()
if(_checked EQUAL 0)
  message(FATAL_ERROR "no fixture with beacons under ${FIXTURES}")
endif()
list(LENGTH _failed _nfailed)
math(EXPR _passed "${_checked} - ${_nfailed}")
message(STATUS "laps golden: ${_passed} passed, ${_nfailed} failed")
if(_nfailed GREATER 0)
  message(FATAL_ERROR "laps golden failed: ${_failed}")
endif()
