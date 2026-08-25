# Changelog

All notable changes to this project will be documented in this file.

The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Removed

- `NetworkAdapter::_requestStop()`, `_consumePendingStop()` and the `_pendingStop`
  member. All three were unreachable: both methods are private, the only `friend`
  is `NetworkManagerClass`, and the manager has always deferred stops through its
  own `_pendingStopBits[]` instead. Checked against every commit in this
  repository — no call to either has ever existed, so this removes code that was
  dead from the first commit rather than orphaned by a later change.

  They were a symmetric copy of the adapter's `_pendingApply` pair, and the
  asymmetry turns out to be correct: an apply is requested from outside through
  the public `requestApply()`, so its flag belongs to the adapter being asked,
  while a stop is a Core decision that arrives as `Decision::stopIdx` and is
  already on the manager's side. Both remaining mechanisms now say so, to keep
  the missing half from being restored out of symmetry.

  Also confusing on the way out: `NetworkAdapter::_requestStop()` shared its name
  with the live `NetworkManagerClass::_requestStop(int8_t)`, and the `friend`
  declaration's comment advertised access to `_consumePendingStop()` that was
  never used. Both are corrected.

### Fixed

- `NETWORK_MANAGER_MAX_ADAPTERS` actually defaults to 2. It was reduced from 4
  to 2 in 0.1.1, but the `#ifndef` stating that sat *below* the include of
  `NetworkManagerCore.h`, which carries its own fallback of 4 for when it is
  compiled alone. The Core got there first, so every build that did not set the
  macro itself silently used 4 — including the adapter table and the Core's
  `StateView` arrays. `README.md` and `API.md` documented 4 and were right about
  the behaviour while the CHANGELOG was right about the intent.

  Both fallbacks are now 2 and the one here is stated above the includes, with
  the reason written at both sites so they are kept in step. The range check
  moved up with it: the Core sizes arrays from this macro, so an out-of-range
  value has to be rejected before it does.

  Applications that set the macro themselves are unaffected. Those that did not
  get two fewer adapter slots than they have been getting, which is the
  documented maximum and the number of adapter types this library has.
- ESP32: `_configureNtp()` now re-asserts `esp_sntp_servermode_dhcp()` from the
  profile in use, as the ESP8266 path has always done with
  `sntp_servermode_dhcp()`. Both platforms make this choice once before
  `start()`, because lwIP must know before the DHCP handshake — but with
  per-interface `NtpProfile`s swapped from an event handler, that early decision
  is necessarily made from the *previous* interface's profile: the event that
  swaps them cannot arrive until the interface is already up. ESP8266 corrected
  itself at configure time; ESP32 had no counterpart there and stayed out of step
  until the next handoff.

  Two consequences, both only reachable when two profiles disagree about holding
  servers. Configured servers could be overwritten at a later lease renewal by an
  option-42 server whose capture was never switched off. And an interface left
  without servers never picked one up, because capture had been switched off for
  the previous interface's configured ones.

  This does not rescue a handshake that has already happened — a server offered
  in option 42 while capture was off is not applied retroactively — so the second
  case now waits for a lease renewal rather than never resolving. `API.md`'s
  "Mixed DHCP and configured NTP" table is updated accordingly.

  Where both profiles hold servers, or neither does, the value written here is
  the one already in force and nothing changes.

- The adapters' static member definitions are `inline`, so a project may include
  an adapter header from more than one translation unit. Twelve definitions
  across `ESP32WiFiAdapter.h`, `ESP32EthAdapter.h` and `ESP8266WiFiAdapter.h`
  sat at namespace scope without it, which means every including unit emitted
  its own and a second `.cpp` failed to link with `multiple definition of
  ESP32WiFiAdapter::_instance` and friends. `#pragma once` does not help: it
  suppresses a repeated include within one unit, not across units.

  A single-file Arduino sketch never met this — everything lands in one unit
  there — and even a multi-file project usually includes an adapter from just one
  `.cpp`, which is why it survived this long. It was nonetheless a limit nobody
  chose and nobody wrote down. Where such a constraint is real this library says
  so, as `NetworkManagerNtpSyncHook.h` does ("exactly one translation unit"), so
  silence here read as "include it wherever you like" — and now that is true.

  `inline` keeps the header-only layout that the Arduino IDE's build order
  requires — moving the definitions to a `.cpp` would have traded one problem
  for a worse one. Both cores compile at C++17 or later, so it is available on
  each. AVR is unaffected; `ESP8266EthAdapter.h` and `AVREthernetAdapter.h` have
  no such definitions.
- `isConnected()` and `getActiveAdapter()` no longer take the manager's mutex,
  which they held to no effect and at a cost. Both do nothing but call
  `_getConnectedIndex()` and return what it found, and that needs no lock: the
  adapter table is immutable after `begin()`, adapter states are atomic (and
  were never written under *this* mutex in the first place), and neither result
  is a reservation — it escapes the lock before the caller acts on it either
  way.

  The cost was a wrong answer. On a `_lock()` timeout these reported "not
  connected" and "no active adapter" — statements about the network, made when
  the only thing that had actually failed was acquiring a lock. `nullptr` from
  `getActiveAdapter()` now means nothing is connected, and nothing else.

  A failover can land between reading the state and the caller using the
  result, and that was equally true before: the value escaped the lock either
  way, so locking narrowed the window by a few instructions while the caller's
  use of it — a `getStatus()`, a whole `statusToJson()` — stayed outside
  regardless. Closing it properly would mean holding the mutex across caller
  code, which this library does not do. The exposure is bounded by design: the
  index cannot go out of range and the pointer cannot dangle, so the worst case
  is a stale adapter answering honestly for itself.

  The three conditions the removal rests on are written out at the call site, so
  that a later change which breaks one of them — adapters becoming addable after
  `begin()`, above all — shows plainly where the lock would have to go back.
  `addAdapter()`'s own documentation now states the "before `begin()`" rule that
  the rest of this depends on; only `API.md` had said it before.

  `getActiveAdapter()` also gained the caveat it had always needed: it is for
  reaching the serving adapter now, not for keeping. A pointer stored across a
  fallback goes on naming the interface that was serving. Storing the index
  instead is no safer — the table is fixed, so a stale index resolves to the
  same stale adapter.
- `NetworkAdapter::_state` is now atomic on ESP32 and ESP8266, joining
  `_pendingApply`, `_pendingStop` and `_lastFailedMs`, which were already. It is
  written by `_setState()` on whichever task classified the change — the SDK
  network-event task on ESP32 — and read by `getState()` from the loop task with
  no lock, so a plain member was a cross-task read of something being written
  concurrently. It stays a plain member on AVR, which has one task.

  Two comments described the code as if this had always been the case and are
  now true rather than aspirational: `_snapshot()` said it read each state with
  an atomic load, and `_getConnectedIndex()` claimed the caller had to hold
  `_mutex`. The latter was wrong in both directions — holding the manager's
  mutex synchronises nothing here, since the states are written under each
  adapter's own mutex, and four of the six call sites did not hold it anyway.
  What the function actually needs is a stable adapter table, which
  `addAdapter()`'s "before `begin()`" contract already guarantees; that is what
  it now says.

  No behaviour change is expected on either platform: a one-byte aligned load
  compiles to the same instruction. What changes is that the guarantee is stated
  in the type rather than assumed by the reader.
- `statusToJson()` dropped the whole document when an NTP server name ran past
  53 characters. Each server's `name` field was formatted through the shared
  64-byte scratch buffer, but a name may be a full-length FQDN — the very length
  `STATUS_JSON_LEN` budgets for. A short write fails the build outright, so one
  long name cost the entire object rather than just its own field, and the caller
  got an empty string with no indication why. The name is now appended in pieces
  straight into the output, as the SSID beside it already was, and escaped on the
  way, so a name carrying anything a host name should not can no longer break the
  syntax either. Measured: names failed from 54 characters up before, and are
  carried to the full 253 now.

  The NTP block was also the one part of this builder still formatting with
  RAM-resident literals while everything around it used `PSTR()`; it now matches,
  which returns a little flash on AVR.
- `getNtp()` dereferenced a null output buffer. The initial clear was guarded
  (`if (out && len)`), but the read that followed it was not, so a null pointer
  or a zero length faulted instead of reporting false. The private
  `_ntpServerAt()` helper behind it now tolerates one as well, so a future caller
  cannot reintroduce the same fault from a different direction.
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

### Fixed (tests)

- The `test/` stub of `NetworkProfile.h` gained `json_cat_esc()`, which
  `statusToJson()` now calls. Same drift as the entry below: the stub
  reimplements the JSON helpers rather than shimming them, so every helper the
  shipping header starts using has to be added here too, or `test_glue` stops
  proving that the header compiles.
- The host test harness compiles again. `statusToJson()` began using the PROGMEM
  helpers and the shared JSON tools when the ESP8266 port landed, and
  `NetworkProfile::ipToStr()` along with them, but the stubs under `test/` were
  never taught about any of it — so `test_glue`, whose whole purpose is to prove
  that the shipping header compiles, had quietly stopped doing that. The stubs
  now provide the PROGMEM macros and working implementations of `json_fitted()`,
  `json_catAtomic()`, `json_catAtomic_P()`, `json_escape()` and `ipToStr()`.
  Implementations rather than empty shims: a stub that behaved differently would
  let the test pass on output the device would never produce.

### Added

- `NetworkAdapter::setNtpProfile()`, binding NTP servers to a single interface.
  An adapter's own profile wins over the manager's device-level one; unbound
  adapters keep using that, so the two mix and a device with one clock source
  needs nothing new.

  This restores behaviour that the move of NTP into `NtpProfile` lost. Before it,
  each profile carried its own servers and the manager read them from *the
  adapter it was about to start* — the comment at that call site said so
  outright, and explained why the decision cannot wait: lwIP settles whether to
  keep DHCP option 42 during the handshake, so a choice made at CONNECTED is
  already too late. Afterwards the only source was device-level, which at start
  time necessarily still describes the previous interface, because the event
  telling the application which one is taking over cannot arrive until that
  interface is already up. Re-aiming the device-level profile from an event
  handler — which this library's own examples and documentation recommended —
  cannot close that gap, and no amount of documenting it would have.

  The effect was confined to interfaces that disagree about holding servers, and
  it was quiet: an interface meant to take its time server from DHCP kept
  serving the previous interface's, because `configTime(0, 0, NULL, NULL, NULL)`
  clears lwIP's server *names* and leaves the resolved addresses in place.

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

- The global `NetworkManager` is a reference rather than a macro. On ESP32 the
  Arduino core has a class of the same name in its `Network` library, and a
  macro rewrote every later occurrence of the word — including that class's own
  declaration. Whether a sketch compiled therefore depended on which header it
  included first: adapter headers pull in `<WiFi.h>`/`<ETH.h>` and with them the
  core's declaration, so the examples worked, while the natural

  ```cpp
  #include <NetworkManager.h>
  #include <WiFiAdapter.h>
  ```

  failed with `expected identifier before '(' token` pointing into this
  library's header, nowhere near the cause.

  A reference coexists with the class: a qualified name like
  `NetworkManager::setHostname()` — the core's static hostname API — is looked
  up among types and namespaces only, so it still finds the class, while
  `NetworkManager.begin()` finds the reference. `<Network.h>` is now included at
  the top of this header so the class is always declared before the name is
  taken, rather than that depending on the sketch. The adapters already included
  it and use `Network.onEvent()`, so nothing new is linked in.

  Sketch code is unaffected — `NetworkManager.begin()` reads and behaves as
  before. Two details differ underneath: the singleton is constructed during
  static initialisation instead of on first use (its constructor is defaulted
  and touches no other global, so there is no ordering hazard), and
  `instance()` joins `_getNetworkManagerInstance()` as a public spelling of the
  accessor, which is what `NETWORK_MANAGER_NO_GLOBAL` builds use.

  One spelling remains ambiguous: naming the core's class as a plain type in a
  declaration, `NetworkManager *p`, finds the reference. Write
  `class NetworkManager *p`, or define `NETWORK_MANAGER_NO_GLOBAL` to suppress
  the global entirely — the same escape `EEPROMPreferences` offers for its
  `Preferences` alias.

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
- `getNtp()` no longer requires a connection. The NTP servers are persisted
  configuration read from the `NtpProfile` given to `setNtpProfile()`, so they
  stay knowable while every interface is down — gating them on connectivity hid
  the settings exactly when an application most wants to show them, such as on a
  configuration page served by a device that cannot currently reach the network.
  It also put the method on the wrong side of the library's own split:
  `getHostname()` and `getMac()` report configured values without a connection,
  while `getLocalIP()` and the other address getters require one because a live
  netif property genuinely does not exist without it. `getNtp()` was a configured
  value behaving like a live one.

  Only the disconnected case changes. Connected behaviour, the signature and the
  buffer contract are untouched, `out` is still emptied before anything else, and
  an unused slot still reports false. DHCP-provided servers (option 42) are
  unaffected: they are never configuration, and have always been reported by
  `getActiveNtpName()`/`getActiveNtpIP()` instead, which read the live SNTP
  client and go empty when `_disableNtp()` tears it down as the last interface
  goes down.

  Applications reading `getNtp()`'s false as "not connected" should use
  `isConnected()`.

### Documentation

- README and API.md follow the changes: `setNtpProfile()` replaces reading the
  servers from a profile, `setNtpFromDhcp()` is documented on the adapter, and
  the teardown section is rewritten around what was measured rather than what was
  assumed — the teardown does *not* clear DNS; another interface starting or
  getting a lease does.
- Comments left behind by the NTP move are corrected. `clearNtpProfile()` said
  it falls back to "the deprecated per-interface fields" — those fields are what
  `NtpProfile` replaced, and there is no fallback: no profile means no configured
  servers, so the next connect lets DHCP supply one, exactly as an empty profile
  would. `ESP32WiFiAdapter`'s static-IP handler still apologised for
  `getConfig()` copying NTP fields, which it no longer does. And the block
  explaining `_ntpSyncInterval` had come to rest above `_ntpProfile`, leaving the
  field it describes with no comment and the field it sits on with two.
- `onNtpSync()` and the `NtpSyncCb` type claimed the callback is never invoked on
  AVR and that NTP there is the application's problem. The manager has run its
  own UDP SNTP client on AVR for a while, and calls the callback from it; the
  README and API.md already described that client, so only the header disagreed.
- `getNtp()` is documented in API.md, next to the two *active* getters it is
  easily confused with. The configured-versus-active split is the whole point of
  those three methods and the reference described only two of them.
- `EthProfile::_initMacDefault()` said it leaves `_macDefault` zero on ESP8266
  and generates a hostname prefixed "ETH" or "AVR". It derives a
  locally-administered MAC from the WiFi STA address there, and the prefix is
  "ESP" or "AVR" — it names the controller rather than the interface, so both of
  a device's profiles generate the same default hostname.
- `test/README.md` promised `8/8 passed`; the Core suite has eleven cases.

### Examples

- Every `setConfig()` and `setServer()` call is checked. `setConfig()` validates
  the whole struct and stores nothing if any field fails, so the unchecked calls
  the examples used to make would leave a profile at its defaults while the
  sketch ran on as if it had been configured. An empty SSID then presents as a
  WiFi fault rather than as a typo at the top of the file — a debugging session
  that starts in entirely the wrong place. The same went for `setServer()`,
  where a mistyped NTP name left the slot empty and the clock quietly
  unsynchronised.

  The four sketches now report the offending field through
  `NetworkProfile::configCheckName()`, and the two fallback sketches share an
  `applyOrHalt()` helper that stops rather than continuing on an unusable
  profile, which is the right answer for a sketch: there is nobody to re-enter
  it.

- `EthWiFiFallback` can exercise DHCP-provided NTP again, and per interface. The
  NTP move emptied the two `if` bodies that used to fill `ntp[]` in each network
  config but left the shells and the `ETH_NTP_DHCP` / `WIFI_NTP_DHCP` flags
  behind, so both flags controlled nothing: the sketch always configured three
  servers through a single profile and option 42 was never reached.

  It now keeps one `NtpProfile` per interface and points the manager at the
  serving one from `onNetworkEvent()`. A flag set to true leaves that profile
  empty, which is how the manager is told to ask DHCP. Setting the two flags
  differently is the mixed configuration described under "Mixed DHCP and
  configured NTP" in API.md, and the handler says so where a reader will meet it.

- `EthWiFiFallback` sets its NTP servers through an `NtpProfile` instead of
  writing `cfg.ntp[]`, which is gone; the profile is device-level, so the servers
  no longer have to be repeated for each interface. `Ethernet`, `WiFi` and
  `EthWiFiFallback` keep their NTP code behind `NTP_PROFILE_ENABLED`, so they
  build with NTP left out. `FallbackTestRunner` does not, and now refuses such a
  build with a message: its NTP sync is not a feature being demonstrated but half
  the measurement — a sync against an FQDN is what proves the route and the
  resolver survived a handoff, which the event column cannot see.

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
