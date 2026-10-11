# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
#
# Prefix table (D-003, 08 section 3.3). The codename appears as an identifier only here and in documentation,
# so a later rename is a scripted search-and-replace driven by this table (tools/rename.py, planned).
# The name may change before the first public release (Q-02: no trademark search scheduled).
include_guard(GLOBAL)

set(ESSES_PREFIX      "esses")          # CMake target prefix: ${ESSES_PREFIX}_base
set(ESSES_NAMESPACE   "esses")          # C++ namespace and alias prefix esses::base
set(ESSES_CAPI_PREFIX "esses_")         # C ABI symbol prefix (v1.0)
set(ESSES_APP_NAME    "Esses")          # window title, installer, bundle name
set(ESSES_EXE_NAME    "esses")          # esses, esses-cli, esses-render
set(ESSES_ORG_DOMAIN  "esses.invalid")  # replaced when the domain exists (schema ids, settings)
set(ESSES_DIR_NAME    "esses")          # per-user directory name: %APPDATA%\esses, ~/.config/esses (01 section 9)
set(ESSES_ENV_PREFIX  "ESSES")          # environment overrides: ESSES_CONFIG_DIR, ESSES_LOG, ... (01 sections 8-9)
