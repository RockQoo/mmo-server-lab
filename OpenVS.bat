@echo off
rem Generate the VS 2026 solution and filters (server-cpp/build/vs) and open one in Visual Studio.
rem Re-run after adding files to CMakeLists.txt.
rem   OpenVS.bat           ask which view to open
rem   OpenVS.bat Core      open directly (All, Core, Servers, Tests)
set "FILTER=%~1"
if not "%FILTER%"=="" goto open

echo   1. All      (whole solution)
echo   2. Core     (Core libraries only)
echo   3. Servers  (Core + servers)
echo   4. Tests    (all tests + what they test)
choice /c 1234 /n /m "Open which view? [1-4] "
if errorlevel 4 (set "FILTER=Tests" & goto open)
if errorlevel 3 (set "FILTER=Servers" & goto open)
if errorlevel 2 (set "FILTER=Core" & goto open)
set "FILTER=All"

:open
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0server-cpp\build.ps1" -Sln -Filter %FILTER%
if errorlevel 1 pause
