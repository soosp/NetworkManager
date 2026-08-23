# Changelog

All notable changes to this project will be documented in this file.

The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- ESP8266: TX power is applied after `WiFi.begin()`, not before. `setOutputPower()`
  acts on a running radio, so the value set before the radio started was silently
  discarded and the profile's setting never reached the hardware on that connect.
  The ESP32 adapter already did this and said why in its comments; the rule had
  simply not been carried across. The hostname and the static address moved after
  `WiFi.mode(WIFI_STA)` in the same pass, so all three act on an existing station
  netif.

  Note for anyone who meets it: `WiFi.hostname(...): lwIP error -12 on interface
  st` with core debug enabled is **not** a symptom of this. The core assigns the
  name to every netif unconditionally and only then attempts `dhcp_renew()` to
  refresh an existing lease immediately, which fails on an interface that has
  just gone down. The name is set either way and the next DHCP request carries
  it.
- The serving interface's DNS servers are now held against the other interfaces.
  lwIP keeps one resolver table for the whole stack, so any interface that starts
  or acquires a lease takes it over — measured on both platforms: `WiFi.begin()`
  on ESP32 clears the table outright, before association, and on both an arriving
  lease overwrites whatever was there. A primary that keeps retrying and never
  connects therefore stripped DNS from the interface actually carrying traffic,
  which is how a failover run lost SNTP for an hour while routing stayed correct.

  The old repair read the table back and re-asserted it, which cannot work: the
  table is the very thing that was overwritten, and it ran once per change of
  serving adapter, while the damage arrives in between. The servers are now
  captured while they are still ours — from the profile under static addressing,
  from the table at connect under DHCP — and restored whenever it drifts, checked
  once a second.

  A restore also re-arms NTP: SNTP resolves its server by name once and then
  starts name resolution again, so a server name that could not be resolved while
  the table was empty is looked up once there is a resolver.

  The gateway needs none of this and is untouched: it lives per netif, and no
  other interface's lease moves it.

  This repairs damage rather than preventing it. On ESP32 the table stays empty
  for up to a second or so after `WiFi.begin()`, and a name looked up inside that
  window still fails; the check bounds how long it lasts, it cannot close it.

### Added

- `setNtpProfile()` / `clearNtpProfile()` — the NTP servers now come from a
  device-level `NtpProfile` (NetworkProfile >= 0.7.0) rather than from whichever
  interface happens to be up. A device has one clock; where interfaces genuinely
  reach different time servers, keep two profiles and set the one that applies.
  No profile set now means no NTP at all: the per-interface fields it used to
  fall back to are gone from NetworkProfile.
- `NetworkAdapter::setNtpFromDhcp()` — the manager tells an interface, *before*
  starting it, whether to accept DHCP option 42. It has to be before: lwIP
  decides from that flag whether to keep the offer at all, so setting it once the
  lease has arrived is too late. The ESP32 adapters read the flag instead of
  consulting their own profile, which could not see a device-level one.

### Changed

- Guards test `NTP_PROFILE_ENABLED` — "is there NTP in this build" — instead of
  reading a server count and inferring it from the count being non-zero. Buffers
  and loop bounds take `NtpProfile::SERVER_COUNT`, so the size comes from the
  class that owns it. Requires NetworkProfile >= 0.7.0. A build that disabled NTP
  with `NTP_PROFILE_SERVER_COUNT=0` sets `NTP_PROFILE_ENABLED=0` instead; the old
  form is refused with a message naming the replacement, and the examples and the
  host test harness are updated.
- Requires `NetworkProfile` >= 0.7.0, which no longer stores NTP servers per
  interface. Examples updated: they declare an `NtpProfile`, save the server to
  its own namespace, and hand it to the manager before `begin()`.
- `NTP_PROFILE_SERVER_COUNT` replaces `NETWORK_PROFILE_NTP_SERVER_COUNT`
  throughout, including the examples and the host test harness. The old name is
  reported by a `#warning` from NetworkProfile rather than ignored.

### Documentation

- README and API.md follow the changes: `setNtpProfile()` replaces reading the
  servers from a profile, `setNtpFromDhcp()` is documented on the adapter, and
  the teardown section is rewritten around what was measured rather than what was
  assumed — the teardown does *not* clear DNS; another interface starting or
  getting a lease does.

## [0.2.0] - 2026-08-05

### Added

- `NetworkAdapter::getLinkInfo(LinkInfo&)` — live link details an interface may
  have, defaulting to none and overridden by the Wi-Fi adapters (SSID, RSSI,
  BSSID, read from the radio rather than the profile). The manager asks the
  active adapter, so reporting them needs no interface-type switch and a new
  interface type answers for itself.
- `statusToJson()` appends a `"link"` object when the active interface reports
  any of those fields. Each field carries its own validity flag, so a missing
  reading is left out rather than sent as a zero. The SSID is escaped through the
  shared `json_escape()`, so a name containing a quote cannot break the document.

### Changed

- `STATUS_JSON_LEN` grew to cover the `"link"` object at worst case (an SSID with
  every byte escaped).
- Requires `NetworkProfile` >= 0.5.3.

## [0.1.8] - 2026-08-05

### Fixed

- `statusToJson()` wrote a malformed `dns` array. The format string still asked
  for four `%u` octets while the address had already been rendered to a string by
  `ipToStr()`, so it printed the pointer as a number and then read three more
  arguments that were never passed. Now a single `%s`.
- `statusToJson()` produced invalid JSON for the `ntp` object: the separator was
  emitted inside quotes and the server object was closed before its `ip` member,
  giving `""{"name":"x"},"ip":"1.2.3.4"}`. Each server is now one object,
  matching the documented example.
- `statusToJson()` printed an uninitialised buffer as the server name when
  `getActiveNtpName()` failed but the slot had an address — the documented
  `"name":""` case. The name is emptied explicitly.

  All three date from the same never-compiled branch as the 0.1.7 fixes; the
  output has now been checked against the documented example.

## [0.1.7] - 2026-08-05

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

[Unreleased]: https://github.com/soosp/NetworkManager/compare/0.2.0...HEAD
[0.2.0]: https://github.com/soosp/NetworkManager/compare/0.1.8...0.2.0
[0.1.8]: https://github.com/soosp/NetworkManager/compare/0.1.7...0.1.8
[0.1.7]: https://github.com/soosp/NetworkManager/compare/0.1.6...0.1.7
[0.1.6]: https://github.com/soosp/NetworkManager/compare/0.1.5...0.1.6
[0.1.5]: https://github.com/soosp/NetworkManager/compare/0.1.4...0.1.5
[0.1.4]: https://github.com/soosp/NetworkManager/compare/0.1.3...0.1.4
[0.1.3]: https://github.com/soosp/NetworkManager/compare/0.1.2...0.1.3
[0.1.2]: https://github.com/soosp/NetworkManager/compare/0.1.1...0.1.2
[0.1.1]: https://github.com/soosp/NetworkManager/compare/0.1.0...0.1.1
[0.1.0]: https://github.com/soosp/NetworkManager/releases/tag/0.1.0
