@echo off
rem SPDX-FileCopyrightText: 2026 the Esses contributors
rem SPDX-License-Identifier: MIT
rem
rem Portable Esses (D-212; 06 section 19 item 5): associates .ld log files with the esses.exe beside this script for
rem the current user (HKCU, no administrator rights): ProgId Esses.LogFile, "esses.exe" -l "%1". Run it again
rem after moving the folder; unassociate-ld.cmd removes the association. A program the user chose in "Open with"
rem stays the user's choice.
setlocal
set "EXE=%~dp0esses.exe"
if not exist "%EXE%" (
  echo associate-ld: esses.exe is not beside this script 1>&2
  exit /b 1
)
reg add "HKCU\Software\Classes\Esses.LogFile" /ve /d "Esses log file" /f >nul || exit /b 1
reg add "HKCU\Software\Classes\Esses.LogFile\DefaultIcon" /ve /d "\"%EXE%\",0" /f >nul || exit /b 1
reg add "HKCU\Software\Classes\Esses.LogFile\shell\open\command" /ve /d "\"%EXE%\" -l \"%%1\"" /f >nul || exit /b 1
reg add "HKCU\Software\Classes\.ld\OpenWithProgids" /v "Esses.LogFile" /t REG_NONE /f >nul || exit /b 1
reg add "HKCU\Software\Classes\.ld" /ve /d "Esses.LogFile" /f >nul || exit /b 1
echo .ld files now open with "%EXE%"
exit /b 0
