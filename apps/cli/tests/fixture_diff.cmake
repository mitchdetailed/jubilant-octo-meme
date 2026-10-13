# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# ctest cli.fixture_diff.explains (05 section 9.2 "fixture diff"): a copy of a committed CSV fixture whose
# expectations are altered on purpose (one sample of one channel, one probe member) differs; `fixture diff`
# exits 1 and names the channel, the first differing sample with the reader's and the oracle's values, the
# ulp distance, and the JSON Pointer of the probe difference; --channel limits the report to one channel; the
# untouched fixture is SAME (exit 0).
#   cmake -DCLI=<esses-cli> -DFIXTURE=<fixture dir> -DWORK=<dir> -P fixture_diff.cmake
foreach(_v CLI FIXTURE WORK)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "fixture_diff.cmake: -D${_v}=... is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(COPY "${FIXTURE}" DESTINATION "${WORK}")
get_filename_component(_name "${FIXTURE}" NAME)
set(_copy "${WORK}/${_name}")

execute_process(COMMAND "${CLI}" fixture diff "${_copy}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "SAME")
  message(FATAL_ERROR "the unaltered copy should be SAME (exit ${_rc}):\n${_out}\n${_err}")
endif()

# Sample 2 of Speed: 90.38 -> 90.5; the probe's importerVersion member.
set(_csv "${_copy}/expected.channels/000_Speed.csv")
file(READ "${_csv}" _text)
string(REPLACE "2,20000,90.38,1" "2,20000,90.5,1" _text "${_text}")
file(WRITE "${_csv}" "${_text}")
set(_probe "${_copy}/expected.probe.json")
file(READ "${_probe}" _text)
string(REPLACE "\"importerVersion\": \"1.0.0\"" "\"importerVersion\": \"0.9.0\"" _text "${_text}")
file(WRITE "${_probe}" "${_text}")

execute_process(COMMAND "${CLI}" fixture diff "${_copy}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
message("${_out}")
if(NOT _rc EQUAL 1)
  message(FATAL_ERROR "fixture diff should exit 1 on a difference, got ${_rc}: ${_err}")
endif()
foreach(_want "DIFF" "channel 'Speed' \\(oracle 0 'Speed'\\): 4000 samples, 1 differ, max [0-9]+ F32 ulp; first: sample 2: 90.3"
        "oracle 90.5" "replace at '/importerVersion': expected \"0.9.0\", got \"1.0.0\"")
  if(NOT _out MATCHES "${_want}")
    message(FATAL_ERROR "fixture diff output lacks '${_want}'")
  endif()
endforeach()

execute_process(COMMAND "${CLI}" fixture diff "${_copy}" --channel "gps lat" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out)
if(_out MATCHES "channel 'Speed'" OR NOT _out MATCHES "channel 'GPS Lat'")
  message(FATAL_ERROR "--channel should limit the report to GPS Lat:\n${_out}")
endif()
