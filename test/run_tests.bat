@echo off
REM run_tests.bat - build and run the NetworkManager host tests on Windows.
REM
REM Needs a C++17 g++ on PATH (e.g. MinGW-w64 / MSYS2, or the toolchain that
REM ships with many Arduino setups). No make required. Double-click it, or run
REM it from a terminal. The window stays open at the end so you can read the
REM results.

REM Run from this script's own folder, so double-clicking works.
cd /d "%~dp0"

where g++ >nul 2>nul
if errorlevel 1 (
  echo ERROR: g++ not found on PATH.
  echo Install MinGW-w64 / MSYS2 and add its bin folder to PATH, then retry.
  echo.
  pause
  exit /b 1
)

REM Headers may be in the project root ^(..^) or under ..\src - search both.
set FLAGS=-std=c++17 -O0 -Wall -I. -I.. -I..\src

set RC=0

echo ------------------------------------------------------------
echo Building Core regression suite ...
g++ %FLAGS% nm_harness.cpp -o nm_harness.exe
if errorlevel 1 ( echo BUILD FAILED: nm_harness.cpp & set RC=1 & goto :glue )
echo Running Core regression suite:
nm_harness.exe
if errorlevel 1 set RC=1

:glue
echo ------------------------------------------------------------
echo Building real-glue smoke test ...
g++ %FLAGS% test_glue.cpp -o test_glue.exe
if errorlevel 1 ( echo BUILD FAILED: test_glue.cpp & set RC=1 & goto :done )
echo Running real-glue smoke test:
test_glue.exe
if errorlevel 1 set RC=1

:done
echo ------------------------------------------------------------
if "%RC%"=="0" (
  echo ALL TESTS PASSED.
) else (
  echo SOME TESTS FAILED ^(see output above^).
)
echo.
REM Keep the window open for double-click users; the VSCode task passes an
REM argument to skip the pause so the integrated terminal does not block.
if "%~1"=="" pause
exit /b %RC%