# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# esses-render (esses_render, layer 4, Qt Gui offscreen; 01 section 2.1, 04 section 6.7 item 5; AC-M1-05):
#   esses-render <workbook.json> <log...> --worksheet N -o out.png [--dpr 2] [--component K] [--selection fastest]
#                [--size WxH] [--scheme light|dark|<file>] [--font-dir DIR] [--timeout S]
# Opens the logs through esses_qtmodel's SourceLoader/MathsService, prepares the build inputs with RenderService,
# renders with esses_qtrender's OffscreenRenderer and writes a PNG. Built only with ESSES_BUILD_QT=ON. Sources
# are globbed (CONFIGURE_DEPENDS).

file(GLOB_RECURSE _esses_render_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")
esses_add_executable(render LAYER 4
  SOURCES ${_esses_render_sources}
  DEPS
    esses::base
    esses::units
    esses::model
    esses::analysis
    esses::maths
    esses::project
    esses::plotcore
    esses::qtmodel
    esses::qtrender
    Qt6::Gui
  OUTPUT_NAME esses-render)

if(ESSES_BUILD_TESTS)
  add_test(NAME render.help COMMAND $<TARGET_FILE:${ESSES_PREFIX}_render> --help)
  # Argument validation (exit code 2, checked through the message): these stop before any file is read.
  add_test(NAME render.usage_missing_output
           COMMAND $<TARGET_FILE:${ESSES_PREFIX}_render> workbook.json log.ld)
  set_tests_properties(render.usage_missing_output PROPERTIES
                       PASS_REGULAR_EXPRESSION "a workbook, at least one log and -o <file> are required")
  add_test(NAME render.usage_bad_dpr
           COMMAND $<TARGET_FILE:${ESSES_PREFIX}_render> workbook.json log.ld -o out.png --dpr 0)
  set_tests_properties(render.usage_bad_dpr PROPERTIES PASS_REGULAR_EXPRESSION "invalid --dpr 0")
  add_test(NAME render.usage_bad_size
           COMMAND $<TARGET_FILE:${ESSES_PREFIX}_render> workbook.json log.ld -o out.png --size 12)
  set_tests_properties(render.usage_bad_size PROPERTIES PASS_REGULAR_EXPRESSION "invalid --size 12")
  add_test(NAME render.missing_workbook
           COMMAND $<TARGET_FILE:${ESSES_PREFIX}_render> no-such.workbook.json log.ld -o out.png)
  set_tests_properties(render.missing_workbook PROPERTIES
                       PASS_REGULAR_EXPRESSION "esses-render: no-such\.workbook\.json: ")
  set_tests_properties(render.help render.usage_missing_output render.usage_bad_dpr render.usage_bad_size
                       render.missing_workbook PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                       ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
                       WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                       LABELS "qt" TIMEOUT 60)
endif()
