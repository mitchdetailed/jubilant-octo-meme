# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Pixel goldens (AC-M1-04, 04 section 6.7, 08 section 6.4, D-127): tests/golden/images/tst_*.cpp render the
# eight M1 worksheet cases with OffscreenRenderer at DPR 1 and 2 and compare with the committed PNGs
# (tests/golden/<type>/<case>@<dpr>x.png; a pixel differs when a channel differs by > 8/255; fail above 0.5 %).
# Authoritative on ubuntu only; Windows and macOS run the DrawList JSON goldens (esses_plotcore tests) and
# render smoke tests. Fonts: tests/golden/fonts/ only. Added with ESSES_BUILD_QT and ESSES_BUILD_TESTS.

esses_add_qt_test_dir(golden DIR "${CMAKE_CURRENT_SOURCE_DIR}/images" TIMEOUT 600
  DEPS
    esses::qtrender
    esses::qtmodel)
