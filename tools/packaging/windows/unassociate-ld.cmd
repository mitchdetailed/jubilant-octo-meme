@echo off
rem SPDX-FileCopyrightText: 2026 the Esses contributors
rem SPDX-License-Identifier: MIT
rem
rem Portable Esses (D-212): removes what associate-ld.cmd added for the current user. The default handler of .ld is
rem removed only while it is Esses's (exactly Esses.LogFile); another program's stays.
rem reg is Windows's own, by its full path, and no program is looked up by its bare name (D-218 (b); for /f starts
rem cmd through %ComSpec%): cmd looks a bare command name up in the current folder first and then along PATH,
rem where another program of the same name can come before System32 (Git for Windows' usr\bin has a GNU find,
rem which made "reg query | find" fail and kept .ld associated).
setlocal EnableExtensions DisableDelayedExpansion
set "REG=%SystemRoot%\System32\reg.exe"
if not exist "%REG%" (
  echo unassociate-ld: %%SystemRoot%%\System32\reg.exe was not found 1>&2
  exit /b 1
)
rem reg query prints the default value as "    <its name>    REG_SZ    <data>". The name depends on the language of
rem Windows and may hold a space, so the line is matched on what follows REG_SZ: exactly Esses.LogFile. A line is
rem stored while delayed expansion is off, so a ! in another program's value cannot turn it into Esses.LogFile.
set "OURS="
for /f "delims=" %%L in ('^""%REG%" query "HKCU\Software\Classes\.ld" /ve 2^>nul^"') do (
  set "LINE=%%L"
  setlocal EnableDelayedExpansion
  if "!LINE:*    REG_SZ    =!"=="Esses.LogFile" (
    endlocal
    set "OURS=1"
  ) else (
    endlocal
  )
)
set "FAILED="
if defined OURS "%REG%" delete "HKCU\Software\Classes\.ld" /ve /f >nul || set "FAILED=1"
"%REG%" delete "HKCU\Software\Classes\.ld\OpenWithProgids" /v "Esses.LogFile" /f >nul 2>nul
"%REG%" delete "HKCU\Software\Classes\Esses.LogFile" /f >nul 2>nul
if defined FAILED (
  echo unassociate-ld: the default handler of .ld, Esses.LogFile, could not be removed 1>&2
  exit /b 1
)
echo the .ld association of Esses is removed
exit /b 0
