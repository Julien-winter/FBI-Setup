@echo off
REM Undo-FBI-Setup - reverts the changes made by FBI Setup
REM Elevates to administrator if needed, then runs Undo-FBI-Setup.ps1
setlocal
REM Admin check: reading the LOCAL SERVICE hive needs admin rights and does
REM not depend on the Server service (unlike "net session")
reg query HKU\S-1-5-19 >nul 2>&1
if %errorLevel%==0 goto run
echo Requesting administrator privileges...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b
:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Undo-FBI-Setup.ps1"
echo.
pause
