@echo off
rem SPDX-FileCopyrightText: 2026 the Esses contributors
rem SPDX-License-Identifier: MIT
rem
rem Portable Esses (D-212): removes what associate-ld.cmd added for the current user. The default handler of .ld is
rem removed only while it is Esses's.
setlocal
reg query "HKCU\Software\Classes\.ld" /ve 2>nul | find "Esses.LogFile" >nul && reg delete "HKCU\Software\Classes\.ld" /ve /f >nul
reg delete "HKCU\Software\Classes\.ld\OpenWithProgids" /v "Esses.LogFile" /f >nul 2>nul
reg delete "HKCU\Software\Classes\Esses.LogFile" /f >nul 2>nul
echo the .ld association of Esses is removed
exit /b 0
