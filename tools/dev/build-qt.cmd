@echo off
rem SPDX-FileCopyrightText: 2026 the Esses contributors
rem SPDX-License-Identifier: MIT
rem
rem Configure, build and test everything, Qt layers included, with MSVC + Ninja from any shell (cmd, PowerShell,
rem Git Bash). The Qt counterpart of build-core.cmd (M1, 07 section 4.1 "qt-app").
rem   build-qt.cmd [preset] [binary-dir] [build-type] [extra cmake configure arguments...]
rem Defaults: preset win-msvc-qt (Qt 6.7.2 at C:\Qt\6.7.2\msvc2019_64, Debug), binary dir build\<preset>,
rem build type from the preset. The CI variant is ci-win-msvc-qt (warnings as errors).
rem Examples:
rem   tools\dev\build-qt.cmd win-msvc-qt build\m1-debug Debug
rem   tools\dev\build-qt.cmd ci-win-msvc-qt build\m1-ci
rem Environment overrides: ESSES_QT_DIR (Qt kit root; default C:\Qt\6.7.2\msvc2019_64, passed to CMake as
rem CMAKE_PREFIX_PATH when set), ESSES_VCVARS (path to vcvars64.bat), ESSES_CMAKE_ARGS (extra configure
rem arguments; use it for -DNAME=VALUE, because cmd.exe splits positional arguments at '='), ESSES_CTEST_ARGS
rem (extra ctest arguments), ESSES_SKIP_TESTS=1 (configure and build only).
rem The Qt kit's bin directory is put first on PATH and QT_QPA_PLATFORM=offscreen is set, so the Qt Test
rem executables, esses-render and esses run from the build tree without deployment.
setlocal EnableExtensions

for %%I in ("%~dp0..\..") do set "REPO=%%~fI"
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=win-msvc-qt"
set "BINDIR=%~2"
if "%BINDIR%"=="" set "BINDIR=build\%PRESET%"
set "BUILDTYPE=%~3"

rem Collect extra configure arguments (4th and later).
set "EXTRA="
shift & shift & shift
:collect
if "%~1"=="" goto collected
set EXTRA=%EXTRA% %1
shift
goto collect
:collected

set "QTARG="
if defined ESSES_QT_DIR (
  set "QTARG=-DCMAKE_PREFIX_PATH=%ESSES_QT_DIR%"
) else (
  set "ESSES_QT_DIR=C:\Qt\6.7.2\msvc2019_64"
)
if not exist "%ESSES_QT_DIR%\bin\Qt6Core.dll" (
  echo build-qt: no Qt kit at "%ESSES_QT_DIR%"; set ESSES_QT_DIR 1>&2
  exit /b 2
)

rem Remember the CMake on PATH before vcvars (vcvars puts Visual Studio's bundled, older CMake first).
rem where is Windows's own, by its full path: a bare name is looked up in the current folder and along PATH.
set "CMAKE_EXE="
for /f "delims=" %%C in ('^""%SystemRoot%\System32\where.exe" cmake 2^>nul^"') do if not defined CMAKE_EXE set "CMAKE_EXE=%%C"
if not defined CMAKE_EXE (
  echo build-qt: cmake not found on PATH 1>&2
  exit /b 2
)
for %%D in ("%CMAKE_EXE%") do set "CMAKE_DIR=%%~dpD"

if defined VSCMD_VER goto have_vs
rem Default: the newest Visual Studio with the C++ x64 tools (vswhere), else Visual Studio 2022 Community.
set "ESSES_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined ESSES_VCVARS if exist "%ESSES_VSWHERE%" for /f "usebackq delims=" %%I in (`"%ESSES_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ESSES_VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not defined ESSES_VCVARS set "ESSES_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%ESSES_VCVARS%" (
  echo build-qt: vcvars64.bat not found; set ESSES_VCVARS 1>&2
  exit /b 2
)
rem vcvars prints a harmless "vswhere.exe is not recognized" when the installer directory is not on PATH.
call "%ESSES_VCVARS%" >nul 2>nul
if not defined VSCMD_VER (
  echo build-qt: "%ESSES_VCVARS%" did not set up the developer environment 1>&2
  exit /b 2
)
:have_vs
rem Ninja: Visual Studio's copy is on PATH after vcvars; fall back to the Qt tools copy.
"%SystemRoot%\System32\where.exe" ninja >nul 2>nul || set "PATH=%PATH%;C:\Qt\Tools\Ninja"
rem Qt DLLs and plugins for the tests and the applications (the plugins are found relative to Qt6Core.dll).
set "PATH=%ESSES_QT_DIR%\bin;%PATH%"
set "QT_QPA_PLATFORM=offscreen"

pushd "%REPO%" || exit /b 2
set "TYPEARG="
if not "%BUILDTYPE%"=="" set "TYPEARG=-DCMAKE_BUILD_TYPE=%BUILDTYPE%"
echo === configure: preset %PRESET%, binary dir %BINDIR% %TYPEARG% %QTARG%
"%CMAKE_DIR%cmake.exe" --preset "%PRESET%" -B "%BINDIR%" %TYPEARG% %QTARG% %EXTRA% %ESSES_CMAKE_ARGS% || goto fail
echo === build
"%CMAKE_DIR%cmake.exe" --build "%BINDIR%" --parallel || goto fail
if "%ESSES_SKIP_TESTS%"=="1" goto done
echo === test
"%CMAKE_DIR%ctest.exe" --test-dir "%BINDIR%" --output-on-failure --parallel %NUMBER_OF_PROCESSORS% %ESSES_CTEST_ARGS% || goto fail
:done
popd
exit /b 0
:fail
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
