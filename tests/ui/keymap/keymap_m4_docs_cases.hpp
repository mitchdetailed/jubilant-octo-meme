// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The KEYMAP_TEST entries of the M4 docs package (wave 2; D-206, docs/milestones/M4.md section 3; 06 section
// 14.4 help.contents F1), included by tst_keymap.cpp inside its anonymous namespace after Fixture, Case and
// Before (as keymap_m3_cases.hpp). Empty until the package implements help.contents: it then flips the row of
// data/keymaps/default.json to "implemented" and adds its entry here (verify: the user documentation's start
// page is handed to the shell's URL opener, which the test replaces).
#pragma once

void addM4DocsCases(std::map<QString, Case>& /*table*/) {}
