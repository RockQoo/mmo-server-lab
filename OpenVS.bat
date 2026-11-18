@echo off
rem Generate the VS 2026 solution (server-cpp/build/vs) and open it in Visual Studio.
rem Re-run after adding files to CMakeLists.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0server-cpp\build.ps1" -Sln
if errorlevel 1 pause
