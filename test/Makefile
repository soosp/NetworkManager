# Makefile — builds and runs the NetworkManager host tests.
#
# Requires a C++17 compiler (g++ or clang++) and GNU make. From this folder:
#   make            build and run both test suites
#   make core       build + run only the Core regression suite
#   make glue       build + run only the real-glue smoke test
#   make clean      remove the built binaries
#
# Intended for Unix-like shells: Linux, macOS, WSL, Git-Bash or MSYS2 on
# Windows. Native Windows cmd users: run  run_tests.bat  instead (no make
# needed). The library headers are searched one level up (..) and in ../src,
# so this works whether the headers sit in the project root or under src/.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O0 -Wall
INCLUDES := -I. -I.. -I../src

CORE := nm_harness
GLUE := test_glue

.PHONY: all run core glue clean
all: run

# Default: build + run everything.
run: core glue

$(CORE): nm_harness.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(GLUE): test_glue.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

core: $(CORE)
	@echo "------------------------------------------------------------"
	@echo "Core regression suite:"
	@./$(CORE)

glue: $(GLUE)
	@echo "------------------------------------------------------------"
	@echo "Real-glue smoke test:"
	@./$(GLUE)

clean:
	@rm -f $(CORE) $(GLUE) $(CORE).exe $(GLUE).exe