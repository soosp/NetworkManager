# NetworkManager — API Reference

Priority-based network connection manager for ESP32, ESP8266 and AVR. This
document describes the public API. For an overview and quick start, see
[README.md](README.md).

## Table of Contents

- [Compile-time macros](#compile-time-macros)
- [NetworkManagerClass](#networkmanagerclass)
  - [Setup & lifecycle](#setup--lifecycle)
  - [Status accessors](#status-accessors)
  - [JSON status](#json-status)
  - [Time & NTP](#time--ntp)
  - [Applying a profile at runtime](#applying-a-profile-at-runtime)
- [Event enum](#event-enum)
- [NetworkStatus struct](#networkstatus-struct)
- [NetworkAdapter (base class)](#networkadapter-base-class)
  - [State enum](#state-enum)
  - [Getters](#getters)
  - [Re-apply profiles](#re-apply-profiles)
- [Adapters](#adapters)
  - [WiFi adapters — TX power & RSSI](#wifi-adapters--tx-power--rssi)
  - [Ethernet adapters](#ethernet-adapters)
  - [Adapter alias headers](#adapter-alias-headers)
- [NetworkManagerNtpSyncHook.h](#networkmanagerntpsynchookh)
- [Platform notes](#platform-notes)

---

## Compile-time macros

Define these **before** including any library header (in your sketch, or via
`platformio.ini` `build_flags`). They set fixed buffer sizes, so they must be
**identical in every translation unit** — in a multi-file project, define them
globally (PlatformIO `build_flags`; on ESP8266 a `<sketch>.ino.globals.h`; on
ESP32 a `build_opt.h`). See
[Where to define these](README.md#where-to-define-these) for the per-toolchain
details.

|Macro|Default|Description|
|--|--:|--|
|`NETWORK_MANAGER_MAX_ADAPTERS`|`4`|Maximum number of adapters (valid range 1–8).|
|`NETWORK_MANAGER_RECONNECT_TIMEOUT`|`60000`|Milliseconds with nothing connected before `DISCONNECTED` is emitted. `0` disables it.|
|`NETWORK_MANAGER_MUTEX_TIMEOUT`|`1000`|Mutex acquisition timeout, ms (ESP32 only; ESP8266/AVR use no lock).|
|`NETWORK_MANAGER_DEFAULT_NTP_SYNC_INTERVAL`|`3600000`|Default SNTP poll interval, ms (1 h). Overridable at runtime with `setNtpSyncInterval()`.|
|`NETWORK_MANAGER_DEFAULT_NTP_RETRY_INTERVAL`|`30000`|AVR only: base retry delay after a failed sync (server already resolved), ms; doubles per failure up to `_MAX`. No effect on ESP.|
|`NETWORK_MANAGER_DEFAULT_NTP_RETRY_MAX`|`300000`|AVR only: cap for the retry backoff, ms.|
|`NETWORK_ADAPTER_RETRY_INTERVAL`|`15000`|Minimum interval between probe attempts on a failed adapter, ms.|

Adapter-specific macros (define before including the adapter header):

|Macro|Default|Applies to|
|--|--:|--|
|`ESP32_WIFI_ADAPTER_DHCP_TIMEOUT`|`15000`|ESP32 WiFi|
|`ESP8266_WIFI_ADAPTER_DHCP_TIMEOUT`|`15000`|ESP8266 WiFi|
|`ESP8266_ETH_ADAPTER_DHCP_TIMEOUT`|`15000`|ESP8266 Ethernet|
|`ESP8266_ETH_ADAPTER_SPI_FREQUENCY`|`4000000`|ESP8266 Ethernet|
|`AVR_ETH_ADAPTER_DHCP_TIMEOUT`|`15000`|AVR Ethernet|
|`AVR_ETH_ADAPTER_LINK_TIMEOUT`|`5000`|AVR Ethernet|
|`AVR_ETH_ADAPTER_POLL_INTERVAL`|`500`|AVR Ethernet|
|`AVR_ETH_ADAPTER_DHCP_BEGIN_TIMEOUT`|`8000`|AVR Ethernet|
|`AVR_ETH_ADAPTER_DHCP_BEGIN_RESPONSE`|`4000`|AVR Ethernet|
|`ETH_PHY_TYPE`|— (required)|ESP8266 Ethernet — `ETH_PHY_W5500` / `ETH_PHY_W5100` / `ETH_PHY_ENC_28J60`|
|`ETH_PHY_CS`|— (required)|ESP8266 & AVR Ethernet — SPI chip-select pin|

On ESP32 see the documentation of your board and ESP32 Arcuino Core about using
`ETH_PHY_*` macros. If your board is supported by the ESP32 Arduino core (e.g.
Olimex ESP32-POE, ESP32-Gateway etc.), select it in 'Tools -> Board' menu in
Arduino IDE and the ETH_PHY_* definitions are provided automatically.

NTP buffer sizing is inherited from NetworkProfile / Host
(`NETWORK_PROFILE_NTP_SERVER_COUNT`, `HOST_FQDN_LEN`, …); see
[Memory & fine-tuning](README.md#memory--fine-tuning) in the README.

---

## NetworkManagerClass

`NetworkManager` is a singleton; use the global instance `NetworkManager`
(there is no need to construct one). All calls below must be made from the same
task that calls `update()` — normally the Arduino `loop()`.

### Setup & lifecycle

#### `bool addAdapter(NetworkAdapter& adapter)`

Registers an adapter. Adapters are ordered by their profile `priority` (lower
number = higher priority; ties broken by registration order). Returns `false`
if the adapter table is full (`NETWORK_MANAGER_MAX_ADAPTERS`). Call before
`begin()`.

#### `bool setHostname(const char* name)`

Sets a device-level hostname on the profile of every adapter added so far, so one
identity applies to whichever interface serves. Call **after** `addAdapter()` (the
manager holds no hostname, so a later-added adapter keeps its default until set).
`""` resets each adapter to its generated default. A MAC is per-interface, so
there is deliberately no `setMac()` here — set it per profile.

#### `void onEvent(EventCb cb)`

Registers the connection-event callback: `void cb(NetworkManagerClass::Event, NetworkAdapter&)`.
The callback is invoked from the `update()` task (see
[Execution model](#execution-model)), so it is safe to call any
`NetworkManager` accessor — including `getStatus()` — from inside it.

At event `DISCONNECTED` the adapter passed to the callback is the interface that
was last serving before the outage (the primary adapter if none ever served).

#### `void onNtpSync(NtpSyncCb cb)`

Registers the NTP-sync callback: `void cb()`. Invoked once the SNTP client
completes a genuine time sync (not an RTC/manual `settimeofday`). ESP32/ESP8266
only.

#### `bool begin()`

Starts the manager: sorts adapters by priority and probes the highest-priority
one. Returns `false` if no adapters were added.

#### `void end()`

Stops all adapters and resets internal state.

#### `void update()`

Drives the state machine: delivers queued events, applies deferred stops/starts,
runs the fallback/restore/reconnect decision, and polls poll-driven adapters.
**Call every loop iteration** from a single task.

### Status accessors

All status accessors read a cached snapshot of the **active** adapter; they make
no hardware calls and hold no lock across hardware. When nothing is connected
they return `INADDR_ANY` (`0.0.0.0`) / an empty snapshot.

#### `bool isConnected() const`

`true` if any adapter currently holds an IP.

#### `NetworkAdapter* getActiveAdapter() const`

The currently serving adapter, or `nullptr` if none.

#### `IPAddress getLocalIP() const`

Active adapter's IP address.

#### `NetworkStatus getStatus() const`

A consistent [snapshot](#networkstatus-struct) of the active adapter
(interface type, connected flag, IP, mask, gateway, DNS). Captured at the moment
the adapter acquires/renews an address.

#### `IPAddress getSubnetMask() const`

#### `IPAddress getGatewayIP() const`

#### `IPAddress getDns(uint8_t i = 0) const`

Individual fields of the active snapshot. `i` is clamped to
`[0, NETWORK_PROFILE_DNS_SERVER_COUNT)`.

#### `bool getHostname(char* buf, size_t len, NetworkProfile::ConfigSource source = ConfigSource::ACTIVE) const`

#### `bool getMac(NetworkProfile::MACAddress mac, NetworkProfile::ConfigSource source = ConfigSource::ACTIVE) const`

Hostname / MAC of the serving interface — the highest-priority connected adapter,
or the first adapter if none is connected. `ACTIVE` returns the effective value
(user override if set, else the generated default); `FACTORY` the generated
default. Return `false` if no adapter has been added.

### JSON status

#### `size_t statusToJson(char* out, size_t len, bool includeNtp = false) const`

Serialises the active status as a compact JSON object into `out` (no heap, no
`String`). Size the buffer with `NetworkManager.STATUS_JSON_SIZE`. Returns the
number of characters written (excluding NUL), or `0` on error/overflow (in which
case `out` is set empty).

Always contains `interface`, `connected`, `ip`, `mask`, `gw`, and a `dns` array
(populated slots only). With `includeNtp`, an `ntp` object is appended holding
the overall `synced` flag and a `servers` array of the live SNTP servers (name +
resolved/numeric ip). A DHCP-provided server shows an empty name.

```json
{"interface":"eth","connected":true,"ip":"172.20.11.78","mask":"255.255.0.0",
 "gw":"172.20.0.1","dns":["172.20.0.121","172.20.0.122"],
 "ntp":{"synced":true,"servers":[{"name":"pool.ntp.org","ip":"162.159.200.1"}]}}
```

Constants: `STATUS_JSON_LEN` (max length excl. NUL) and `STATUS_JSON_SIZE`
(`= LEN + 1`).

### Time & NTP

The library exposes NTP information at two layers: **configured** (what you set,
from the profile) and **active** (what the SNTP client is actually using, read
live). See [README](README.md#ntp-two-views) for the model.

#### `uint32_t getEpoch() const`

Current Unix time in seconds, or `0` if not yet synced.

#### `bool isTimeValid() const`

`true` once at least one successful SNTP sync has occurred.

#### `bool getNtp(uint8_t i, char* out, size_t len) const`

**Configured** NTP server `i` of the active adapter's profile, as a string.
Returns `false` if not connected or on truncation. (NTP builds only.)

#### `bool getActiveNtpName(uint8_t i, char* out, size_t len) const`

**Active** SNTP server name for slot `i`, read live from the SNTP client.
Non-empty for host-name servers (e.g. `pool.ntp.org`); empty for numeric /
DHCP-provided servers. Returns `false` when empty.

#### `IPAddress getActiveNtpIP(uint8_t i) const`

**Active** SNTP server address for slot `i`: the resolved address of a host-name
server (which can change as a pool rotates) or a DHCP/numeric server. Returns
`0.0.0.0` if the slot is unset or a host name has not been resolved yet — the
resolved IP typically appears from the first successful sync onward.

#### `void setNtpSyncInterval(uint32_t ms)`

Sets the SNTP poll interval at runtime (clamped to a 15 s floor). **ESP8266
requires the [NtpSyncHook](#networkmanagerntpsynchookh)** for this to reach the
lwIP SNTP stack; ESP32 and AVR apply it natively.

#### `uint32_t getNtpSyncInterval() const`

The configured poll interval, ms.

### Applying a profile at runtime

#### `bool applyProfile(NetworkAdapter& adapter)`

Stops and restarts `adapter` with its (possibly updated) profile. Must be called
from the `update()` task. From another task (e.g. a web-server handler), call
`adapter.requestApply()` instead — `update()` will apply it on the next
iteration.

```cpp
// From any task:
wifiProfile.setConfig(newCfg);   // profiles are thread-safe
wifiAdapter.requestApply();      // applied by update() on the manager task
```

---

## Event enum

`NetworkManagerClass::Event`:

|Value|Meaning|
|-------|---------|
|`CONNECTED`|An adapter obtained an IP with nothing serving before it — the first connection at boot, or the first after a `DISCONNECTED`.|
|`FALLBACK`|The active adapter failed; a lower-priority adapter took over.|
|`RESTORED`|A higher-priority adapter recovered and took over from the fallback.|
|`DISCONNECTED`|Nothing connected for `RECONNECT_TIMEOUT`, including a boot where no interface ever came up. Emitted at most once per outage. A working fallback does **not** trigger this.|
|`RECONNECTING`|Reserved; not currently emitted.|

A link outage shorter than `RECONNECT_TIMEOUT` produces **no event**: if a
higher-priority adapter briefly drops and recovers before the timeout, the
decision core still considers it the serving adapter (no `DISCONNECTED`, hence no
subsequent `CONNECTED`). `RESTORED` requires a prior fallback (a second adapter),
and a fresh `CONNECTED` means nothing was serving beforehand — either at boot, or
after a `DISCONNECTED`. Application events are therefore deliberately coarse;
components needing finer link-state tracking (e.g. the AVR SNTP client) watch
the live link independently.

---

## NetworkStatus struct

Returned by `NetworkManager::getStatus()` and `NetworkAdapter::getStatus()`.
Plain value type (copyable); holds only live netif properties — **NTP is not
included** (read it via the NTP accessors instead).

```cpp
struct NetworkStatus {
    NetworkProfile::InterfaceType interfaceType;   // ETH / WIFI
    bool      connected;                           // true while an IP is held
    IPAddress localIP;                             // 0.0.0.0 if not connected
    IPAddress subnetMask;
    IPAddress gateway;
    IPAddress dns[NETWORK_PROFILE_DNS_SERVER_COUNT];
};
```

---

## NetworkAdapter (base class)

Adapters are normally driven by the manager; application code interacts with the
adapter object mainly for the accessors below (and, for WiFi, TX power / RSSI).

### State enum

#### `enum class State { IDLE, CONNECTING, CONNECTED, FAILED }`

### Getters

#### `State getState() const`

Current lifecycle state.

#### `NetworkStatus getStatus() const`

#### `IPAddress getSubnetMask() const`

#### `IPAddress getGatewayIP() const`

#### `IPAddress getDns(uint8_t i = 0) const`

#### `IPAddress getLocalIP() const`

Same snapshot semantics as the manager accessors, but for **this** adapter
specifically (whether or not it is the active one).

#### `const NetworkProfile& getProfile() const`

The associated profile.

#### `bool getHostname(char* buf, size_t len, NetworkProfile::ConfigSource source = ConfigSource::ACTIVE) const`

#### `bool getMac(NetworkProfile::MACAddress mac, NetworkProfile::ConfigSource source = ConfigSource::ACTIVE) const`

Hostname / MAC of this interface (delegates to its profile). `ACTIVE` returns the
effective value (override if set, else the generated default); `FACTORY` the
generated default. Available before the interface connects — configuration, not
runtime state.

### Re-apply profiles

#### `void requestApply()`

Thread-safe request to re-apply this adapter's profile; honoured by the
manager's `update()`. See [applyProfile](#applying-a-profile-at-runtime).

> `start()`, `stop()`, `update()` and `canProbe()` are the manager's
> orchestration interface — do not call them from application code.

---

## Adapters

Each platform provides concrete adapters. Construct them with a matching
profile and register with `addAdapter()`:

```cpp
WiFiProfile      wifiProfile;
ESP32WiFiAdapter wifiAdapter(wifiProfile);
```

|Adapter|Header|Interface|
|---------|--------|-----------|
|`ESP32WiFiAdapter`|`ESP32WiFiAdapter.h`|ESP32 WiFi STA|
|`ESP32EthAdapter`|`ESP32EthAdapter.h`|ESP32 Ethernet (SPI PHY)|
|`ESP8266WiFiAdapter`|`ESP8266WiFiAdapter.h`|ESP8266 WiFi STA|
|`ESP8266EthAdapter`|`ESP8266EthAdapter.h`|ESP8266 wired lwIP (W5500 / W5100 / ENC28J60)|
|`AVREthernetAdapter`|`AVREthernetAdapter.h`|AVR Ethernet (W5100 / W5500)|

### WiFi adapters — TX power & RSSI

Available on `ESP32WiFiAdapter` and `ESP8266WiFiAdapter` (identical signatures):

#### `bool setTxPower(float dBm)`

Sets the transmit power at runtime. Writes through to the profile (validated and
lock-protected) and applies it immediately, so it also survives reconnects
(`start()` re-applies the profile value). Persist it across reboots by saving the
profile (`saveCfg()`). Returns `false` if `dBm` is out of range (nothing
changes). Call from the `loop()` task, or — for a preemptive async web handler
on ESP32 — defer the call to the loop, or rely on the profile lock.

#### `float getTxPower() const`

The **configured** TX power in dBm (from the profile), or `NaN` if the hardware
default is in effect. (ESP32's quantised hardware value is available via
`WiFi.getTxPower()`.)

#### `int getRssi() const`

Current RSSI in dBm from `WiFi.RSSI()`. Only meaningful while connected.

```cpp
wifiAdapter.setTxPower(8.5f);
int rssi = wifiAdapter.getRssi();   // e.g. -63
```

### Ethernet adapters

**ESP8266 (`ESP8266EthAdapter`)** — select the PHY at compile time; the lwIP
wired drivers are distinct types, so this cannot be a runtime value:

```cpp
#define ETH_PHY_TYPE ETH_PHY_W5500
#define ETH_PHY_CS   16
#include "ESP8266EthAdapter.h"
```

**AVR (`AVREthernetAdapter`)** — set the CS pin:

```cpp
#define ETH_PHY_CS 10
#include "AVREthernetAdapter.h"
```

See [Platform notes](#platform-notes) for the W5100 link-detection limitation.

### Adapter alias headers

`EthAdapter.h` and `WiFiAdapter.h` resolve to the correct platform adapter, so a
sketch can be written once and compiled for any target:

```cpp
#include <EthAdapter.h>    // -> ESP32EthAdapter / ESP8266EthAdapter / AVREthernetAdapter
#include <WiFiAdapter.h>   // -> ESP32WiFiAdapter / ESP8266WiFiAdapter (ESP only)

EthAdapter  ethAdapter(ethProfile);
WiFiAdapter wifiAdapter(wifiProfile);
```

---

## NetworkManagerNtpSyncHook.h

An **opt-in** header that makes `setNtpSyncInterval()` effective on **ESP8266**.
The lwIP2 SNTP stack has no runtime API for the poll interval; it calls a weak
function that must be overridden by a strong definition. Because a strong free
function cannot live in a header-only, multi-TU library without ODR clashes, the
override is provided here for you to include in **exactly one** translation unit
(normally your `.ino`):

```cpp
#include "NetworkManager.h"
#include "NetworkManagerNtpSyncHook.h"   // ONE .cpp/.ino only
```

Including it in more than one TU is a deliberate multiple-definition link error.
On ESP32 and AVR the header is a no-op (they set the interval natively), and
after inclusion `NETWORK_MANAGER_NTP_SYNC_INTERVAL_SETTER` is defined so code can
detect it at compile time.

---

## Platform notes

### Execution model

`update()` and all other `NetworkManager` calls must originate from a single
task (the Arduino `loop()`, or a dedicated network task). On ESP32/ESP8266,
SDK network events arrive on a separate context; the adapters translate those
into state changes and **queue** the resulting user events, which `update()`
then delivers on the loop task. This means your `onEvent` / `onNtpSync`
callbacks — and anything they call, such as `getStatus()` or `Serial.print` —
run on the loop task with a full stack, never on the small SDK event-task stack.

Thread-safety summary:

- Adapter state and cross-task signals are `std::atomic` on ESP32/ESP8266,
  plain on AVR.
- The fallback/restore decision core is serialised by one mutex (ESP32);
  ESP8266/AVR are single-context and need no lock.
- No lock is ever held across `start()`, `stop()`, or a user callback.
- Profiles are independently thread-safe and may be read/written from any task.

### Interface teardown: DNS and the default route (ESP32)

lwIP's resolver table is global rather than per-netif, and a stopped adapter keeps
its netif — it is never destroyed, to avoid SPI re-initialisation errors. Two
things therefore survive a fallback or restore in a broken state, and the manager
repairs both after each teardown:

- **DNS.** The teardown clears the global resolver, wiping the servers of the
  interface that is now serving. The manager re-installs them from the serving
  adapter's cached status. Without this, resolution works only until the DNS
  cache expires; an FQDN NTP server then never resolves again.

- **Default route.** esp_netif re-elects the default netif on IP events only.
  With DHCP, stopping the adapter emits LOST_IP and the re-election happens by
  itself; with a **static address there is no DHCP client and no LOST_IP**, so
  the default route can stay on the interface that was just taken out of service
  — the serving interface has an IP and working DNS, yet no packet leaves the
  device. The manager points the default route at the serving adapter
  (NetworkAdapter::setDefaultRoute()).

Sketches need no action for either.

### ESP32: IPC-task stack overflow when registering the W5500 interrupt

With ETH_PHY_IRQ set, the core's SPI-Ethernet driver installs the GPIO ISR service
during the first ETH.begin(). The IDF runs that registration on the IPC task, whose
stack is small; the allocation inside esp_intr_alloc() leaves little headroom,
and an interrupt arriving at that moment saves its register frame on top —
occasionally tripping the guard, which appears as an intermittent "Stack canary
watchpoint triggered (ipc1)" panic. It happens inside the SDK, before any
library code runs. Only the FIRST ETH.begin() takes this path (the ISR service
is global and installed once), so it is a boot-time phenomenon — in a
WiFi-primary setup, only on boots where WiFi is unavailable and Ethernet is
therefore started. Lowering the Core Debug Level gives enough headroom in
practice; leaving ETH_PHY_IRQ unset (polling mode) removes the cause entirely.

### ESP32: "invalid static ip" logged on Ethernet re-arm (harmless)

stop() stops the DHCP client so that a background lease renewal cannot
overwrite the serving interface's DNS and gateway. When the link later returns,
the core's own ETH_CONNECTED handler runs first, sees a stopped DHCP client,
assumes a static address, finds none, and logs
`E esp_netif_handlers: invalid static ip`. The adapter's start() then restarts
the DHCP client and the address is obtained normally. The message is cosmetic
and appears once per Ethernet re-arm on DHCP profiles.

### ESP8266 static configuration: route and link by hand

On ESP8266 the wired driver is LwipIntfDev, which installs the default route
only on the DHCP bind path and, on stop(), leaves the netif in place but takes
its link down. For a **static** address the adapter therefore does two things
LwipIntfDev does not: after setDefault() it installs the gateway and default
route by hand (`netif_set_gw` + `netif_set_default`), and on re-arm it brings
the netif link back up. Without these a static ESP8266 interface resolves
on-link names but cannot reach any off-link host (e.g. a public NTP server),
and loses connectivity entirely after a link flap. **DHCP** configurations are
unaffected — LwipIntfDev handles their route and link. ESP32 and AVR install
static routes natively and need none of this.

### NTP resynchronisation on reconnect

ESP8266 resynchronises NTP on its own SNTP poll cycle after a re-arm, not
immediately; ESP32 and AVR trigger an immediate resync on reconnect.

### AVR: begin() blocks during DHCP without a link

The Arduino Ethernet library's Ethernet.begin(mac) is synchronous and waits out
its internal DHCP timeout when no lease can be obtained — so a cold boot with no
link stalls in begin() (~60-80 s by default) before the manager ever sees the
adapter fail, delaying the first DISCONNECTED accordingly. The AVR adapter caps
this with the timeout overload (AVR_ETH_ADAPTER_DHCP_BEGIN_TIMEOUT). A static
address is unaffected — begin() returns immediately. ESP32/ESP8266 do not block
in begin() at all.

### W5100 has no link detection

Cable-driven fallback requires a controller with hardware link detection —
**W5500** (or ENC28J60 on ESP8266). On **W5100** the driver reports the link as
always up (`isLinkDetectable()` is false; `isLinked()` is hardcoded true), so:

- `connected()` depends only on holding an IP; a physical cable unplug is
  invisible.
- With a **static IP**, the interface never reports failure → fallback does not
  trigger.
- With **DHCP**, failure is seen only when the lease is eventually lost (slow),
  not on unplug.

W5100 is fine for single-interface use, but not for cable-driven ETH↔WiFi
fallback.

### AVR SNTP is link-gated

The AVR SNTP client polls only while an adapter is connected. On a controller
with link detection (**W5500**) it pauses within one `update()` tick of a link
loss — faster than the `RECONNECT_TIMEOUT` event — and resumes with a fresh sync
(re-resolve, backoff reset) when the link returns. On **W5100** (no link
detection) it keeps trying: an accepted compromise for that chip.

### DHCP-provided NTP: one server

DHCP option 42 capture is enabled, but the platform lwIP build caps the number
of NTP servers taken from DHCP at **one** (ESP8266: `LWIP_DHCP_MAX_NTP_SERVERS`,
wired into the precompiled lwIP2; ESP32: `CONFIG_LWIP_DHCP_MAX_NTP_SERVERS`, IDF
default 1). Manually configured NTP servers are not limited this way — all
`NETWORK_PROFILE_NTP_SERVER_COUNT` slots are used.

---

## License

MIT — see [LICENSE](LICENSE).
