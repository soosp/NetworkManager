# Changelog

All notable changes to this project will be documented in this file.

The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- `statusToJson()` did not compile with `NETWORK_PROFILE_NTP_SERVER_COUNT > 0`.
  The NTP branch is behind a preprocessor guard and had never been built: it was
  missing a parenthesis, referred to `NetwotkProfile` (five times) and to a
  lower-case `ipstr`, and one `ipToStr()` call was unterminated.
- `statusToJson()` no longer crashes on ESP8266 and AVR. `PSTR()` literals were
  handed to `_jsonCat()` and `snprintf()`, which read their source from RAM;
  where program space is a separate address space that reads flash byte by byte
  and faults. Formats now use `snprintf_P()`, program-space fragments the new
  `_jsonCat_P()`. ESP32 was unaffected, its flash being memory-mapped.
- `statusToJson()` compared `snprintf()`'s `int` result against a `size_t` buffer
  size (a `-Wsign-compare` warning, and a negative return would have wrapped into
  a huge unsigned value). The checks now use `NetworkProfile::_fitted()`.

### Added

- `_jsonCat_P()` — the program-space counterpart of `_jsonCat()`. Kept separate
  from the RAM version deliberately: a pointer's type does not reveal its address
  space, so the call site has to say it.

### Changed

- The JSON helpers now delegate to `detail/json_tools.h` in `NetworkProfile`
  (already a dependency) rather than carrying a private copy. `_jsonCat()` maps to
  `json_catAtomic()`, which keeps this builder's all-or-nothing behaviour —
  distinct from the truncating variant `NetworkProfile` uses.
- Requires `NetworkProfile` >= 0.5.2.

## [0.1.6] - 2026-07-30

### Fixed

- `getActiveNtpName()` checks whether the query returned an IP address in the NTP
  server name buffer; if so, it gives back an empty buffer and returns `false`
  as specified in the API documentation.

## [0.1.5] - 2026-07-30

### Changed

- refactor: Replace bitwise AND (`&=`) with logical AND (`&&`) to avoid implicit
  bool-to-int conversions and clarify boolean logic intent.
- Bump to `NetworkProfile` version 0.5.0.

## [0.1.4] - 2026-07-24

### Fixed

- Bump to `NetworkProfile` version 0.4.1. In the previous version, there was a
  missing `#include`, which prevented it from compiling on the ESP32.

## [0.1.3] - 2026-07-24

### Fixed

- Re-add the `FallbackTestRunner.ino` example file. It somehow fell out of git
  tracking.

### Changed

- Bump to `NetworkProfile` version 0.4.0. There are no changes to the code;
  however, the comments had to be clarified due to changes in how
  `NetworkProfile` handles WiFi transmit power. This version has eliminated
  the possibility of a hidden discrepancy between the actual value and the
  configured and stored value on the ESP32 platform.

## [0.1.2] - 2026-07-23

### Fixed

- Fix `.gitignore` entry to track file `.vscode\tasks.json` used to access
  NetworkManager Core\`s host test suite directly from VSCode (`Ctrl+Shift+B`).

### Changed

- Update NetworkProfile version dependency to 0.2.0.

### Added

- Add `.vscode\tasks.json` to the git repository.

## [0.1.1] - 2026-07-23

### Fixed

- Minor spelling and clarifying corrections in `README.md`.
- Missing dependencies in the `README.md`.
- Update the ESP8266 test results to the latest layout.
- Fix Changelog links

### Changed

- The value of the `NETWORK_MANAGER_MAX_ADAPTERS` macro has been reduced from 4
  to 2, because there are currently only two supported types, and only one
  instance of each is allowed. This allows applications using the default
  settings to save 2x the memory required by NTP servers. This macro can be
  configured by the application; only the default value has changed.

## [0.1.0] - 2026-07-16

First public release.

### Added

- Priority-based network manager coordinating multiple `NetworkAdapter`
  instances, with automatic fallback to the next-highest-priority interface and
  restore to a higher-priority one when it recovers.
- High-level events delivered to the application: `CONNECTED`, `FALLBACK`,
  `RESTORED` and `DISCONNECTED`. `DISCONNECTED` is debounced — a working
  lower-priority fallback never triggers it — is emitted at most once per outage,
  and also covers a boot where no interface ever connects. Its adapter argument
  reports the interface that was last serving.
- Adapters for ESP32 (Ethernet, WiFi), ESP8266 (Ethernet, WiFi) and AVR
  (Ethernet), sharing a common `NetworkAdapter` interface.
- Optional NTP support with per-interface server configuration, live/configured
  server distinction, and DHCP-provided NTP (option 42) on ESP platforms.
  Compile-time opt-in via `NETWORK_PROFILE_NTP_SERVER_COUNT` (default 0 on AVR
  to save RAM).
- Device-level hostname (`setHostname`, fanned out to all adapters) and
  per-interface MAC/hostname accessors at the manager level.
- Runtime WiFi TX-power control on ESP adapters, written through to the profile
  so it survives reconnects.
- Automatic recovery of the lwIP default route and DNS resolver after an
  interface handoff on ESP32 and ESP8266, so an FQDN NTP server keeps resolving
  across fallback and restore (including with static addressing, where no
  DHCP-driven route re-election occurs).
- AVR Ethernet cold-boot DHCP wait is bounded with the timeout overload of
  `Ethernet.begin()` (`AVR_ETH_ADAPTER_DHCP_BEGIN_TIMEOUT`), so a boot without a
  link is not stalled for the library's built-in ~60 s timeout.
- Host-side regression suite driving the real decision core
  (`test/nm_harness.cpp`), and a semi-automatic 32-case hardware fallback test
  runner (`examples/FallbackTestRunner`). See `Testing.md`.
- Universal examples: `Ethernet` (ESP32/ESP8266/AVR), `WiFi` (ESP) and
  `EthWiFiFallback` (ESP).
- API reference (`API.md`) and testing notes (`Testing.md`).

[Unreleased]: https://github.com/soosp/NetworkManager/compare/0.1.6...HEAD
[0.1.6]: https://github.com/soosp/NetworkManager/compare/0.5.4...0.1.6
[0.1.5]: https://github.com/soosp/NetworkManager/compare/0.1.4...0.1.5
[0.1.4]: https://github.com/soosp/NetworkManager/compare/0.1.3...0.1.4
[0.1.3]: https://github.com/soosp/NetworkManager/compare/0.1.2...0.1.3
[0.1.2]: https://github.com/soosp/NetworkManager/compare/0.1.1...0.1.2
[0.1.1]: https://github.com/soosp/NetworkManager/compare/0.1.0...0.1.1
[0.1.0]: https://github.com/soosp/NetworkManager/releases/tag/0.1.0
