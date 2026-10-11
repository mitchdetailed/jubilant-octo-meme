# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# ctest cli.demo_script: the M0 demo script of 07_ROADMAP.md section 3.6 on synthetic data (the steps that name
# the user's real files run by hand, docs/milestones/M0.md):
#   info --json on a synthetic family; channels and laps; fixture add with the oracle into a scratch directory
#   and fixture verify of it (step 4, the round trip of D-041); validate of a sidecar document.
#   cmake -DCLI=<esses-cli> -DPYTHON=<python> -DSOURCE=<repo> -DINPUTS=<dir of generated families> -DWORK=<dir>
#         -P demo_script.cmake
foreach(_var CLI PYTHON SOURCE INPUTS WORK)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "demo_script.cmake: -D${_var}=... is required")
  endif()
endforeach()

function(_step expect_text)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE ";" " " shown "${ARGN}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "step failed (${rc}): ${shown}\n${out}\n${err}")
  endif()
  if(NOT expect_text STREQUAL "" AND NOT out MATCHES "${expect_text}")
    message(FATAL_ERROR "step output lacks '${expect_text}': ${shown}\n${out}")
  endif()
  message(STATUS "ok: ${shown}")
endfunction()

# A step that must fail (exit status not 0) with `expect_text` in its error output.
function(_step_fails expect_text)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE ";" " " shown "${ARGN}")
  if(rc EQUAL 0)
    message(FATAL_ERROR "step should have failed: ${shown}\n${out}\n${err}")
  endif()
  if(NOT err MATCHES "${expect_text}")
    message(FATAL_ERROR "step error lacks '${expect_text}': ${shown}\n${err}")
  endif()
  message(STATUS "ok (refused as expected): ${shown}")
endfunction()

set(_in "${INPUTS}/synthetic-multirate/input.ld")
if(NOT EXISTS "${_in}")
  message(FATAL_ERROR "${_in} missing: run fixtures.generate_inputs first")
endif()
file(REMOVE_RECURSE "${WORK}")

# 2. Probe a synthetic file; the channel table; markers and the whole-outing range.
_step("\"\\$schema\": \"urn:esses:probe-1\"" "${CLI}" info "${_in}" --json)
_step("Rate 1000 Hz" "${CLI}" channels "${_in}")
_step("Outing 1" "${CLI}" laps "${INPUTS}/synthetic-acc-dialect/input.ld")
_step("\"urn:esses:laps-1\"" "${CLI}" laps "${INPUTS}/synthetic-acc-dialect/input.ld" --json --no-sidecar)
# 3. Validate a sidecar document.
file(GLOB _sidecars "${SOURCE}/schemas/examples/sidecar-1/valid/*.json")
list(GET _sidecars 0 _sidecar)
_step("1 valid" "${CLI}" validate "${_sidecar}")
# 4. Round trip: the oracle's expectations for a synthetic file, then verify them.
_step("" "${CLI}" fixture add "${_in}" --format motec.ld --oracle ref-decode --out "${WORK}/fx" --licence CC0-1.0
      --provenance synthetic --python "${PYTHON}" --ref-decode "${SOURCE}/tools/ld_ref_decode.py")
_step("1 passed, 0 failed" "${CLI}" fixture verify "${WORK}/fx")
_step("0 invalid" "${CLI}" validate "${WORK}/fx")
# 4b. An input with gap blocks: the reference decoder cannot see them, so oracle.validity defaults to
#     finite-only and the fixture verifies.
_step("" "${CLI}" fixture add "${INPUTS}/synthetic-gap-blocks/input.ld" --format motec.ld --oracle ref-decode
      --out "${WORK}/gaps" --licence CC0-1.0 --provenance synthetic --python "${PYTHON}"
      --ref-decode "${SOURCE}/tools/ld_ref_decode.py")
file(READ "${WORK}/gaps/fixture.json" _fx)
if(NOT _fx MATCHES "\"validity\": \"finite-only\"")
  message(FATAL_ERROR "fixture add on a gap-block input did not default oracle.validity to finite-only")
endif()
_step("1 passed, 0 failed" "${CLI}" fixture verify "${WORK}/gaps")
# 4c. The 07 section 3.6 command without --licence: refused before anything is written.
_step_fails("--licence" "${CLI}" fixture add "${_in}" --format motec.ld --oracle ref-decode --out "${WORK}/nolic"
            --python "${PYTHON}" --ref-decode "${SOURCE}/tools/ld_ref_decode.py")
if(EXISTS "${WORK}/nolic")
  message(FATAL_ERROR "a refused fixture add left ${WORK}/nolic behind")
endif()
file(REMOVE_RECURSE "${WORK}")
