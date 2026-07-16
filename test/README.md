# NetworkManager host tests

These tests run on a normal PC — no ESP32, no flashing. They exercise the
fallback / restore / reconnect logic on the host, in seconds, so regressions are
caught before anything reaches hardware.

Two suites:

- **`nm_harness.cpp`** — drives `NetworkManagerCore` (the pure decision logic)
  through a set of scenarios and asserts the exact event sequence each produces.
- **`test_glue.cpp`** — instantiates the real `NetworkManagerClass` singleton
  with a mock adapter and runs a full CONNECTED → FALLBACK → RESTORED →
  DISCONNECTED → CONNECTED path. This also compile-checks the shipping headers.

`Arduino.h` and `NetworkProfile.h` here are tiny **host stubs** — just enough for
the glue to compile and run on a PC. They are not used on the device.

## Requirements

A C++17 compiler on your PATH: `g++` / `clang++` on Linux/macOS, or `g++` from
MSYS2 on Windows. Nothing else.

### Windows — installing g++ via MSYS2

1. Download and install MSYS2 from <https://www.msys2.org> (default path:
   `C:\msys64`).
2. Open the **MSYS2 UCRT64** shell and run:

   ```shell
   pacman -S mingw-w64-ucrt-x86_64-gcc
   ```

3. Add the compiler to your PATH so `cmd.exe` and VSCode can find it. Choose
   one of:

   **Permanently (recommended):** Add `C:\msys64\ucrt64\bin` to your user PATH
   via *System Properties → Environment Variables*.

   **For the VSCode task only:** The supplied `.vscode/tasks.json` already adds
   the MSYS2 paths automatically for the build task, so Ctrl+Shift+B works
   without touching the system PATH. If you installed MSYS2 somewhere other than
   `C:\msys64`, update the two paths in the `options.env.PATH` entry of
   `.vscode/tasks.json`.

   **For `run_tests.bat` (double-click / cmd):** Either the permanent option
   above, or prepend the path in the same cmd window before running:

   ```cmd
   set PATH=C:\msys64\ucrt64\bin;C:\msys64\usr\bin;%PATH%
   run_tests.bat
   ```

## Running

Pick whichever fits your setup — they all do the same thing:

|Environment|Command|
|-------------------------------------|--------------------|
|Linux / macOS / WSL / Git-Bash|`./run_tests.sh`|
|Windows (cmd / double-click)|`run_tests.bat`|
|Anywhere with `make`|`make`|
|VSCode|Ctrl+Shift+B|

On Linux/macOS you may first need `chmod +x run_tests.sh`. The VSCode task is in
`.vscode/tasks.json`; it assumes the project root is your workspace folder.

A passing run ends with `8/8 passed` for the Core suite and `[PASS]` for the
glue smoke test.

## Layout assumption

The build commands look for the library headers one level up (`..`) **and** in
`../src`, so this works whether your headers sit in the project root or under
`src/`. If your layout differs, adjust the include paths in `Makefile` /
`run_tests.sh` / `run_tests.bat`.
