# Packaging

The release packages of Esses (M4, D-212; 07 section 7.1 "Packaging" and "Release", 08 sections 3.11 and 7;
AC-M4-04, AC-M4-06, AC-M4-08, AC-M4-09). CI makes them (`.github/workflows/package.yml`, on tags `v*`, on manual
dispatch and on pull requests that touch this folder or `apps/app`); the same scripts run on a developer machine.

| Package | Made on | Contents | Status at 0.1 |
|---|---|---|---|
| `esses-<version>-win64-setup.exe` | windows-2022 | Inno Setup installer: `esses`, `esses-cli`, `esses-render`, Qt (windeployqt), the docking library, data, notices; the `.ld` association task; the C++ runtime check | release-blocking |
| `esses-<version>-win64-portable.zip` | windows-2022 | the same tree + `portable.marker` + the C++ runtime DLLs + `associate-ld.cmd`/`unassociate-ld.cmd` | release-blocking |
| `esses-cli-<version>-win64.zip` | windows-2022 | `esses-cli` + data + notices + the C++ runtime DLLs | release-blocking (D-034) |
| `esses-<version>-x86_64.AppImage` | ubuntu-24.04 | linuxdeploy AppDir with Qt, `esses.desktop`, the `.ld` MIME type, the icon | builds green, not supported (D-116) |
| `esses-cli-<version>-linux-x86_64.tar.gz` | ubuntu-24.04 | `esses-cli` + data + notices | release-blocking (D-034) |
| `esses-<version>-macos.dmg` | macos-14 | `Esses.app` (universal) by macdeployqt, ad hoc signed, `.ld` document type | builds green, not supported (D-116) |
| `esses-cli-<version>-macos.tar.gz` | macos-14 | `esses-cli` + data + notices | release-blocking (D-034) |

Each package also gets `esses[-cli]-<version>-<platform>.cdx.json` (CycloneDX 1.5 SBOM) and the platform's
`sha256sums-<platform>.txt`; the source job adds `esses-<version>-source.spdx` (`reuse spdx`), and the tag's draft
release one `sha256sums.txt` over everything.

## Files

| Path | What |
|---|---|
| `CMakeLists.txt` | every install rule of the tree (components `app` and `cli`, the per-OS layout of 08 section 3.11 where `esses::Paths` finds the data), `ESSES_RELEASE_TAG` (a release build: must be `v` + the stamped version; gives the About dialog the tag's source URL), `<build>/packaging/package_info.json` for `package.py`, the `packaging.*` ctest entries |
| `package.py` | the driver: install, deploy Qt, notices and SBOM, vendor-mark check of the tree, archives, checksums; `--dry-run` prints the commands |
| `gen_third_party.py`, `third_party.json` | `THIRD_PARTY_LICENSES.md` and the CycloneDX SBOM of a package from the component list, the licence files and the files found in the package; `--check` keeps the list, `NOTICES.md` and `third_party/` in step |
| `smoke.py` | the smoke tests of each package (CI; usable on a VM that has Python) |
| `windows/esses.iss` | the Inno Setup script (AppId fixed; the version from the build; `.ld` association task; the C++ runtime check of 08 section 7.1) |
| `windows/associate-ld.cmd`, `unassociate-ld.cmd` | the portable zip's per-user `.ld` association (06 section 19 item 5) |
| `linux/esses.desktop`, `linux/esses-mime.xml` | the desktop entry and the `application/x-esses-log` type (`*.ld`, low weight) |
| `macos/Info.plist.in` | the bundle's Info.plist (apps/app sets it as `MACOSX_BUNDLE_INFO_PLIST`) |
| `icons/` | the application icon, drawn by `make_icons.py` from geometry (PNG 16-512, `.ico`, `.icns`, `.svg`); `--check` compares the committed files with a fresh render |
| `licenses/LICENSE.LGPL-3.0-only.txt`, `LICENSE.GPL-3.0-only.txt` | Qt's licence texts (the SPDX texts as Qt 6.7.2's `qtbase/LICENSES/` holds them, line ends LF), shipped as `licenses/LGPL-3.0-only.txt` and `licenses/GPL-3.0-only.txt`; `LICENSE.*` names keep `reuse lint` from counting them as unused project licences |

## Making the packages locally

```
tools\dev\build-qt.cmd win-msvc-qt-rel build\release        (or any Release configure with ESSES_BUILD_QT=ON)
python -I tools\packaging\package.py --build build\release --out build\dist           (Inno Setup 6 for the installer)
python -I tools\packaging\package.py --build build\release --out build\dist --no-installer --dry-run
python -I tools\packaging\smoke.py windows-portable --zip build\dist\esses-<v>-win64-portable.zip --version <v> --no-registry
```

`--no-registry` leaves out the association scripts' check, which writes to the user's registry (CI runs it). The
installer's smoke test installs and uninstalls for real: run it on a VM or a CI runner, not on a working machine.
ctest runs the tools' self-tests, the icon check and the manifest's consistency (label `packaging`).

`package.py` needs CMake on `PATH`, the build's Qt kit (`windeployqt`, `macdeployqt`; linuxdeploy with
linuxdeploy-plugin-qt on Linux), on Windows a developer environment (`VCToolsRedistDir`, for the runtime the
installer and the zips carry) and Inno Setup 6.3 or later; on macOS `hdiutil` and `codesign`. The archives are
deterministic (sorted entries, the time of `SOURCE_DATE_EPOCH`, else 2026-01-01; no owner).

## Behaviour of the packages

- **Layout.** NOTICES.md, THIRD_PARTY_LICENSES.md, LICENSE and `licenses/` sit beside the `data` folder: the
  installation folder on Windows, `usr/share/esses/` in the AppImage, `Esses.app/Contents/Resources/` on macOS
  (the About dialog links the installed NOTICES.md). The command-line archives are flat: `esses-cli` with `data/`
  beside it.
- **Portable zip.** `portable.marker` beside `esses.exe` makes `esses::Paths` keep the settings, the session
  layout, the cache and the logs in `config/`, `cache/` and `logs/` beside it (D-055); a portable copy is an
  instance of its own (its settings folder is part of the single-instance channel's name).
- **C++ runtime (Windows).** Programs of the VS 2022 17.10 (14.40) or later toolsets crash with an older
  `msvcp140.dll`, and an old redistributable counts as installed (08 section 7.1). The installer reads the
  installed runtime's version; an administrative install runs the bundled `vc_redist.x64.exe` when it is older
  than the build's and refuses ("cannot proceed", exit code 7 silent) when that does not help; a per-user install
  puts the runtime's DLLs beside `esses.exe`. The zips always carry them.
- **Associations.** Windows: the installer's task `assocld` (preselected unless another program owns `.ld`; silent:
  `/MERGETASKS="assocld"`) and the portable zip's `associate-ld.cmd`; `.csv` is only offered in "Open with".
  Linux: `esses.desktop` with `MimeType=application/x-esses-log;` (needs the desktop's integration of the
  AppImage). macOS: `CFBundleDocumentTypes` for `ld`, `LSHandlerRank Alternate`.
- **Vendor marks.** `package.py` runs `tools/check_vendor_marks.py --tree` on every staged tree (file names, text
  files, the strings of our programs) before it is archived (AC-M4-08).

## What only CI or the maintainer checks

The installer's silent install, uninstall and runtime check, the AppImage and the DMG are exercised by
`package.yml` on the hosted runners; the clean Windows 10 and 11 VMs, the VM with a runtime older than 14.40, the
Ubuntu VM and the release steps are the maintainer's (`docs/milestones/M4.md` sections 6.1 and 6.5).
