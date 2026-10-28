@echo off
rem SPDX-FileCopyrightText: 2026 the Esses contributors
rem SPDX-License-Identifier: MIT
rem
rem Configure, build and test the Qt-free core with MSVC + Ninja from any shell (cmd, PowerShell, Git Bash).
rem   build-core.cmd [preset] [binary-dir] [build-type] [extra cmake configure arguments...]
rem Defaults: preset win-msvc-core, binary dir build\<preset>, build type from the preset.
rem Examples:
rem   tools\dev\build-core.cmd win-msvc-core build\skeleton-debug Debug
rem   tools\dev\build-core.cmd win-msvc-core build\skeleton-rel RelWithDebInfo
rem Environment overrides: ESSES_VCVARS (path to vcvars64.bat), ESSES_CMAKE_ARGS (extra configure arguments;
rem use it for -DNAME=VALUE, because cmd.exe splits positional arguments at '='), ESSES_CTEST_ARGS (extra ctest
rem arguments), ESSES_SKIP_TESTS=1 (configure and build only).
setlocal EnableExtensions

for %%I in ("%~dp0..\..") do set "REPO=%%~fI"
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=win-msvc-core"
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

rem Remember the CMake on PATH before vcvars (vcvars puts Visual Studio's bundled, older CMake first).
rem where is Windows's own, by its full path: a bare name is looked up in the current folder and along PATH.
set "CMAKE_EXE="
for /f "delims=" %%C in ('^""%SystemRoot%\System32\where.exe" cmake 2^>nul^"') do if not defined CMAKE_EXE set "CMAKE_EXE=%%C"
if not defined CMAKE_EXE (
  echo build-core: cmake not found on PATH 1>&2
  exit /b 2
)
for %%D in ("%CMAKE_EXE%") do set "CMAKE_DIR=%%~dpD"

if defined VSCMD_VER goto have_vs
rem Default: the newest Visual Studio with the C++ x64 tools (vswhere), else Visual Studio 2022 Community.
set "ESSES_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined ESSES_VCVARS if exist "%ESSES_VSWHERE%" for /f "usebackq delims=" %%I in (`"%ESSES_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ESSES_VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not defined ESSES_VCVARS set "ESSES_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%ESSES_VCVARS%" (
  echo build-core: vcvars64.bat not found; set ESSES_VCVARS 1>&2
  exit /b 2
)
rem vcvars prints a harmless "vswhere.exe is not recognized" when the installer directory is not on PATH.
call "%ESSES_VCVARS%" >nul 2>nul
if not defined VSCMD_VER (
  echo build-core: "%ESSES_VCVARS%" did not set up the developer environment 1>&2
  exit /b 2
)
:have_vs
rem Ninja: Visual Studio's copy is on PATH after vcvars; fall back to the Qt tools copy.
"%SystemRoot%\System32\where.exe" ninja >nul 2>nul || set "PATH=%PATH%;C:\Qt\Tools\Ninja"

pushd "%REPO%" || exit /b 2
set "TYPEARG="
if not "%BUILDTYPE%"=="" set "TYPEARG=-DCMAKE_BUILD_TYPE=%BUILDTYPE%"
echo === configure: preset %PRESET%, binary dir %BINDIR% %TYPEARG%
"%CMAKE_DIR%cmake.exe" --preset "%PRESET%" -B "%BINDIR%" %TYPEARG% %EXTRA% %ESSES_CMAKE_ARGS% || goto fail
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
