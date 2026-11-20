@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" -Mode Install %*
set "result=%ERRORLEVEL%"
echo.
if not "%result%"=="0" echo Installation did not complete. See the message above.
pause
exit /b %result%
