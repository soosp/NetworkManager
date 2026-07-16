# NetworkManager

An Arduino library for priority-based network connection management on ESP32,
ESP8266, and AVR. It handles connection, fallback, reconnection, live status,
and NTP synchronisation across multiple interfaces with a small, event-driven,
heap-free API.

Full API details are in [API.md](API.md).

## Architecture

```txt
NetworkManager          (singleton — connection orchestration)
├── NetworkManagerCore  (pure decision logic — host-testable)
├── EthAdapter          (platform adapter — hardware lifecycle)
│   └── EthProfile      (configuration — IP, DNS, NTP, priority, MAC)
└── WiFiAdapter
    └── WiFiProfile
```

**Profile** ([NetworkProfile](https://github.com/soosp/NetworkProfile)) is a
pure, thread-safe data class holding one interface's configuration (IP, DNS,
NTP, hostname, MAC, priority, TX power). It makes no hardware calls and can be
read or written at any time, including while the interface is active.

**Adapter** wraps a platform interface (WiFi, Ethernet) and owns its lifecycle
(`start()`, `stop()`, `update()`), translating profile settings into platform
API calls. Five adapters are provided; custom ones subclass `NetworkAdapter`.

**NetworkManagerCore** is the fallback / restore / reconnect decision logic as a
pure C++ class with no hardware or threading dependencies — it can be compiled
and tested on a host PC.

**NetworkManager** orchestrates the adapters: it sorts them by priority, starts
the highest-priority one, monitors state, and drives fallback, reconnect and
restore. Application code interacts only with `NetworkManager` after setup.

## Features

- **Priority-based fallback** — the highest-priority connected adapter is
  always preferred
- **Automatic reconnect** — failed connections are retried; `DISCONNECTED` is
  emitted only on a genuine total outage (including a boot where nothing ever
  connects), once per outage
- **Higher-priority restore** — a recovered primary interface is restored
  automatically
- **Event callbacks** — `CONNECTED`, `FALLBACK`, `RESTORED`, `DISCONNECTED`,
  delivered on the loop task
- **Live status snapshot** — `getStatus()` / individual getters /
 `statusToJson()` for a web or MQTT payload
- **NTP** — sync callback, configured vs. live-server accessors, runtime poll
  interval, and automatic DHCP option 42
- **Runtime WiFi control** — `setTxPower()` / `getTxPower()` / `getRssi()`
- **No heap, no `String`** — all state is fixed-size
- **Compile-time tuning** — adapter count, timeouts, and buffer sizes via
  macros

## Supported platforms and adapters

|Platform|Adapter|Interface|
|---|---|---|
|ESP32|`ESP32WiFiAdapter`|WiFi STA|
|ESP32|`ESP32EthAdapter`|Ethernet (SPI PHY)|
|ESP8266|`ESP8266WiFiAdapter`|WiFi STA|
|ESP8266|`ESP8266EthAdapter`|wired lwIP (W5500 / W5100 / ENC28J60)|
|AVR|`AVREthernetAdapter`|Ethernet (W5100 / W5500)|

The alias headers `EthAdapter.h` and `WiFiAdapter.h` resolve to the correct
adapter for the target, so a fallback sketch can be written once and built for
ESP32 or ESP8266 unchanged.

## Dependencies

- [soosp/NetworkProfile](https://github.com/soosp/NetworkProfile) — interface
  configuration and configuration persistence
- [soosp/Host](https://github.com/soosp/Host) — IP/FQDN validators and the
  FQDN-length policy for NTP-server strings
- [arduino-libraries/Ethernet](https://github.com/arduino-libraries/ethernet) —
  **AVR only**; Ethernet library (> 2.0)

## Quick Start

```cpp
#include <WiFiAdapter.h>
#include <NetworkManager.h>

WiFiProfile wifiProfile;
WiFiAdapter wifiAdapter(wifiProfile);

void onNetworkEvent(NetworkManagerClass::Event event, NetworkAdapter&) {
    if (event == NetworkManagerClass::Event::CONNECTED) {
        NetworkStatus s = NetworkManager.getStatus();
        Serial.print("Connected — IP: ");
        Serial.println(s.localIP);
    } else if (event == NetworkManagerClass::Event::DISCONNECTED) {
        Serial.println("Disconnected");
    }
}

void onNtpSync() {
    Serial.print("NTP synced — epoch ");
    Serial.println(NetworkManager.getEpoch());
}

void setup() {
    Serial.begin(115200);

    WiFiProfile::WiFiConfig cfg;
    cfg.dhcp     = true;
    cfg.priority = 0;
    // MAX_SSID_LEN is a macro (collides with the ESP32 SDK's) — use it
    // unqualified, or copy with sizeof(dest)-1.
    strncpy(cfg.ssid,     "MyNetwork",    MAX_SSID_LEN);
    strncpy(cfg.password, "MyPassword",   WiFiProfile::MAX_PASSWORD_LEN);
    strncpy(cfg.ntp[0],   "pool.ntp.org", Host::MAX_FQDN_LEN);
    wifiProfile.setConfig(cfg);

    NetworkManager.onEvent(onNetworkEvent);
    NetworkManager.onNtpSync(onNtpSync);
    NetworkManager.addAdapter(wifiAdapter);
    NetworkManager.begin();
}

void loop() {
    NetworkManager.update();
    delay(10);
}
```

See the `examples/` directory for ETH-only, WiFi-only, ETH+WiFi fallback, and
AVR sketches.

## ETH → WiFi fallback

Lower priority number = higher priority. When the primary fails, the manager
starts the next adapter and emits `FALLBACK`; when the primary recovers it is
restored and the fallback stopped (`RESTORED`).

```cpp
ethCfg.priority  = 0;   // primary
wifiCfg.priority = 1;   // fallback

NetworkManager.addAdapter(ethAdapter);
NetworkManager.addAdapter(wifiAdapter);
NetworkManager.begin();
```

If two adapters share a priority value, the tie is broken by registration order.
The restore probe calls `start()` on the primary while the fallback is active,
so this feature is intended for **independent** hardware interfaces (ETH + WiFi),
not two adapters that share one radio.

> **Cable-driven fallback needs hardware link detection (W5500 or ENC28J60).**
> On **W5100** the driver reports the link as unknown, so a physical unplug is
> invisible: with a static IP the interface never reports failure (no fallback),
> and with DHCP failure is only seen when the lease is eventually lost. W5100 is
> fine for single-interface use.

## Status

`getStatus()` returns a consistent snapshot of the active interface; individual
getters and a JSON serialiser are also provided. All read a cached snapshot — no
hardware calls, safe from a callback.

```cpp
NetworkStatus s = NetworkManager.getStatus();     // interfaceType, connected, localIP, subnetMask, gateway, dns[]
IPAddress gw    = NetworkManager.getGatewayIP();

char js[NetworkManager.STATUS_JSON_SIZE];
if (NetworkManager.statusToJson(js, sizeof(js), /*includeNtp=*/true))
    server.send(200, "application/json", js);      // or mqtt.publish(topic, js)
```

`statusToJson()` always emits `interface`, `connected`, `ip`, `mask`, `gw`, and
a `dns` array; with `includeNtp` it appends an `ntp` object (`synced` + a
`servers` array). See [API.md](API.md#json-status).

## NTP (two views)

NTP information is available at two layers:

- **Configured** — what you set in the profile: `getNtp(i, buf, len)`.
- **Active** — what the SNTP client is actually using, read live:
  `getActiveNtpName(i, buf, len)` and `getActiveNtpIP(i)`.

The active view also reflects DHCP-provided servers and the address a pool name
currently resolves to. `onNtpSync()` fires on the first genuine sync;
`isTimeValid()` / `getEpoch()` report the time state.

`setNtpSyncInterval(ms)` changes the poll cadence at runtime. On ESP32/AVR this
is native; **on ESP8266 you must include `NetworkManagerNtpSyncHook.h` in exactly
one translation unit** (normally your `.ino`) for it to take effect — see
[API.md](API.md#networkmanagerntpsynchookh).

> **DHCP provides at most one NTP server** on both platforms (an lwIP build
> limit). Manually configured servers use all `NETWORK_PROFILE_NTP_SERVER_COUNT`
> slots.

**AVR NTP resolves synchronously.** With a host-name NTP server, the first sync
after each connect performs a blocking DNS lookup inside `update()`, briefly
stalling `loop()` (bounded by the Ethernet DNS timeout; the result is cached
until the adapter changes). A **literal IP** NTP server skips DNS entirely and
never blocks — the recommended choice on AVR.

**AVR NTP retries with backoff and follows the link.** Once the server is
resolved, a failed sync is retried after `NETWORK_MANAGER_DEFAULT_NTP_RETRY_INTERVAL`,
doubling on each failure up to `_RETRY_MAX` and resetting on success; a
*resolution* failure instead waits the full sync interval. The client also
tracks the live link — on a chip with link detection (W5500) it pauses within
one `update()` tick of a link loss and resumes with a fresh sync when the link
returns (W5100 has no detection, so it keeps trying). On ESP, retry and backoff
are handled by lwIP.

## Runtime WiFi control

On the WiFi adapters (`ESP32WiFiAdapter` / `ESP8266WiFiAdapter`):

```cpp
wifiAdapter.setTxPower(8.5f);        // dBm; validated, persists across reconnects
float p = wifiAdapter.getTxPower();  // configured value
int   r = wifiAdapter.getRssi();     // live RSSI, e.g. -63
```

`setTxPower()` writes through to the profile (validated, lock-protected), so it
survives reconnects; save the profile to persist it across reboots. See
[API.md](API.md#wifi-adapters--tx-power--rssi).

## Configuration

Define macros **before** including any library header (or in `platformio.ini`
`build_flags`). Common ones:

|Macro|Default|Description|
|--|--:|--|
|`NETWORK_MANAGER_MAX_ADAPTERS`|`4`|Maximum adapters (1–8).|
|`NETWORK_MANAGER_RECONNECT_TIMEOUT`|`60000`|ms before `DISCONNECTED`; 0 disables.|
|`NETWORK_MANAGER_MUTEX_TIMEOUT`|`1000`|Mutex timeout, ms (ESP32 only).|
|`NETWORK_MANAGER_DEFAULT_NTP_SYNC_INTERVAL`|`3600000`|Default SNTP interval, ms.|
|`NETWORK_MANAGER_DEFAULT_NTP_RETRY_INTERVAL`|`30000`|AVR only: base retry delay after a failed sync (server already resolved), ms; doubles per failure up to `_MAX`. No effect on ESP.|
|`NETWORK_MANAGER_DEFAULT_NTP_RETRY_MAX`|`300000`|AVR only: cap for the retry backoff, ms.|
|`NETWORK_ADAPTER_RETRY_INTERVAL`|`15000`|Probe retry interval, ms.|

The full list, including adapter-specific and PHY-selection macros, is in
[API.md](API.md#compile-time-macros).

### Where to define these

These macros set **fixed buffer sizes** (e.g. `NetworkStatus`, the profile's NTP
buffers), so **every translation unit that includes a library header must see the
same values**. In a single-file sketch, define them before the first `#include`.
In a multi-file project they must be defined *globally* — otherwise the differing
struct layouts across `.cpp` files are an ODR violation (undefined behaviour).

- **PlatformIO** (all platforms) — in `platformio.ini`:

  ```ini
  build_flags =
      -D HOST_FQDN_LEN=32
      -D NETWORK_PROFILE_NTP_SERVER_COUNT=1
  ```

- **Arduino IDE, ESP8266** — add a file named `<SketchName>.ino.globals.h` next
  to your `.ino`; the ESP8266 core force-includes it into every source file:

  ```cpp
  // MySketch.ino.globals.h
  #define HOST_FQDN_LEN                    32
  #define NETWORK_PROFILE_NTP_SERVER_COUNT 1
  ```

- **Arduino IDE, ESP32** — add a file named `build_opt.h` in the sketch folder.
  The core passes it to the compiler as a response file for **every** translation
  unit, so it holds **compiler flags**, not `#define`s (despite the `.h` name):

  ```txt
  -DHOST_FQDN_LEN=32
  -DNETWORK_PROFILE_NTP_SERVER_COUNT=1
  ```

- **Arduino IDE, AVR** — there is no per-sketch global mechanism. Either keep the
  sketch to a single `.ino` (define before the first include), or pass the flags
  via `arduino-cli --build-property
  "compiler.cpp.extra_flags=-DHOST_FQDN_LEN=32 …"`.
  For a multi-file AVR project, PlatformIO is the simplest route.

**Important notes:**

1. The ESP8266 `.ino.globals.h` and the ESP32 `build_opt.h` are **different
mechanisms** — ESP32 does not read `.ino.globals.h`, and the ESP8266
`/*@create-file:build.opt@ … @end*/` block has no ESP32 equivalent (put the
raw flags straight into `build_opt.h` instead).
2. Defining a macro before the include in only your `.ino` affects **that file
only** — other `.cpp` files use the defaults. That mismatch is the multi-TU
pitfall the global methods above avoid.

## Memory & fine-tuning

The library never allocates on the heap; every buffer is a fixed size fixed at
compile time. The footprint is driven by a handful of macros inherited from
NetworkProfile / Host — **define them before including any library header** (or
via `platformio.ini` `build_flags`), identically in every translation unit.

|Macro|Default|Controls|Impact|
|--|--:|--|--|
|`HOST_FQDN_LEN`|253|Max NTP-server FQDN length|**Largest lever** — every FQDN buffer is `HOST_FQDN_LEN + 1`, and they appear per profile, in the SNTP setup, and in `statusToJson()`.|
|`NETWORK_PROFILE_NTP_SERVER_COUNT`|0 (AVR) / 3 (ESP)|NTP server slots (`0` = NTP compiled out)|One FQDN buffer per slot, per profile, plus one per slot in the SNTP setup and JSON.|
|`NETWORK_PROFILE_DNS_SERVER_COUNT`|1 (AVR) / 2 (ESP)|DNS slots per profile|4 bytes/slot (minor).|
|`NETWORK_PROFILE_HOSTNAME_LEN`|63|Max host name length|One buffer of this size per profile.|

`NetworkManager.STATUS_JSON_SIZE` (the `statusToJson()` buffer size) is
**derived**: `200 + NETWORK_PROFILE_NTP_SERVER_COUNT * (HOST_FQDN_LEN + 1 + 48)`.

### Where the FQDN buffers go

With the ESP defaults (`HOST_FQDN_LEN = 253` → 254-byte buffers, 3 NTP slots, two
profiles):

- **Persistent** — NTP servers stored in each profile (`254 × 3 × 2 ≈ 1.5 KB`)
  plus the `static` scratch array used when SNTP is (re)configured
  (`254 × 3 ≈ 0.75 KB`): ~2.3 KB spent purely on NTP server names.
- **Transient (stack)** — `statusToJson()` (one FQDN buffer), the `NetworkConfig`
  value read at adapter start, and the profile's `toJson()`.
- **Your buffers** — `STATUS_JSON_SIZE` (≈ 1.1 KB with three NTP slots) and
  `NetworkProfile::JSON_SIZE` are sized for the worst case and shrink with the
  macros above.

The DHCP-provided NTP server count is capped at **one** by the platform lwIP
build, so the multi-slot storage only pays off for manually configured servers.

### Tuning guidance

- **Shorten `HOST_FQDN_LEN` first.** `pool.ntp.org` is 12 chars; a literal IP is
  15. `HOST_FQDN_LEN = 32` (or `64`) cuts every FQDN buffer to roughly an eighth
  of the default with no functional loss.
- **Set `NETWORK_PROFILE_NTP_SERVER_COUNT` to what you use.** `1` is plenty for
  most deployments and removes two thirds of the NTP storage versus the ESP
  default of `3`; `0` compiles NTP out entirely (the AVR default).
- **AVR (ATmega2560, 8 KB SRAM):** keep NTP off unless needed; if you enable it,
  pair it with a small `HOST_FQDN_LEN` — a 253-byte FQDN alone would size a ~1 KB
  `statusToJson()` buffer.
- JSON costs nothing until you declare a buffer for `statusToJson()` / `toJson()`.

```cpp
// Lean configuration — short NTP names or literal IPs, a single server
#define HOST_FQDN_LEN                    32
#define NETWORK_PROFILE_NTP_SERVER_COUNT 1
#define NETWORK_PROFILE_DNS_SERVER_COUNT 1

#include <EthAdapter.h>
#include <WiFiAdapter.h>
#include <NetworkManager.h>
```

```cpp
// AVR without NTP — no FQDN storage at all (the AVR default)
#define NETWORK_PROFILE_NTP_SERVER_COUNT 0
```

The NetworkProfile README's *Memory Optimisation* section has the full
per-field breakdown and the exact savings table.

## Design notes

### Execution model

`update()` and all `NetworkManager` calls must come from a single task (the
Arduino `loop()`, or a dedicated network task). SDK network events arrive on a
separate context; the adapters queue the resulting user events, which `update()`
delivers on the loop task. Your `onEvent` / `onNtpSync` callbacks therefore run
on the loop task with a full stack — it is safe to call `getStatus()`,
`Serial.print`, etc. from inside them.

### Deferred start/stop and events

Adapter `start()` / `stop()` invoke platform APIs that can themselves generate
events or block, so they are never called from an event context: the internal
`_handleStateChange()` records deferred flags **and queues the user event**, and
`update()` executes them on the next iteration. This keeps teardown and arbitrary
app callback code off the small SDK event-task stack.

### Decision core

All fallback / restore / reconnect policy lives in `NetworkManagerCore`, a
platform-independent class with no Arduino, hardware, or threading primitives. It
receives a snapshot of adapter states and returns an intent (which adapter to
start/stop, which event to emit); `NetworkManager` executes those intents. The
`test/` directory runs the core against a host compiler.

### Thread safety

- Adapter state (`_state`) and cross-task signals (`_lastFailedMs`, deferred
  bits, pending-apply) are `std::atomic` on ESP32/ESP8266; plain on AVR.
- The decision core is protected by the single `NetworkManager` mutex (ESP32);
  ESP8266 (cooperative) and AVR (single-threaded) need no lock.
- Callbacks run outside any mutex, on the `update()` task.
- Profiles are independently thread-safe and may be read/written from any task.

## Platform notes

Most platform-specific behaviour is handled internally; the API reference covers
it in full under [Platform notes](API.md#platform-notes). A few points are worth
knowing as a user:

- **ESP32 with a W5500 and `ETH_PHY_IRQ` set** can, on rare boots, hit an SDK
  stack-canary panic (`Stack canary watchpoint triggered (ipc1)`) while the core
  installs the Ethernet interrupt — before any library code runs. Leaving
  `ETH_PHY_IRQ` unset (polling mode) removes the cause; lowering the Core Debug
  Level also helps in practice.
- **ESP32, DHCP Ethernet**: a harmless `esp_netif_handlers: invalid static ip`
  line may be logged once when the Ethernet link returns. It is cosmetic — the
  interface re-acquires its lease normally.
- **AVR, DHCP without a link at boot**: `Ethernet.begin()` blocks for its
  internal DHCP timeout, delaying the first `DISCONNECTED`. The adapter caps this
  with `AVR_ETH_ADAPTER_DHCP_BEGIN_TIMEOUT` (default 8000 ms). A static address
  is unaffected.
- **ESP8266 NTP after a handoff**: resynchronisation follows the SNTP poll cycle
  rather than happening immediately, so a fresh sync can lag a reconnect by one
  interval. ESP32 and AVR resync immediately.

Fallback and restore keep the default route and DNS resolver pointed at the
serving interface on both ESP platforms, including with static addressing — see
the API reference for the mechanism.

## Testing

The fallback/restore behaviour is validated on real hardware with a
semi-automatic 32-case test runner (`examples/FallbackTestRunner`), and the
decision core has a host-side regression suite (`test/nm_harness.cpp`). See the
[test/README.md](test/README.md) for details. Results across ESP32 (W5500 and
LAN8720A), ESP8266 and the test method are documented in
[Testing.md](Testing.md).

## License

MIT — see [LICENSE](LICENSE) for details.
