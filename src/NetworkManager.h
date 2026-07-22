/*
 * Header-only: the Arduino IDE compiles libraries separately from the sketch, so
 * a precompiled unit would not see the configuration macros you define — being
 * header-only, the library is compiled with each includer's macros instead.
 * In a multi-file project, define those macros globally so every translation
 * unit agrees (see the README, "Where to define these").
 */
 
#pragma once

#include "NetworkAdapter.h"
#include "NetworkManagerCore.h"   // pure decision logic (host-testable, no Arduino)

#ifndef NETWORK_MANAGER_MAX_ADAPTERS
#   define NETWORK_MANAGER_MAX_ADAPTERS 2
#endif

#if (NETWORK_MANAGER_MAX_ADAPTERS < 1) || (NETWORK_MANAGER_MAX_ADAPTERS > 8)
#   error "NETWORK_MANAGER_MAX_ADAPTERS must be in the [1, 8] range!"
#endif

#ifndef NETWORK_MANAGER_RECONNECT_TIMEOUT
#   define NETWORK_MANAGER_RECONNECT_TIMEOUT 60000
#endif

#ifndef NETWORK_MANAGER_PROBE_INTERVAL
#   define NETWORK_MANAGER_PROBE_INTERVAL 30000
#endif

#ifndef NETWORK_MANAGER_MUTEX_TIMEOUT
#   define NETWORK_MANAGER_MUTEX_TIMEOUT 1000
#endif

// NTP sync interval in milliseconds. Matches the ESP-IDF default of one hour,
// so on ESP an application that never calls setNtpSyncInterval() sees the same
// cadence as before. The SNTP minimum is 15 s (RFC 4330): lower values are
// clamped up to 15000 on every platform — by ESP-IDF/lwIP on ESP, and by local
// NTP implementation on AVR, both to honour the RFC and to avoid hammering
// public NTP pools.
#ifndef NETWORK_MANAGER_DEFAULT_NTP_SYNC_INTERVAL
#   define NETWORK_MANAGER_DEFAULT_NTP_SYNC_INTERVAL 3600000UL
#endif

// AVR only: after the server address is resolved, a failed sync (no reply, bad
// timestamp, socket error) is retried after this delay instead of the full sync
// interval, doubling on each consecutive failure up to _RETRY_MAX and resetting
// on success. A *resolution* failure is deliberately excluded (AVR DNS is
// blocking), so it keeps waiting the full sync interval. On ESP the SNTP
// retry/backoff is lwIP's own and these macros have no effect.
#ifndef NETWORK_MANAGER_DEFAULT_NTP_RETRY_INTERVAL
#   define NETWORK_MANAGER_DEFAULT_NTP_RETRY_INTERVAL 30000UL
#endif
#ifndef NETWORK_MANAGER_DEFAULT_NTP_RETRY_MAX
#   define NETWORK_MANAGER_DEFAULT_NTP_RETRY_MAX 300000UL
#endif

#ifndef ARDUINO_ARCH_AVR
#   include <atomic>
#endif
#if !defined(ARDUINO_ARCH_AVR) && !defined(ARDUINO_ARCH_ESP8266)
#   include <mutex>
#   include <chrono>
#endif

#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
#   include <lwip/dns.h>         // dns_setserver() — re-assert after teardown
#   include <lwip/ip_addr.h>     // ip_addr_t / IP_ADDR4
#endif


#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
#   if defined(ARDUINO_ARCH_ESP32)
#       include <esp_sntp.h>
#   elif defined(ARDUINO_ARCH_ESP8266)
#       include <coredecls.h>
#       include <lwip/apps/sntp.h>   // sntp_servermode_dhcp() — DHCP option 42 NTP
                                     // (a no-op macro when not compiled into lwip2)
#   elif defined(ARDUINO_ARCH_AVR)
        // AVR has no SDK SNTP stack, so NetworkManager runs a tiny SNTP client
        // itself over UDP. NTP is transport-agnostic, but on AVR the only
        // interface type is Ethernet, so the client assumes EthernetUDP. If a
        // non-Ethernet AVR interface is ever added, this is where a transport
        // seam (e.g. a UDP client obtained from the active adapter) would go.
#       include <Ethernet.h>
#       include <Dns.h>
#       include <string.h>     // memset() for the SNTP request buffer
#   endif
#endif


/**
 * @brief Orchestrates multiple network adapters with priority-based fallback.
 *
 * Manages up to NETWORK_MANAGER_MAX_ADAPTERS adapters. Adapters are tried
 * in ascending priority order (lower value = higher priority). When the
 * active adapter fails, the next one is started automatically. When a
 * higher-priority adapter recovers, it takes over and the lower-priority
 * one is stopped.
 *
 * A single global instance is provided as NetworkManager.
 *
 * Usage:
 * @code
 * #include "NetworkManager.h"
 *
 * // ESP32 Ethernet — board-specific ETH_PHY_* definitions must come first
 * #define ETH_PHY_TYPE  ETH_PHY_LAN8720
 * #define ETH_PHY_ADDR  0
 * #define ETH_PHY_MDC   23
 * #define ETH_PHY_MDIO  18
 * #include <ETH.h>
 * #include "ESP32EthAdapter.h"
 * #include "ESP32WiFiAdapter.h"
 *
 * WiFiProfile      wifiProfile;
 * EthProfile       ethProfile;
 * ESP32WiFiAdapter wifiAdapter(wifiProfile);
 * ESP32EthAdapter  ethAdapter(ethProfile);
 *
 * void setup() {
 *     ethProfile.setPriority(0);
 *     wifiProfile.setPriority(1);
 *     NetworkManager.addAdapter(ethAdapter);
 *     NetworkManager.addAdapter(wifiAdapter);
 *     NetworkManager.onEvent([](NetworkManagerClass::Event e, NetworkAdapter& a) { ... });
 *     NetworkManager.onNtpSync([]() { ... });
 *     NetworkManager.begin();
 * }
 *
 * void loop() {
 *     NetworkManager.update();
 * }
 * @endcode
 *
 * @note On ESP32 Ethernet, ETH.h must be included before ESP32EthAdapter.h
 *       with the appropriate board-specific ETH_PHY_* definitions.
 *       See ESP32EthAdapter.h for details.
 * @note Adapter references must remain valid for the lifetime of the manager.
 * @note Do NOT register sntp_set_time_sync_notification_cb() (ESP32) or
 *       settimeofday_cb() (ESP8266) directly — these are used internally
 *       by NetworkManager for NTP synchronisation detection.
 * @note This class follows the Meyers' Singleton pattern. It is non-copyable,
 *       non-assignable, and provides thread-safe lazy initialization.
 */
class NetworkManagerClass {
public:
    /** @brief Default mutex acquisition timeout in milliseconds. */
    static constexpr uint32_t MUTEX_TIMEOUT = NETWORK_MANAGER_MUTEX_TIMEOUT;

    // -------------------------------------------------------------------------
    // Events
    // -------------------------------------------------------------------------

    /**
    * @brief High-level network events emitted to the application.
    *
    * CONNECTED    — an adapter obtained an IP address (first connection,
    *                a fallback/restore handoff is reported separately below,
    *                or the first connection after a DISCONNECTED reset).
    * DISCONNECTED — nothing has been connected for RECONNECT_TIMEOUT. This
    *                includes a boot where no interface ever came up: with
    *                nothing serving, a boot is just an outage that started at
    *                startup, and it is reported like any other. Emitted at most
    *                once per outage — it does not repeat every timeout while the
    *                outage lasts. Note: a working lower-priority FALLBACK is NOT
    *                reported as DISCONNECTED — only a genuine total outage is.
    *                The adapter passed to the callback is the interface that was
    *                last serving (the primary adapter if none ever served).
    * FALLBACK     — the active adapter failed; a lower-priority adapter
    *                took over and obtained an IP.
    * RESTORED     — a higher-priority adapter recovered and took over
    *                from a lower-priority one.
    * RECONNECTING — RESERVED, not currently emitted. Kept for API stability.
    *                The intuitive wiring ("emit when re-probing with nothing
    *                connected") fires during every ordinary fallback gap too,
    *                which is noise; a meaningful "total-outage retry" signal
    *                needs agreed semantics first. Left here so the enum value
    *                does not shift and existing switch statements keep compiling.
    */
    enum class Event : uint8_t {
        CONNECTED,
        DISCONNECTED,
        FALLBACK,
        RESTORED,
        RECONNECTING,
    };

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    /**
     * @brief Callback type invoked on high-level network events.
     *
     * @param event   The event that occurred.
     * @param adapter The adapter that triggered the event.
     */
    using EventCb = void(*)(Event event, NetworkAdapter& adapter);

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    /**
     * @brief Callback type invoked when NTP time synchronisation completes.
     *
     * Registered via onNtpSync(). On ESP32, the internal SNTP callback is
     * used; on ESP8266, settimeofday_cb() is used. Do not register these
     * platform callbacks directly — see class-level note.
     *
     * @note Never called on AVR platforms.
     */
    using NtpSyncCb = void(*)();
#endif

    // -------------------------------------------------------------------------
    // Singleton
    // -------------------------------------------------------------------------

    // Delete copy constructor and assignment operator to ensure uniqueness
    NetworkManagerClass(const NetworkManagerClass&)            = delete;
    NetworkManagerClass& operator=(const NetworkManagerClass&) = delete;

    /**
     * @brief Returns the single instance of the class (Meyers' Singleton).
     *
     * Uses lazy initialization: the instance is created on the static data segment 
     * only upon the first call, avoiding heap allocation and the 'static 
     * initialization order fiasco'.
     *
     * * @return NetworkManagerClass& reference to the singleton instance.
     */
    static NetworkManagerClass& _getNetworkManagerInstance() {
        static NetworkManagerClass _instance;
        return _instance;
    }

    // -------------------------------------------------------------------------
    // Setup
    // -------------------------------------------------------------------------

    /**
    * @brief Registers an adapter with the manager.
    *
    * Adapters are tried in priority order (lower NetworkProfile::getPriority()
    * value = higher priority). If two adapters share the same priority value,
    * the tie is broken by registration order: the one passed to addAdapter()
    * first becomes the higher-priority one (and thus the primary, with the other
    * as its fallback). This is deterministic — _sortByPriority() is a stable
    * sort — but there is no load-balancing or round-robin between equal-priority
    * adapters; exactly one interface is active at a time. Assign distinct
    * priorities whenever the order between two interfaces actually matters.
    *
    * @note Adapters that would compete for the same underlying radio
    *       (e.g. multiple ESP32 WiFi STA profiles) are not currently
    *       supported for fallback/restore — only one such adapter can be
    *       active at a time (enforced via the adapter's static _activeCount),
    *       and the periodic higher-priority probe in update() cannot safely
    *       retry one without first tearing down the other. Safe combinations
    *       are adapters backed by independent hardware (e.g. ETH + WiFi).
    *
    * @param adapter Reference to the adapter to register. Must outlive
    *                the NetworkManagerClass instance.
    */
    bool addAdapter(NetworkAdapter& adapter) {
        if (!_lock()) return false;
        bool ok = (_adapterCount < NETWORK_MANAGER_MAX_ADAPTERS);
        if (ok) {
            _adapters[_adapterCount++] = &adapter;
            _sortByPriority();
        }
        _unlock();
        return ok;
    }

    /**
     * @brief Registers a callback for high-level network events.
     *
     * Only one callback is supported. Pass nullptr to unregister.
     *
     * @param cb Function pointer to invoke on events.
     */
    void onEvent(EventCb cb) { _onEvent = cb; }

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    /**
     * @brief Registers a callback invoked when NTP time synchronisation completes.
     *
     * On ESP32/ESP8266, the system SNTP callback is used internally to detect
     * synchronisation. Do NOT register your own sntp_set_time_sync_notification_cb()
     * (ESP32) or settimeofday_cb() (ESP8266) — it will override the NetworkManager's
     * internal handler and the NTP sync notification will not be delivered.
     *
     * On AVR, NTP synchronisation is the application's responsibility.
     * This callback will never be invoked on AVR platforms.
     *
     * Only one callback is supported. Pass nullptr to unregister.
     *
     * @param cb Function pointer to invoke on NTP synchronisation.
     */
    void onNtpSync(NtpSyncCb cb) { _onNtpSync = cb; }

    /**
     * @brief Returns the current time as a Unix epoch (seconds since 1970).
     *
     * Portable across all platforms; pairs with onNtpSync() ("time updated")
     * and isTimeValid() ("is the time usable yet").
     *
     * @note On ESP32/ESP8266 this is a thin compatibility wrapper over
     *       time(nullptr): the SDK already maintains the system clock, so
     *       native ESP code can just call time()/ctime() directly. The wrapper
     *       exists so cross-platform code can use one API on every target.
     *       On AVR there is no system clock, so the value is extrapolated from
     *       the last successful sync plus elapsed millis().
     *
     * @return Seconds since 1970-01-01 UTC, or 0 if no sync has happened yet.
     */
    uint32_t getEpoch() const {
        if (!isTimeValid()) return 0;
#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
        return (uint32_t)time(nullptr);
#elif defined(ARDUINO_ARCH_AVR)
        // millis()-based extrapolation from the last sync. millis() wraps every
        // ~49.7 days; the unsigned subtraction stays correct across one wrap.
        return _syncEpoch + (uint32_t)((millis() - _syncMillis) / 1000UL);
#else
        return 0;
#endif
    }

    /**
     * @brief Reports whether the clock has been synchronised at least once.
     *
     * @return true once any NTP sync has succeeded; false until then.
     */
    bool isTimeValid() const { return _timeValid; }

    /**
     * @brief Sets the NTP sync interval in milliseconds (all platforms).
     *
     * Values below 15 s are clamped up to 15 s (RFC 4330 minimum). The interval
     * is stored so a value set before begin() takes effect when NTP is first
     * configured. On ESP32 the change is applied live (the SNTP stack is
     * restarted); on AVR the stored value is picked up by the next poll cycle.
     *
     * @param ms Desired interval in milliseconds.
     *
     * @note ESP8266: lwip2 has no runtime interval setter, so this stored value
     *       only reaches the SNTP scheduler if the sketch includes
     *       NetworkManagerNtpSyncHook.h (in exactly one translation unit).
     *       Without that include the ESP8266 poll cadence stays at the lwip
     *       default (1 hour); the value is still stored and returned by
     *       getNtpSyncInterval(). This is a quirk of the ESP8266 SDK / lwip2.
     */
    void setNtpSyncInterval(uint32_t ms) {
        if (ms < 15000UL) ms = 15000UL;   // RFC 4330 floor, mirrored on every platform
        _ntpSyncInterval = ms;
#if defined(ARDUINO_ARCH_ESP32)
        esp_sntp_set_sync_interval(ms);
        esp_sntp_restart();               // no-op if SNTP is not running yet
#endif
        // ESP8266: lwip2 has no runtime interval setter. The interval reaches
        // lwip via the weak hook sntp_update_delay_MS_rfc_not_less_than_15000(),
        // which the sketch installs by including NetworkManagerNtpSyncHook.h in
        // one TU. It returns _ntpSyncInterval, applied on the next sync cycle.
        // AVR: _avrNtpPoll() reads _ntpSyncInterval directly.
    }

    /**
     * @brief Returns the configured NTP sync interval in milliseconds.
     *
     * @return Interval in milliseconds (never below 15000).
     *
     * @note ESP8266: this is the value NetworkManagerNtpSyncHook.h feeds to
     *       lwip2's SNTP scheduler. It always reflects what was set, but only
     *       changes the actual poll cadence when that hook header is included by
     *       the sketch (see setNtpSyncInterval()).
     */
    uint32_t getNtpSyncInterval() const { return _ntpSyncInterval; }
#endif

    /**
     * @brief Initialises the manager and starts the highest-priority adapter.
     *
     * Registers the internal state change callback on all adapters, then
     * calls start() on the highest-priority adapter.
     *
     * @return true if at least one adapter was started, false if no adapters
     *         are registered.
     */
    bool begin() {
        if (!_lock()) return false;
        bool ok = (_adapterCount != 0);
        if (ok) {
            for (uint8_t i = 0; i < _adapterCount; i++) {
                _adapters[i]->setOnStateChange(_onAdapterStateChange);
            }
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
#if defined(ARDUINO_ARCH_ESP32)
            sntp_set_time_sync_notification_cb(_ntpSyncCallback);
#elif defined(ARDUINO_ARCH_ESP8266)
            settimeofday_cb(_ntpSyncCallback);
#endif
#endif
        }
        _unlock();
        if (ok) ok = _adapters[0]->start();
        return ok;
    }

    /**
     * @brief Stops all adapters and resets the manager state.
     */
    void end() {
        if (!_lock()) return;
        uint8_t count = _adapterCount;
        NetworkAdapter* snapshot[NETWORK_MANAGER_MAX_ADAPTERS];
        for (uint8_t i = 0; i < count; i++) {
            snapshot[i] = _adapters[i];
            // Clear callback while holding the mutex so that any in-flight
            // _handleStateChange() call on the event task either completes
            // before we clear it (and its stop()/start() will be deferred to
            // update(), which will never run again) or sees nullptr and returns
            // immediately. Either way, stop() below runs outside the mutex,
            // preventing a deadlock between the NetworkManager mutex and the
            // adapter's own mutex or the WiFi/ETH driver lock.
            snapshot[i]->setOnStateChange(nullptr);
        }
        // Reset all policy state via the Core, and clear any deferred stops so a
        // later begin() starts from a clean slate. Done under the lock so it
        // cannot race with an in-flight _handleStateChange() on the event task.
        _core.reset();
        _eventQueueHead = 0;
        _eventQueueCount = 0;
        for (uint8_t i = 0; i < NETWORK_MANAGER_MAX_ADAPTERS; i++) {
            _clearPendingStop(i);
        }
        _appliedIdx = -1;
        _unlock();
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
        _disableNtp();
#endif
        for (uint8_t i = 0; i < count; i++) {
            snapshot[i]->stop();
        }
    }

    /**
     * @brief Re-applies the profile of the given adapter.
     *
     * Stops the adapter if active, then restarts it with the current
     * profile settings. Safe to call from any task — thread-safe.
     *
     * Typical use: call after modifying a profile via the web interface
     * to apply the new settings immediately.
     *
     * @param adapter The adapter whose profile has changed.
     * @return true if the restart was initiated, false on mutex failure
     *         or if the adapter is not registered.
     */
    bool applyProfile(NetworkAdapter& adapter) {
        if (!_lock()) return false;
        // Verify adapter is registered
        bool found = false;
        for (uint8_t i = 0; i < _adapterCount; i++) {
            if (_adapters[i] == &adapter) { found = true; break; }
        }
        _unlock();
        if (!found) return false;
        adapter.stop();
        return adapter.start();
    }

    // -------------------------------------------------------------------------
    // Runtime
    // -------------------------------------------------------------------------

    /**
    * @brief Drives the manager and all adapter state machines.
    *
    * Must be called regularly from the main loop. On ESP32/ESP8266 this
    * is lightweight (adapters are event-driven); on AVR and modem adapters
    * this performs the actual polling.
    *
    * Pending stop/start operations are executed here to avoid recursive
    * calls from within event handler context.
    *
    * Also checks the reconnect timeout: if all adapters have been failing
    * for longer than NETWORK_MANAGER_RECONNECT_TIMEOUT milliseconds,
    * emits Event::DISCONNECTED.
    *
    * While a lower-priority adapter is active (FALLBACK state), periodically
    * retries the highest-priority inactive adapter — see NETWORK_MANAGER_
    * PROBE_INTERVAL. This is needed because there is no hardware event that
    * signals "a higher-priority interface might be available again" (e.g.
    * nothing tells us a WiFi AP that was down has come back up). Restricted
    * to adapters backed by independent hardware (see addAdapter() note);
    * retrying one of several adapters sharing a single radio is not safe
    * and is intentionally not attempted here.
    */
    void update() {
        // ---------------------------------------------------------------------
        // This runs on the LOOP TASK. The lock discipline below is the whole
        // basis for the Core needing no atomics: every Core call happens under
        // _mutex, and no hardware call (adapter update/start/stop) or user
        // callback ever happens while the lock is held.
        // ---------------------------------------------------------------------

        // (1) Drive each adapter's own state machine (DHCP timeout, AVR polling).
        // NOT under our lock: an adapter may synchronously transition to FAILED
        // here, which re-enters _handleStateChange() — and that takes _mutex.
        // Holding it now would self-deadlock (the mutex is non-recursive).
        for (uint8_t i = 0; i < _adapterCount; i++) {
            _adapters[i]->update();
        }

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0) && defined(ARDUINO_ARCH_AVR)
        // Drive the AVR SNTP client. Like the adapter updates above, this runs
        // on the loop task and outside our lock: it touches UDP/DNS hardware and
        // may invoke the onNtpSync callback, neither of which may hold _mutex.
        _avrNtpPoll();
#endif

        // (1.5) Deliver deferred user events + NTP (re)configuration on the
        // LOOP TASK. These were queued by _handleStateChange(), possibly on the
        // SDK network-event task. Running them here keeps arbitrary app callback
        // code and the SNTP stack off the small event-task stack. Drained before
        // the deferred stops below so a RESTORED event reaches the app before the
        // superseded adapter is torn down (preserving event-before-stop order).
        for (;;) {
            PendingEvent pe;
            if (!_lock()) break;
            bool have = _dequeueEvent(pe);
            _unlock();
            if (!have) break;
            if (pe.idx < 0 || pe.idx >= (int8_t)_adapterCount) continue;
            if (_onEvent) _onEvent(_mapEvent(pe.emit), *_adapters[pe.idx]);
        }

        // (2) Apply deferred stops. A stop() is the logical teardown of an
        // adapter (-> IDLE); it is hardware-touching and may fire its own
        // events, so it must run on the loop task and outside our lock. The
        // pending-stop bits are set by _handleStateChange() (FAILED teardown,
        // or RESTORED supersede) and are the single deferral mechanism for both.
        for (uint8_t i = 0; i < _adapterCount; i++) {
            if (_consumePendingStop(i)) {
                _adapters[i]->stop();
            }
        }

        // Everything that must follow the serving interface — the default route,
        // the DNS resolver and SNTP — is repaired here, keyed to the serving
        // adapter CHANGING. Keying it to "a stop happened in this update()" was
        // wrong: on a real link failure the stop is applied while nothing is
        // connected yet (idx < 0), and the replacement adapter connects only in
        // a later update(), so the repair was skipped entirely.
        int8_t idx = _getConnectedIndex();
        if (idx != _appliedIdx) {
            _appliedIdx = idx;

#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
            if (idx >= 0) {
                _adapters[idx]->setDefaultRoute();   // route first — DNS is useless without it
                _reassertDns(*_adapters[idx]);
            }
#endif

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
            if (idx >= 0) _configureNtp(*_adapters[idx]);
            else          _disableNtp();
#endif
        }

        // (3) Apply profile re-apply requests from other tasks (web UI etc.).
        for (uint8_t i = 0; i < _adapterCount; i++) {
            if (_adapters[i]->_consumePendingApply()) {
                applyProfile(*_adapters[i]);
            }
        }

        // (4) Ask the Core what to do now: probe and/or escalate to DISCONNECTED.
        // Snapshot is built BEFORE locking (it calls canProbe(), which may read
        // hardware such as ETH.linkUp() — kept out of the lock on principle).
        // The Core call itself is under the lock so its policy state stays
        // consistent against a concurrent event-task classification.
        NetworkManagerCore::StateView view = _snapshot();
        if (!_lock()) return;
        NetworkManagerCore::Decision d = _core.tick(view, millis());
        _unlock();

        // (5) Execute the Core's decision OUTSIDE the lock.
        //   - start() is a probe; it is hardware-touching but always originates
        //     here on the loop task, so it is run directly (never deferred).
        //   - DISCONNECTED is delivered to the application like any other event.
        if (d.startIdx >= 0 && d.startIdx < (int8_t)_adapterCount) {
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0) && defined(ARDUINO_ARCH_ESP8266)
            // Enable DHCP-provided NTP (option 42) capture BEFORE the adapter's
            // DHCP handshake starts, so the offered servers are stored as the
            // lease is acquired. Doing this only at CONNECTED (in _configureNtp)
            // is too late for the WiFi path — the handshake has already finished
            // and the option-42 data is gone, so DHCP-based NTP never syncs in
            // fallback. Set from the to-be-started adapter's own profile.
            sntp_servermode_dhcp(
                _adapters[d.startIdx]->getProfile().isConfiguredNtp() ? 0 : 1);
#endif
            _adapters[d.startIdx]->start();
        }
        // The adapter passed to the callback is the interface that was last
        // serving before the outage (the primary adapter if none ever served).
        if (d.emit == NetworkManagerCore::Event::DISCONNECTED &&
            _onEvent && d.emitIdx >= 0 && d.emitIdx < (int8_t)_adapterCount) {
            _onEvent(_mapEvent(d.emit), *_adapters[d.emitIdx]);
        }
    }

    // -------------------------------------------------------------------------
    // Status
    // -------------------------------------------------------------------------

    /**
     * @brief Returns true if any adapter is in CONNECTED state.
     */
    bool isConnected() const {
        if (!_lock()) return false;
        bool ok = _getConnectedIndex() >= 0;
        _unlock();
        return ok;
    }

    /**
     * @brief Returns a pointer to the currently active (CONNECTED) adapter,
     *        or nullptr if none is connected.
     */
    NetworkAdapter* getActiveAdapter() const {
        if (!_lock()) return nullptr;
        int8_t idx = _getConnectedIndex();
        NetworkAdapter* result = (idx >= 0) ? _adapters[idx] : nullptr;
        _unlock();
        return result;
    }

    /**
     * @brief Returns the live network snapshot of the currently active adapter.
     * @return The active adapter's NetworkStatus, or a disconnected snapshot
     *         (connected=false, all addresses 0.0.0.0) if none is connected.
     */
    NetworkStatus getStatus() const {
        NetworkAdapter* active = getActiveAdapter();
        return active ? active->getStatus() : NetworkStatus{};
    }

    /**
    * @brief Returns the IP address of the currently active adapter.
    * @return Assigned IP address, or IPAddress() (0.0.0.0) if not connected.
    */
    IPAddress getLocalIP() const {
        NetworkAdapter* active = getActiveAdapter();
        return active ? active->getLocalIP() : IPAddress();
    }

    /**
     * @brief Subnet mask of the active adapter (0.0.0.0 if not connected).
     */
    IPAddress getSubnetMask() const {
        NetworkAdapter* active = getActiveAdapter();
        return active ? active->getSubnetMask() : IPAddress();
    }

    /**
     * @brief Default gateway of the active adapter (0.0.0.0 if not connected).
     */
    IPAddress getGatewayIP() const {
        NetworkAdapter* active = getActiveAdapter();
        return active ? active->getGatewayIP() : IPAddress();
    }

    /**
     * @brief DNS server @p i of the active adapter (0.0.0.0 if not connected).
     * @param i Index in [0, NetworkProfile::DNS_SERVER_COUNT); clamped to 0.
     */
    IPAddress getDns(uint8_t i = 0) const {
        NetworkAdapter* active = getActiveAdapter();
        return active ? active->getDns(i) : IPAddress();
    }

    /**
     * @brief Sets a device-level hostname on every adapter added so far.
     *
     * A hostname is a device identity rather than a per-interface property, so
     * this applies @p name to each added adapter's profile, giving one name for
     * whichever interface serves. It fans out over the adapters present at the
     * time of the call, so invoke it after addAdapter(); no hostname buffer is
     * held here (RAM-friendly on AVR), hence a later-added adapter keeps its own
     * default until set. An empty string resets each adapter to its generated
     * default.
     *
     * @param name Hostname to apply, or "" to reset to the default.
     * @return true if applied to every adapter; false if none were added or a
     *         profile rejected the value.
     */
    bool setHostname(const char* name) {
        bool ok = (_adapterCount != 0);
        for (uint8_t i = 0; i < _adapterCount; i++)
            ok &= _adapters[i]->getProfile().setHostname(name);
        return ok;
    }

    /**
     * @brief Returns the hostname of the serving interface.
     *
     * The device's current identity: the hostname of the highest-priority
     * connected adapter, or the first adapter if none is connected. See
     * ConfigSource for ACTIVE vs FACTORY.
     *
     * @param buf    Destination buffer.
     * @param len    Capacity of @p buf.
     * @param source ConfigSource::ACTIVE (default) or ConfigSource::FACTORY.
     * @return true on success; false if no adapter has been added.
     */
    bool getHostname(char* buf, size_t len,
                     NetworkProfile::ConfigSource source =
                         NetworkProfile::ConfigSource::ACTIVE) const {
        if (_adapterCount == 0) return false;
        int8_t i = _getConnectedIndex();
        if (i < 0) i = 0;
        return _adapters[i]->getHostname(buf, len, source);
    }

    /**
     * @brief Returns the MAC address of the serving interface.
     *
     * Read-only at the manager level: a MAC is per-interface, so there is
     * deliberately no global setMac() — set it via the interface's profile
     * instead. Reports the highest-priority connected adapter, or the first
     * adapter if none is connected.
     *
     * @param mac    Destination buffer (MAC_LEN bytes).
     * @param source ConfigSource::ACTIVE (default) or ConfigSource::FACTORY.
     * @return true on success; false if no adapter has been added.
     */
    bool getMac(NetworkProfile::MACAddress mac,
                NetworkProfile::ConfigSource source =
                    NetworkProfile::ConfigSource::ACTIVE) const {
        if (_adapterCount == 0) return false;
        int8_t i = _getConnectedIndex();
        if (i < 0) i = 0;
        return _adapters[i]->getMac(mac, source);
    }

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    /**
     * @brief Copies NTP server @p i of the active adapter's profile into @p out.
     *
     * Unlike the address getters, NTP is a configured profile value (not a live
     * netif property), so it is read from the active adapter's profile rather
     * than the status snapshot.
     *
     * @param i   NTP index in [0, NETWORK_PROFILE_NTP_SERVER_COUNT).
     * @param out Caller buffer for the server FQDN/IP string.
     * @param len Size of @p out.
     * @return true on success; false if not connected or on truncation.
     */
    bool getNtp(uint8_t i, char* out, size_t len) const {
        if (out && len) out[0] = '\0';
        NetworkAdapter* active = getActiveAdapter();
        if (!active) return false;
        return active->getProfile().getNtp(i, out, len);
    }

    /**
     * @brief Copies the *active* SNTP server name for slot @p i into @p out.
     *
     * Unlike getNtp() — which returns the server configured in the profile —
     * this reads the live SNTP client, so it also reflects DHCP-provided
     * servers (option 42). A name is present for host-name servers (e.g.
     * "pool.ntp.org") and empty for numeric servers (DHCP delivers addresses);
     * use getActiveNtpIP() for those. Reads the SNTP stack live, so no adapter
     * or lock is involved.
     *
     * @param i   Server slot index.
     * @param out Caller buffer for the name.
     * @param len Size of @p out.
     * @return true if a non-empty name was copied; false otherwise (out is "").
     */
    bool getActiveNtpName(uint8_t i, char* out, size_t len) const {
        if (!out || !len) return false;
        out[0] = '\0';
#if defined(ARDUINO_ARCH_ESP32)
        const char* n = esp_sntp_getservername(i);
#elif defined(ARDUINO_ARCH_ESP8266)
        const char* n = sntp_getservername(i);
#elif defined(ARDUINO_ARCH_AVR)
        // AVR keeps no SNTP name registry; report the active profile's name.
        char nbuf[Host::MAX_FQDN_SIZE];
        NetworkAdapter* active = getActiveAdapter();
        const char* n = nullptr;
        if (active && active->getProfile().getNtp(i, nbuf, sizeof(nbuf))) n = nbuf;
#else
        const char* n = nullptr;
#endif
        if (!n || n[0] == '\0') return false;
        strncpy(out, n, len - 1);
        out[len - 1] = '\0';
        return true;
    }

    /**
     * @brief The *active* (resolved or numeric) SNTP server address for slot @p i.
     *
     * Reads the live SNTP client, so it reflects both DHCP-provided numeric
     * servers and the address a host-name server currently resolves to — which
     * can change as a pool rotates. Returns 0.0.0.0 if the slot is unset or a
     * host name has not been resolved yet.
     */
    IPAddress getActiveNtpIP(uint8_t i) const {
#if defined(ARDUINO_ARCH_ESP32)
        const ip_addr_t* a = esp_sntp_getserver(i);
        if (a && !ip_addr_isany(a)) return IPAddress(ip4_addr_get_u32(ip_2_ip4(a)));
#elif defined(ARDUINO_ARCH_ESP8266)
        const ip_addr_t* a = sntp_getserver(i);
        if (a && !ip_addr_isany(a)) return IPAddress(ip4_addr_get_u32(ip_2_ip4(a)));
#elif defined(ARDUINO_ARCH_AVR)
        // AVR minimal client resolves a single server; expose it once known.
        if (i == 0 && _avrNtpServerKnown) return _avrNtpServerIp;
#else
        (void)i;
#endif
        return IPAddress();
    }
#endif

    /**
     * @brief Worst-case length (excluding NUL) of statusToJson() output.
     *
     * Covers the fixed fields plus, when NTP is enabled, one active NTP server
     * whose name may be a full-length FQDN. Use STATUS_JSON_SIZE to size a buffer.
     */
    static constexpr size_t STATUS_JSON_LEN =
        200
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
        + NetworkProfile::NTP_SERVER_COUNT * (Host::MAX_FQDN_LEN + 48)
#endif
        ;
    /** @brief Buffer size (including NUL) for statusToJson(). */
    static constexpr size_t STATUS_JSON_SIZE = STATUS_JSON_LEN + 1;

    /**
     * @brief Serialises the active network status as a compact JSON object.
     *
     * Intended for a web server or MQTT payload. Writes into a caller buffer
     * (no heap, no String); size it with STATUS_JSON_SIZE. The object always
     * contains interface, connected, ip, mask, gw and a dns array (populated
     * slots only). With @p includeNtp, an \"ntp\" object is appended carrying the
     * overall synced flag and a \"servers\" array of the live SNTP servers (up to
     * NETWORK_PROFILE_NTP_SERVER_COUNT). Each server reports its live name and its
     * resolved/numeric ip; a DHCP-provided server shows an empty name.
     *
     * Example (includeNtp = true):
     * {\"interface\":\"eth\",\"connected\":true,\"ip\":\"172.20.11.78\",
     *  \"mask\":\"255.255.0.0\",\"gw\":\"172.20.0.1\",
     *  \"dns\":[\"172.20.0.121\",\"172.20.0.122\"],
     *  \"ntp\":{\"synced\":true,\"servers\":[
     *     {\"name\":\"pool.ntp.org\",\"ip\":\"162.159.200.1\"},
     *     {\"name\":\"\",\"ip\":\"172.20.0.121\"}]}}
     *
     * @param out        Destination buffer.
     * @param len        Size of @p out.
     * @param includeNtp When true, append the live \"ntp\" object.
     * @return Number of characters written (excluding NUL), or 0 on error/overflow
     *         (in which case @p out is set to an empty string).
     */
    size_t statusToJson(char* out, size_t len, bool includeNtp = false) const {
        if (!out || len == 0) return 0;
        out[0] = '\0';
        const NetworkStatus s = getStatus();
        char buf[64];
        bool ok = true;

        ok &= _jsonCat(out, "{", len);
        const char* ifn =
            (s.interfaceType == NetworkProfile::InterfaceType::ETH)  ? "eth"  :
            (s.interfaceType == NetworkProfile::InterfaceType::WIFI) ? "wifi" : "unknown";
        snprintf(buf, sizeof(buf), "\"interface\":\"%s\",\"connected\":%s",
                 ifn, s.connected ? "true" : "false");
        ok &= _jsonCat(out, buf, len);
        snprintf(buf, sizeof(buf), ",\"ip\":\"%u.%u.%u.%u\"",
                 s.localIP[0], s.localIP[1], s.localIP[2], s.localIP[3]);
        ok &= _jsonCat(out, buf, len);
        snprintf(buf, sizeof(buf), ",\"mask\":\"%u.%u.%u.%u\"",
                 s.subnetMask[0], s.subnetMask[1], s.subnetMask[2], s.subnetMask[3]);
        ok &= _jsonCat(out, buf, len);
        snprintf(buf, sizeof(buf), ",\"gw\":\"%u.%u.%u.%u\"",
                 s.gateway[0], s.gateway[1], s.gateway[2], s.gateway[3]);
        ok &= _jsonCat(out, buf, len);

        ok &= _jsonCat(out, ",\"dns\":[", len);
        bool first = true;
        for (uint8_t i = 0; i < NetworkProfile::DNS_SERVER_COUNT; i++) {
            if (s.dns[i] == IPAddress()) continue;
            snprintf(buf, sizeof(buf), "%s\"%u.%u.%u.%u\"",
                     first ? "" : ",", s.dns[i][0], s.dns[i][1], s.dns[i][2], s.dns[i][3]);
            ok &= _jsonCat(out, buf, len);
            first = false;
        }
        ok &= _jsonCat(out, "]", len);

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
        if (includeNtp) {
            snprintf(buf, sizeof(buf), ",\"ntp\":{\"synced\":%s,\"servers\":[",
                     isTimeValid() ? "true" : "false");
            ok &= _jsonCat(out, buf, len);
            char name[Host::MAX_FQDN_SIZE];
            bool firstNtp = true;
            for (uint8_t i = 0; i < NetworkProfile::NTP_SERVER_COUNT; i++) {
                const bool      hasName = getActiveNtpName(i, name, sizeof(name));
                const IPAddress nip     = getActiveNtpIP(i);
                if (!hasName && nip == IPAddress()) continue;   // unused slot
                ok &= _jsonCat(out, firstNtp ? "{\"name\":\"" : ",{\"name\":\"", len);
                if (hasName) ok &= _jsonCat(out, name, len);
                snprintf(buf, sizeof(buf), "\",\"ip\":\"%u.%u.%u.%u\"}",
                         nip[0], nip[1], nip[2], nip[3]);
                ok &= _jsonCat(out, buf, len);
                firstNtp = false;
            }
            ok &= _jsonCat(out, "]}", len);
        }
#else
        (void)includeNtp;
#endif
        ok &= _jsonCat(out, "}", len);

        if (!ok) { out[0] = '\0'; return 0; }
        return strlen(out);
    }

private:
    /**
     * @brief Bounded JSON append: concatenates @p src onto @p dst if it fits.
     * @return false (without writing) if the result would not fit in @p len.
     */
    static bool _jsonCat(char* dst, const char* src, size_t len) {
        const size_t cur = strlen(dst);
        if (cur >= len) return false;
        const size_t n = strlen(src);
        if (n + 1 > len - cur) return false;   // no room for src + NUL
        memcpy(dst + cur, src, n + 1);
        return true;
    }
    NetworkManagerClass()  = default;   // Private constructor
    ~NetworkManagerClass() = default;   // Private destructor

    NetworkAdapter* _adapters[NETWORK_MANAGER_MAX_ADAPTERS] = {};
    uint8_t         _adapterCount                           = 0;
    EventCb         _onEvent                                = nullptr; ///< Set once before begin(); not modified at runtime.
    int8_t          _appliedIdx                             = -1;

    #if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    NtpSyncCb       _onNtpSync                              = nullptr; ///< Set once before begin(); not modified at runtime.

    // Configured sync interval (ms). Stored on every platform: AVR's poll loop
    // reads it directly; ESP also pushes it into the SDK (see setNtpSyncInterval
    // and _configureNtp). Starts at the compile-time default.
    uint32_t        _ntpSyncInterval                        = NETWORK_MANAGER_DEFAULT_NTP_SYNC_INTERVAL;

    // True once at least one sync has succeeded. On ESP it is written from the
    // SDK's SNTP callback (a different task), so it is atomic there; on AVR it
    // is written and read only on the loop task, so a plain bool suffices.
#ifndef ARDUINO_ARCH_AVR
    std::atomic<bool> _timeValid{false};
#else
    bool            _timeValid                              = false;
#endif

#if defined(ARDUINO_ARCH_AVR)
    // ---- AVR minimal SNTP client state (loop task only; no atomics needed) ----
    enum class AvrNtpState : uint8_t { IDLE, WAIT_REPLY };

    EthernetUDP     _avrNtpUdp;                              ///< UDP socket, opened per sync and closed after.
    NetworkProfile* _avrNtpProfile     = nullptr;           ///< Active profile to read getNtp(0) from; null = NTP off.
    IPAddress       _avrNtpServerIp;                         ///< Cached resolved server address.
    bool            _avrNtpServerKnown  = false;             ///< True once _avrNtpServerIp is valid.
    AvrNtpState     _avrNtpState         = AvrNtpState::IDLE;///< Request/reply state machine.
    bool            _avrNtpDue           = false;            ///< A sync has been requested (connect or interval).
    uint32_t        _avrNtpLastSyncMs    = 0;                ///< millis() of last sync/attempt (0 = never).
    uint32_t        _avrNtpRetryInterval = 0;                ///< Post-resolve backoff delay (0 = use sync interval).
    uint32_t        _avrNtpReqSentMs     = 0;                ///< millis() the current request was sent.
    uint32_t        _syncEpoch           = 0;                ///< Unix epoch captured at last sync.
    uint32_t        _syncMillis          = 0;                ///< millis() captured at last sync (extrapolation base).

    static constexpr uint16_t AVR_NTP_LOCAL_PORT = 8888;    ///< Local UDP port for the client.
    static constexpr uint16_t AVR_NTP_SERVER_PORT = 123;    ///< Standard NTP port.
    static constexpr uint32_t AVR_NTP_REPLY_TIMEOUT_MS = 2000; ///< Give up waiting for a reply after this.
#endif
#endif

    // -------------------------------------------------------------------------
    // Decision engine + deferred-stop plumbing
    // -------------------------------------------------------------------------

    // All fallback/restore/reconnect POLICY lives in the Core. Its state is
    // touched only under _mutex (by _handleStateChange on the event task and by
    // update() on the loop task), which is why it needs no atomics of its own.
    NetworkManagerCore _core { NETWORK_MANAGER_RECONNECT_TIMEOUT };

    // Deferred-stop bits: one per adapter. Set by _handleStateChange() (event
    // handler context) when the Core asks to stop an adapter (FAILED teardown or
    // RESTORED supersede); consumed by update() on the loop task, which performs
    // the actual stop(). Centralising both kinds of stop in one bitmap keeps the
    // event task free of hardware teardown. These bits are genuine cross-task
    // communication, so on ESP32/ESP8266 they are atomic; on AVR plain values.
#ifndef ARDUINO_ARCH_AVR
    std::atomic<bool> _pendingStopBits[NETWORK_MANAGER_MAX_ADAPTERS] = {};
#else
    bool              _pendingStopBits[NETWORK_MANAGER_MAX_ADAPTERS] = {};
#endif

    /** @brief Marks an adapter to be stopped on the next update() (loop task). */
    void _requestStop(int8_t idx) {
        if (idx < 0 || idx >= NETWORK_MANAGER_MAX_ADAPTERS) return;
#ifndef ARDUINO_ARCH_AVR
        _pendingStopBits[idx].store(true);
#else
        _pendingStopBits[idx] = true;
#endif
    }
    /** @brief Atomically reads and clears the deferred-stop bit for an adapter. */
    bool _consumePendingStop(uint8_t idx) {
#if defined(ARDUINO_ARCH_ESP32)
        return _pendingStopBits[idx].exchange(false);
#elif defined(ARDUINO_ARCH_ESP8266)
        // Cooperative: non-atomic read-then-clear avoids __atomic_exchange (no
        // libatomic on xtensa-lx106); load/store still guarantee visibility.
        bool v = _pendingStopBits[idx].load(); _pendingStopBits[idx].store(false); return v;
#else
        bool v = _pendingStopBits[idx]; _pendingStopBits[idx] = false; return v;
#endif
    }
    /** @brief Clears the deferred-stop bit without acting on it (used by end()). */
    void _clearPendingStop(uint8_t idx) {
#ifndef ARDUINO_ARCH_AVR
        _pendingStopBits[idx].store(false);
#else
        _pendingStopBits[idx] = false;
#endif
    }

    // Deferred user-event queue. Populated by _handleStateChange() (event task
    // OR loop task) and drained by update() on the loop task, so the user event
    // callback — which runs arbitrary app code — never executes on the small SDK
    // event-task stack. Touched only under _mutex (like the Core), so it needs
    // no atomics. A small ring buffer avoids losing rapid transitions; on the
    // rare overflow the oldest queued event is dropped.
    struct PendingEvent {
        NetworkManagerCore::Event emit = NetworkManagerCore::Event::NONE;
        int8_t                    idx  = -1;
    };
    static constexpr uint8_t _EVENT_QUEUE_LEN = 8;
    PendingEvent _eventQueue[_EVENT_QUEUE_LEN];
    uint8_t      _eventQueueHead  = 0; ///< Touched only under _mutex.
    uint8_t      _eventQueueCount = 0; ///< Touched only under _mutex.

    /** @brief Enqueues a user event (call under _mutex). Drops the oldest if full. */
    void _enqueueEvent(NetworkManagerCore::Event e, int8_t idx) {
        if (e == NetworkManagerCore::Event::NONE) return;
        if (_eventQueueCount >= _EVENT_QUEUE_LEN) {
            _eventQueueHead = (uint8_t)((_eventQueueHead + 1) % _EVENT_QUEUE_LEN);
            _eventQueueCount--;
        }
        uint8_t tail = (uint8_t)((_eventQueueHead + _eventQueueCount) % _EVENT_QUEUE_LEN);
        _eventQueue[tail].emit = e;
        _eventQueue[tail].idx  = idx;
        _eventQueueCount++;
    }

    /** @brief Pops the oldest queued event (call under _mutex). False if empty. */
    bool _dequeueEvent(PendingEvent& out) {
        if (_eventQueueCount == 0) return false;
        out = _eventQueue[_eventQueueHead];
        _eventQueueHead = (uint8_t)((_eventQueueHead + 1) % _EVENT_QUEUE_LEN);
        _eventQueueCount--;
        return true;
    }

    /**
     * @brief Builds a Core snapshot of the current adapter set.
     *
     * Reads each adapter's state (a lock-free atomic load) and canProbe()
     * (which may read hardware, e.g. ETH.linkUp()). The snapshot is a frozen
     * copy: the Core reasons about it without ever touching live adapters.
     */
    /**
     * @brief Maps an adapter State to the Core's State.
     *
     * The two enums are deliberately separate types (the adapter base and the
     * pure Core know nothing of each other). They currently share the same
     * order, but mapping explicitly — rather than casting — means a future
     * reorder of either enum fails loudly here instead of silently feeding the
     * Core wrong states.
     */
    static NetworkManagerCore::State _mapState(NetworkAdapter::State s) {
        switch (s) {
            case NetworkAdapter::State::IDLE:       return NetworkManagerCore::State::IDLE;
            case NetworkAdapter::State::CONNECTING: return NetworkManagerCore::State::CONNECTING;
            case NetworkAdapter::State::CONNECTED:  return NetworkManagerCore::State::CONNECTED;
            case NetworkAdapter::State::FAILED:     return NetworkManagerCore::State::FAILED;
        }
        return NetworkManagerCore::State::IDLE;
    }

    NetworkManagerCore::StateView _snapshot() const {
        NetworkManagerCore::StateView v;
        v.count = _adapterCount;
        for (uint8_t i = 0; i < _adapterCount; i++) {
            v.state[i]    = _mapState(_adapters[i]->getState());
            v.canProbe[i] = _adapters[i]->canProbe();
        }
        return v;
    }

    /** @brief Maps an internal Core event to the public NetworkManager event. */
    static Event _mapEvent(NetworkManagerCore::Event e) {
        switch (e) {
            case NetworkManagerCore::Event::CONNECTED:    return Event::CONNECTED;
            case NetworkManagerCore::Event::DISCONNECTED: return Event::DISCONNECTED;
            case NetworkManagerCore::Event::FALLBACK:     return Event::FALLBACK;
            case NetworkManagerCore::Event::RESTORED:     return Event::RESTORED;
            case NetworkManagerCore::Event::NONE:         break;  // never mapped — caller guards emit != NONE
        }
        // NONE / out-of-range: return a defined value. No default: above, so if a
        // new Core::Event is ever added, -Wswitch flags it here at compile time
        // (same exhaustiveness guarantee as _mapState).
        return Event::CONNECTED;
    }

    // -------------------------------------------------------------------------
    // Mutex
    // -------------------------------------------------------------------------

    // Real timed mutex only where there is preemptive concurrency (ESP32 and
    // the host-test build). On AVR (single-threaded) and ESP8266 (cooperative
    // scheduling) the lock is a no-op; the atomics above still guard flags
    // written from callback context.
#if !defined(ARDUINO_ARCH_AVR) && !defined(ARDUINO_ARCH_ESP8266)
    mutable std::timed_mutex _mutex;

    bool _lock() const {
        return _mutex.try_lock_for(
            std::chrono::milliseconds(MUTEX_TIMEOUT));
    }
    void _unlock() const { _mutex.unlock(); }
#else
    bool _lock()   const { return true; }
    void _unlock() const {}
#endif

    // -------------------------------------------------------------------------
    // Internal
    // -------------------------------------------------------------------------

    /**
     * @brief Sorts _adapters by profile priority (ascending).
     *
     * Called by addAdapter(). Insertion sort — suitable for small N.
     */
    void _sortByPriority() {
        // Insertion sort — stable, suitable for small N.
        // Caller must hold _mutex.
        for (uint8_t i = 1; i < _adapterCount; i++) {
            NetworkAdapter* key     = _adapters[i];
            uint8_t         keyPrio = key->getProfile().getPriority();
            int8_t          j       = (int8_t)i - 1;
            while (j >= 0 && _adapters[j]->getProfile().getPriority() > keyPrio) {
                _adapters[j + 1] = _adapters[j];
                j--;
            }
            _adapters[j + 1] = key;
        }
    }

#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
    /**
     * @brief Re-installs the serving adapter's DNS servers into lwIP.
     *
     * lwIP's resolver table is global (not per-netif), and tearing down a
     * superseded adapter clears it — wiping the servers of the interface that is
     * now serving. Resolution then survives only as long as the DNS cache, after
     * which every lookup fails (an FQDN NTP server can never be resolved again).
     * The servers are taken from the adapter's cached status, so no new DHCP
     * exchange is needed. Called after the deferred stops, once the topology has
     * settled.
     */
    void _reassertDns(NetworkAdapter& adapter) {
        NetworkStatus s = adapter.getStatus();
        for (uint8_t i = 0; i < NetworkProfile::DNS_SERVER_COUNT && i < DNS_MAX_SERVERS; i++) {
            if (s.dns[i] == IPAddress(0, 0, 0, 0)) continue;
            ip_addr_t a;
            IP_ADDR4(&a, s.dns[i][0], s.dns[i][1], s.dns[i][2], s.dns[i][3]);
            dns_setserver(i, &a);
        }
    }
#endif

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    /** @brief Configures NTP synchronisation for the given adapter's profile. */
    void _configureNtp(NetworkAdapter& adapter) {
#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
        NetworkProfile& profile = adapter.getProfile();
        if (!profile.isConfiguredNtp()) {
            configTime(0, 0, NULL, NULL, NULL);
        } else {
            static char ntp[NetworkProfile::NTP_SERVER_COUNT][Host::MAX_FQDN_SIZE];

            // Pass only the slots that actually hold a server, compacted from
            // index 0. An empty slot must be handed to configTime() as NULL, not
            // as "": lwIP stores server names and cycles through them, and a DNS
            // lookup for an empty name fails, costing one full retry interval per
            // empty slot before a valid server is tried again. Compacting also
            // keeps the active-server accessors reporting from slot 0 upwards.
            const char* srv[3] = { NULL, NULL, NULL };
            uint8_t n = 0;
            for (uint8_t i = 0; i < NetworkProfile::NTP_SERVER_COUNT && n < 3; i++) {
                profile.getNtp(i, ntp[i], Host::MAX_FQDN_SIZE);
                if (ntp[i][0] != '\0') srv[n++] = ntp[i];
            }
            configTime(0, 0, srv[0], srv[1], srv[2]);
        }
#if defined(ARDUINO_ARCH_ESP8266)
        // Use DHCP-provided NTP servers (option 42) when the profile carries no
        // servers of its own; otherwise prefer the configured servers. This is
        // the ESP8266 counterpart of esp_sntp_servermode_dhcp() on ESP32. The
        // lwip2 hook compiles to a no-op macro when SNTP_GET_SERVERS_FROM_DHCP
        // is disabled in the build, so the call is always safe.
        sntp_servermode_dhcp(profile.isConfiguredNtp() ? 0 : 1);
#endif
        // configTime() (re)starts SNTP at the IDF default cadence, so re-assert
        // any interval the application configured. The restart makes the new
        // interval take effect now rather than after the first default period.
#if defined(ARDUINO_ARCH_ESP32)
        esp_sntp_set_sync_interval(_ntpSyncInterval);
        esp_sntp_restart();
#endif
        // ESP8266: the poll interval reaches lwip via the weak hook the sketch
        // installs by including NetworkManagerNtpSyncHook.h (in one TU).
#elif defined(ARDUINO_ARCH_AVR)
        // Arm the AVR SNTP client for the adapter that just connected: remember
        // its profile, drop any cached server address (re-resolve on the new
        // network), and request a sync on the next update() iteration.
        NetworkProfile& profile = adapter.getProfile();
        _avrNtpProfile     = profile.isConfiguredNtp() ? &profile : nullptr;
        _avrNtpServerKnown   = false;
        _avrNtpRetryInterval = 0;                        // fresh connect -> normal cadence
        _avrNtpState       = AvrNtpState::IDLE;
        _avrNtpLastSyncMs  = 0;                          // 0 -> due immediately
        _avrNtpDue         = (_avrNtpProfile != nullptr);
#endif
    }

    /**
     * @brief Pauses NTP while no adapter is connected. On the next connect,
     *        _configureNtp() re-arms it with a fresh cadence (backoff reset).
     */
    void _disableNtp() {
#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
        configTime(0, 0, NULL, NULL, NULL);        // stop the lwIP SNTP client
#if defined(ARDUINO_ARCH_ESP8266)
        sntp_servermode_dhcp(0);
#endif
#elif defined(ARDUINO_ARCH_AVR)
        if (_avrNtpState == AvrNtpState::WAIT_REPLY) _avrNtpUdp.stop();  // release socket
        _avrNtpState   = AvrNtpState::IDLE;
        _avrNtpProfile = nullptr;                  // _avrNtpPoll() early-returns
        _avrNtpDue     = false;
#endif
    }
#endif

    /**
     * @brief Returns the index of the highest-priority CONNECTED adapter,
     *        or -1 if none.
     */
    int8_t _getConnectedIndex() const {
        // Caller must hold _mutex (or _adapterCount must be stable).
        for (uint8_t i = 0; i < _adapterCount; i++) {
            if (_adapters[i]->getState() == NetworkAdapter::State::CONNECTED) {
                return (int8_t)i;
            }
        }
        return -1;
    }

    /**
     * @brief Handles adapter state changes and implements fallback/restore logic.
     *
     * Called by the static _onAdapterStateChange() trampoline. Delegates the
     * actual decision to the Core and executes the result. See the body for the
     * per-task lock discipline.
     *
     * @param adapter The adapter that changed state.
     * @param state   The new state.
     */
    void _handleStateChange(NetworkAdapter& adapter,
                                            NetworkAdapter::State state) {
        // ---------------------------------------------------------------------
        // This may run on EITHER task: the ESP32/ESP8266 network event task (for
        // GOT_IP / DISCONNECTED etc.) or the loop task (when adapter->update()
        // detects a DHCP timeout). Either way the rules are the same:
        //   - The Core is consulted under _mutex (so its policy state is
        //     consistent against the loop task's tick()).
        //   - Hardware actions (stop) are DEFERRED to update() via a pending bit,
        //     because calling teardown from the event task risks re-entrancy /
        //     stack overflow (e.g. WiFi.disconnect() firing more events).
        //   - The user event callback is fired synchronously here, outside the
        //     lock — exactly as before — so its delivery is never lost.
        // The Core only cares about CONNECTED and FAILED; IDLE/CONNECTING carry
        // no policy meaning, so they are ignored cheaply without locking.
        // ---------------------------------------------------------------------
        if (state != NetworkAdapter::State::CONNECTED &&
            state != NetworkAdapter::State::FAILED) {
            return;
        }

        int8_t idx = -1;
        for (uint8_t i = 0; i < _adapterCount; i++) {
            if (_adapters[i] == &adapter) { idx = (int8_t)i; break; }
        }
        if (idx < 0) return;

        // Snapshot before locking (canProbe() may read hardware). The Core
        // guarantees the one invariant we rely on for a CONNECTED event: this
        // adapter reads CONNECTED in the view, because _setState(CONNECTED) ran
        // before this callback.
        NetworkManagerCore::StateView view = _snapshot();

        if (!_lock()) return;
        NetworkManagerCore::Decision d =
            (state == NetworkAdapter::State::CONNECTED)
                ? _core.onConnected((uint8_t)idx, view, millis())
                : _core.onFailed((uint8_t)idx, view, millis());
        // Record the requested stop AND the user event as deferred work while
        // still under the lock; both are executed later by update() on the loop
        // task. Deferring the event (not only the stop) is essential: firing the
        // user callback synchronously here would run arbitrary app code — e.g.
        // getStatus() and Serial prints — deep inside the event chain on the
        // small SDK network-event task stack, which overflows it (observed as an
        // ESP32 'arduino_events' stack-canary panic). NTP (re)configuration is
        // deferred for the same reason (it also touches the SNTP stack).
        if (d.stopIdx >= 0) _requestStop(d.stopIdx);
        _enqueueEvent(d.emit, d.emitIdx);
        _unlock();
    }

    /**
     * @brief Static trampoline registered on all adapters as StateChangeCb.
     *
     * Forwards to NetworkManagerClass::_instance._handleStateChange().
     *
     * @param adapter The adapter that changed state.
     * @param state   The new state.
     */
    static void _onAdapterStateChange(NetworkAdapter& adapter,
                                      NetworkAdapter::State state) {
        _getNetworkManagerInstance()._handleStateChange(adapter, state);
    }

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
#if defined(ARDUINO_ARCH_ESP32)
    /**
     * @brief SNTP sync callback registered via sntp_set_time_sync_notification_cb().
     *
     * Invokes _onNtpSync if registered.
     *
     * @param tv Pointer to the synchronised time (unused).
     */
    static void _ntpSyncCallback(struct timeval* tv) {
        (void)tv;
        // Only report a genuine, completed SNTP sync — skip IN_PROGRESS steps
        // (e.g. under SNTP_SYNC_MODE_SMOOTH) so the callback always means "time
        // is now NTP-accurate", never a partial adjustment.
        if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) return;
        _getNetworkManagerInstance()._timeValid = true;
        if (_getNetworkManagerInstance()._onNtpSync) _getNetworkManagerInstance()._onNtpSync();
    }

#elif defined(ARDUINO_ARCH_ESP8266)
    /**
     * @brief Time-set callback registered via settimeofday_cb(bool).
     *
     * settimeofday_cb() fires on ANY wall-clock set — an SNTP update or a manual
     * settimeofday()/RTC restore. The bool overload distinguishes the source, so
     * onNtpSync is invoked only for SNTP-sourced updates.
     *
     * @param fromSntp true if the time was set by the SNTP client.
     */
    static void _ntpSyncCallback(bool fromSntp) {
        if (!fromSntp) return;
        _getNetworkManagerInstance()._timeValid = true;
        if (_getNetworkManagerInstance()._onNtpSync) _getNetworkManagerInstance()._onNtpSync();
    }
#endif

#if defined(ARDUINO_ARCH_AVR)
    // -------------------------------------------------------------------------
    // AVR minimal SNTP client
    //
    // AVR has no SDK SNTP stack, so NetworkManager runs its own tiny client over
    // EthernetUDP, driven from update() on the loop task. It uses one server
    // (profile index 0) and opens the UDP socket only for the duration of a
    // sync, closing it afterwards so it never permanently occupies one of the
    // W5100/W5500's few hardware sockets. The request-build and reply-parse
    // halves are pure logic and are host-tested.
    // -------------------------------------------------------------------------

    /** @brief Builds a 48-byte client-mode SNTP request. */
    static void _ntpBuildRequest(uint8_t b[48]) {
        memset(b, 0, 48);
        b[0]  = 0xE3;   // LI=3 (clock unsynchronised), VN=4, Mode=3 (client)
        b[1]  = 0x00;   // Stratum (unspecified)
        b[2]  = 0x06;   // Polling interval
        b[3]  = 0xEC;   // Peer clock precision
        b[12] = 0x31; b[13] = 0x4E; b[14] = 0x31; b[15] = 0x34;  // Reference ID
    }

    /**
     * @brief Extracts the Unix epoch from a 48-byte SNTP reply.
     *
     * The transmit timestamp (seconds since 1900) is at bytes 40..43, big-endian.
     *
     * @return Seconds since 1970, or 0 for a pre-1970 / invalid timestamp.
     */
    static uint32_t _ntpParseEpoch(const uint8_t b[48]) {
        uint32_t secs1900 = ((uint32_t)b[40] << 24) | ((uint32_t)b[41] << 16)
                          | ((uint32_t)b[42] << 8)  |  (uint32_t)b[43];
        const uint32_t SEVENTY_YEARS = 2208988800UL;   // 1900 -> 1970 offset
        if (secs1900 < SEVENTY_YEARS) return 0;
        return secs1900 - SEVENTY_YEARS;
    }

    /**
     * @brief Resolves getNtp(0) into _avrNtpServerIp (literal IP or DNS lookup).
     *
     * DNS resolution via the Ethernet stack is blocking, so the result is cached
     * (_avrNtpServerKnown) and only re-resolved when the active adapter changes.
     *
     * @return true on success.
     */
    bool _avrNtpResolveServer() {
        char host[Host::MAX_FQDN_SIZE];
        _avrNtpProfile->getNtp(0, host, Host::MAX_FQDN_SIZE);
        if (host[0] == '\0') return false;

        // Literal IP: resolve locally, without ever touching DNSClient. This
        // guarantees a literal-IP NTP server never blocks update()/loop() — the
        // recommended AVR configuration — independent of the DNS library.
        uint8_t oct[4];
        if (Host::parseIp(host, oct)) {
            _avrNtpServerIp    = IPAddress(oct[0], oct[1], oct[2], oct[3]);
            _avrNtpServerKnown = true;
            return true;
        }

        // FQDN: a real DNS lookup. DNSClient::getHostByName() is BLOCKING, so
        // update() (hence loop()) stalls for the query round-trip — or the DNS
        // library's timeout if the server is unreachable — on the first sync
        // after each connect. The result is cached until the adapter changes.
        DNSClient dns;
        dns.begin(Ethernet.dnsServerIP());
        IPAddress ip;
        if (dns.getHostByName(host, ip) == 1) {
            _avrNtpServerIp    = ip;
            _avrNtpServerKnown = true;
            return true;
        }
        return false;
    }

    /**
     * @brief Advances the post-resolution retry backoff (base, then doubling to
     *        the cap). Only used for failures *after* the server address is
     *        known, so a blocking DNS resolve is never fast-retried; a
     *        successful sync resets it to 0. Mirrors lwIP's SNTP backoff on ESP.
     */
    void _avrNtpBackoff() {
        uint32_t next = _avrNtpRetryInterval
                      ? (_avrNtpRetryInterval << 1)
                      : (uint32_t)NETWORK_MANAGER_DEFAULT_NTP_RETRY_INTERVAL;
        if (next > (uint32_t)NETWORK_MANAGER_DEFAULT_NTP_RETRY_MAX)
            next = (uint32_t)NETWORK_MANAGER_DEFAULT_NTP_RETRY_MAX;
        _avrNtpRetryInterval = next;
    }

    /** @brief Non-blocking SNTP client state machine; call once per update(). */
    void _avrNtpPoll() {
        if (_avrNtpProfile == nullptr) return;  // NTP not configured on active adapter

        // Link-driven gating: on W5500 the adapter leaves CONNECTED as soon as
        // linkStatus() reports LinkOFF, so _getConnectedIndex() < 0 means "no
        // live link" within one update() tick — far faster than the manager's
        // 60 s DISCONNECTED event. While there is no link, pause and arm a fresh
        // sync (re-resolve, reset backoff) so it fires immediately once the link
        // returns. W5100 has no link detection, so this branch never triggers
        // there and the client keeps trying — an accepted W5100 compromise.
        if (_getConnectedIndex() < 0) {
            if (_avrNtpState == AvrNtpState::WAIT_REPLY) _avrNtpUdp.stop();
            _avrNtpState         = AvrNtpState::IDLE;
            _avrNtpServerKnown   = false;   // re-resolve on the restored network
            _avrNtpRetryInterval = 0;       // fresh backoff after the outage
            _avrNtpLastSyncMs    = 0;        // 0 -> due immediately
            _avrNtpDue           = true;     // fire as soon as the link is back
            return;
        }

        const uint32_t now = millis();

        // Interval check: schedule a fresh sync once the due delay has elapsed
        // since the last attempt. Normally that is the full sync interval; while
        // in post-resolution retry backoff it is the (shorter) retry delay. The
        // first sync after connect is already armed by _configureNtp.
        const uint32_t dueAfter =
            _avrNtpRetryInterval ? _avrNtpRetryInterval : _ntpSyncInterval;
        if (!_avrNtpDue && _avrNtpState == AvrNtpState::IDLE &&
            _avrNtpLastSyncMs != 0 &&
            (now - _avrNtpLastSyncMs) >= dueAfter) {
            _avrNtpDue = true;
        }

        switch (_avrNtpState) {
            case AvrNtpState::IDLE: {
                if (!_avrNtpDue) return;

                if (!_avrNtpServerKnown && !_avrNtpResolveServer()) {
                    // Resolution failed. NOT a backoff case: AVR DNS is blocking,
                    // so wait the full sync interval before re-resolving
                    // (_avrNtpRetryInterval stays 0) rather than fast-retrying.
                    _avrNtpDue        = false;
                    _avrNtpLastSyncMs = now;
                    return;
                }

                uint8_t pkt[48];
                _ntpBuildRequest(pkt);
                if (!_avrNtpUdp.begin(AVR_NTP_LOCAL_PORT)) {
                    _avrNtpBackoff();               // post-resolve failure
                    _avrNtpDue        = false;
                    _avrNtpLastSyncMs = now;
                    return;
                }
                _avrNtpUdp.beginPacket(_avrNtpServerIp, AVR_NTP_SERVER_PORT);
                _avrNtpUdp.write(pkt, 48);
                _avrNtpUdp.endPacket();
                _avrNtpReqSentMs = now;
                _avrNtpState     = AvrNtpState::WAIT_REPLY;
                return;
            }

            case AvrNtpState::WAIT_REPLY: {
                if (_avrNtpUdp.parsePacket() >= 48) {
                    // Accept only a reply from the exact address we queried.
                    // The server FQDN is resolved to one IP at send time, so
                    // this is a plain source match, not a reverse-DNS check
                    // (pool addresses don't reverse-map to the pool name). A
                    // packet from any other source is discarded while we keep
                    // waiting for the real reply until the timeout — a stray or
                    // spoofed datagram can neither be accepted nor consume the
                    // sync attempt.
                    if (_avrNtpUdp.remoteIP() != _avrNtpServerIp ||
                        _avrNtpUdp.remotePort() != AVR_NTP_SERVER_PORT) return;

                    uint8_t pkt[48];
                    _avrNtpUdp.read(pkt, 48);
                    const uint32_t epoch = _ntpParseEpoch(pkt);
                    _avrNtpUdp.stop();           // release the socket between syncs
                    _avrNtpState = AvrNtpState::IDLE;
                    _avrNtpDue   = false;
                    if (epoch != 0) {
                        _syncEpoch           = epoch;
                        _syncMillis          = millis();
                        _avrNtpLastSyncMs    = _syncMillis;
                        _avrNtpRetryInterval = 0;        // success -> back to full interval
                        _timeValid           = true;
                        if (_onNtpSync) _onNtpSync();
                    } else {
                        _avrNtpBackoff();                // got a reply but bad stamp
                        _avrNtpLastSyncMs = millis();
                    }
                    return;
                }
                if ((now - _avrNtpReqSentMs) >= AVR_NTP_REPLY_TIMEOUT_MS) {
                    _avrNtpUdp.stop();
                    _avrNtpBackoff();                   // no reply in time
                    _avrNtpState      = AvrNtpState::IDLE;
                    _avrNtpDue        = false;
                    _avrNtpLastSyncMs = now;
                }
                return;
            }
        }
    }
#endif
#endif
};

/**
 * @brief Global accessor for the NetworkManagerClass singleton.
 *
 * Implemented as a macro so it compiles under C++11 (AVR / avr-gcc 7.x) as
 * well as C++17. The underlying function returns a function-local static, so
 * there is exactly one instance regardless of how many translation units
 * include this header — no ODR violation, no initialisation-order issues.
 *
 * Use directly in sketches:
 * @code
 * NetworkManager.addAdapter(ethAdapter);
 * NetworkManager.onNtpSync([]() { Serial.println("Time synced"); });
 * NetworkManager.begin();
 * @endcode
 */
#define NetworkManager (NetworkManagerClass::_getNetworkManagerInstance())

// ESP8266 SNTP poll-interval control lives in the separate, opt-in header
// NetworkManagerNtpSyncHook.h (include it in exactly one translation unit to
// let setNtpSyncInterval()/NETWORK_MANAGER_DEFAULT_NTP_SYNC_INTERVAL drive the
// cadence). See the note on setNtpSyncInterval(). This indirection is an
// ESP8266/lwip2 SDK quirk; ESP32 and AVR need nothing extra.

// Setting NTP poll-interval is always available on ESP32 and AVR
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_AVR)
#define NETWORK_MANAGER_NTP_SYNC_INTERVAL_SETTER 1
#endif
#endif