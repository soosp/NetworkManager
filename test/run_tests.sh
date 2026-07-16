#!/usr/bin/env sh
# run_tests.sh — build and run the NetworkManager host tests.
#
# Works on Linux, macOS, WSL, Git-Bash and MSYS2. Needs a C++17 compiler
# (g++ or clang++) on PATH. No make required.
#
#   ./run_tests.sh        (you may need:  chmod +x run_tests.sh  first)

# Always run from this script's own directory, so double-clicking or calling
# it from elsewhere still finds the sources and the library headers.
cd "$(dirname "$0")" || exit 1

# Pick a compiler: prefer g++, fall back to clang++ or the generic c++.
if   command -v g++      >/dev/null 2>&1; then CXX=g++
elif command -v clang++  >/dev/null 2>&1; then CXX=clang++
elif command -v c++      >/dev/null 2>&1; then CXX=c++
else
  echo "ERROR: no C++ compiler found (need g++, clang++ or c++ on PATH)."
  exit 1
fi

# Headers may be in the project root (..) or under ../src — search both.
FLAGS="-std=c++17 -O0 -Wall -I. -I.. -I../src"

build_and_run() {
  src="$1"; out="$2"; label="$3"
  echo "------------------------------------------------------------"
  echo "Building $label ..."
  if ! $CXX $FLAGS "$src" -o "$out"; then
    echo "BUILD FAILED: $src"
    return 1
  fi
  echo "Running $label:"
  "./$out"
  return $?
}

rc=0
build_and_run nm_harness.cpp nm_harness "Core regression suite" || rc=1
build_and_run test_glue.cpp  test_glue  "real-glue smoke test"  || rc=1

echo "------------------------------------------------------------"
if [ "$rc" -eq 0 ]; then
  echo "ALL TESTS PASSED."
else
  echo "SOME TESTS FAILED (see output above)."
fi
exit "$rc"