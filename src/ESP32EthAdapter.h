/*
 * Header-only: the Arduino IDE compiles libraries separately from the sketch, so
 * a precompiled unit would not see the configuration macros you define — being
 * header-only, the library is compiled with each includer's macros instead.
 * In a multi-file project, define those macros globally so every translation
 * unit agrees (see the README, "Where to define these").
 */

#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#ifndef ETH_PHY_TYPE
#error "Missing ETH_PHY_* definitions. Please specify your board or custom PHY settings."
#endif
#include <ETH.h>

#include "NetworkAdapter.h"
#include "EthProfile.h"
#include <Network.h>
#include <atomic>
#include <esp_netif.h>

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
#include <esp_sntp.h>
#endif

#ifndef ESP32_ETH_ADAPTER_DHCP_TIMEOUT
#define ESP32_ETH_ADAPTER_DHCP_TIMEOUT 15000
#endif

/**
 * @brief NetworkAdapter implementation for ESP32 Ethernet (SPI or RMII PHY).
 *
 * Event-driven — uses Network.onEvent() internally. The update() method
 * is lightweight and only checks DHCP timeout.
 *
 * ## Driver lifecycle
 *
 * ETH.begin() initialises the SPI bus and PHY driver. Calling ETH.end()
 * followed by ETH.begin() causes SPI re-initialisation errors. To avoid
 * this, stop() performs only a *logical* teardown: it stops the DHCP
 * client, resets the routing priority to 0 (so the inactive interface
 * cannot become the default route or overwrite the active interface's DNS
 * and gateway settings), and clears the singleton pointer — but does NOT
 * call ETH.end(). ETH.begin() is therefore called only once, guarded by
 * _ethStarted.
 *
 * ## Routing and DNS
 *
 * lwIP stores DNS server addresses globally. If the ETH DHCP client runs
 * while a higher-priority WiFi interface is active, a DHCP lease renewal
 * on ETH can overwrite the DNS and default gateway. To prevent this,
 * stop() calls esp_netif_dhcpc_stop() and sets the route priority to 0.
 * start() restores them: esp_netif_dhcpc_start() for DHCP profiles, or
 * ETH.config() for static IP, plus setRoutePrio(255 - priority).
 *
 * setRoutePrio() requires ESP-IDF >= 5.5.0. On earlier versions the call
 * is compiled out; DNS protection relies solely on esp_netif_dhcpc_stop().
 *
 * ## DHCP timeout
 *
 * If no IP is obtained within ESP32_ETH_ADAPTER_DHCP_TIMEOUT milliseconds
 * after CONNECTING, the adapter transitions to FAILED. Override the macro
 * in platformio.ini or before including this header (default: 15000 ms).
 *
 * @note Only one ESP32EthAdapter instance may be active at a time.
 *       start() returns false if another instance is already active.
 */
class ESP32EthAdapter : public NetworkAdapter {
public:
    /** @brief DHCP acquisition timeout in milliseconds. */
    static constexpr uint32_t DHCP_TIMEOUT = ESP32_ETH_ADAPTER_DHCP_TIMEOUT;

    /**
     * @brief Constructs an ESP32EthAdapter with an associated EthProfile.
     * @param profile Reference to the EthProfile describing this interface.
     */
    explicit ESP32EthAdapter(EthProfile& profile)
        : NetworkAdapter(profile)
        , _ethProfile(profile)
        , _connectingStart(0)
    {}

    // -------------------------------------------------------------------------
    // NetworkAdapter interface
    // -------------------------------------------------------------------------

    /**
     * @brief Starts the Ethernet connection.
     *
     * Calls ETH.begin() only on the first invocation (_ethStarted flag).
     * Subsequent calls skip ETH.begin() to avoid SPI re-initialisation
     * errors — the driver continues running across logical stop/start cycles.
     *
     * Configures DHCP or static IP according to the profile, sets the
     * routing priority, and transitions to CONNECTING.
     *
     * @return true if started successfully, false if another instance is
     *         already active or ETH.begin() fails on first call.
     */
    bool start() override {
        if (_activeCount > 0) return false;

        // Register event handler once for the lifetime of the adapter.
        if (!_eventRegistered) {
            Network.onEvent(_eventHandler);
            _eventRegistered = true;
        }

        // Set _instance before ETH.begin() so that events fired during
        // initialisation (ETH_START, ETH_CONNECTED) are not missed by
        // the event handler's guard check.
        _instance        = this;
        _activeCount++;
        _connectingStart = millis();
        _setState(State::CONNECTING);
        _applyRoutePrio();

        // ETH.begin() initialises the SPI bus and PHY. Calling it again
        // after ETH.end() causes SPI re-initialisation errors, so we call
        // it only once and keep the driver running across stop/start cycles.
        if (!_ethStarted) {
            if (!ETH.begin()) {
                // Undo setup if ETH.begin() fails.
                _instance        = nullptr;
                _activeCount--;
                _connectingStart = 0;
                _setState(State::IDLE);
                return false;
            }
            _ethStarted = true;
        } else {
            // On subsequent starts, the driver is already running but the DHCP
            // client was stopped in stop(). If the physical link is already up,
            // ETH_CONNECTED will not fire again (the link state has not changed),
            // so we must apply the IP configuration here directly, mirroring
            // the ETH_CONNECTED handler logic.
            NetworkProfile::NetworkConfig cfg;
            _ethProfile.getConfig(cfg);
            if (!cfg.dhcp) {
                esp_netif_t* netif = ETH.netif();
                if (netif) esp_netif_dhcpc_stop(netif);
                if (NetworkProfile::DNS_SERVER_COUNT == 2) {
                    ETH.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0], cfg.dns[1]);
                } else {
                    ETH.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0]);
                }
            } else {
                esp_netif_t* netif = ETH.netif();
                if (netif) esp_netif_dhcpc_start(netif);
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
                if (ETH.linkUp()) {
                    esp_sntp_servermode_dhcp(!_ethProfile.isConfiguredNtp());
                }
#endif
            }
        }

        return true;
    }

    /**
     * @brief Logically stops the Ethernet adapter.
     *
     * Does NOT call ETH.end() — see class documentation for why. Instead:
     *   1. Transitions to IDLE first so that async ETH events arriving
     *      during or after teardown are rejected by the state guards.
     *   2. Stops the DHCP client to prevent background lease renewals from
     *      overwriting the active interface's DNS and gateway settings.
     *   3. Resets the routing priority to 0 so the inactive ETH interface
     *      cannot become the default route while another interface is active.
     *   4. Clears _instance and decrements _activeCount.
     */
    void stop() override {
        _setState(State::IDLE);
        _clearStatus();
        _connectingStart = 0;

        // Stop the DHCP client so background lease renewals cannot overwrite
        // the active interface's DNS and default gateway (lwIP stores these
        // globally, not per-interface).
        // Also explicitly zero the IP info: without this, esp_netif_dhcpc_start()
        // on the next start() call would wait for the IP lost timer
        // (CONFIG_ESP_NETIF_IP_LOST_TIMER_INTERVAL, default 120 s) before
        // issuing a fresh DHCP request, causing a long restore delay.
        // Zeroing ip_info here bypasses the timer and allows immediate DHCP
        // discovery when the adapter is restarted.
        esp_netif_t* netif = ETH.netif();
        if (netif) {
            esp_netif_dhcpc_stop(netif);
            esp_netif_ip_info_t ip_info = {};
            esp_netif_set_ip_info(netif, &ip_info);
        }

        // Reset routing priority so this interface is not selected as the
        // default route while logically stopped.
        _resetRoutePrio();

        if (_activeCount > 0) _activeCount--;
        if (_instance == this) _instance = nullptr;

        // Set _lastFailedMs so canProbe() enforces the retry interval even
        // after a clean stop (e.g. RESTORED). Without this, the ETH adapter
        // would restart immediately after being stopped, causing a spurious
        // FALLBACK event while another interface is still active.
        _lastFailedMs.store(millis());

        // Intentionally no ETH.end() — see class documentation.
    }
    /**
     * @brief Returns true if the Ethernet adapter is a valid probe candidate.
     *
     * On the first call (driver not yet started), always allows the probe so
     * that ETH.begin() can be called to initialise the driver and detect the
     * link state. After the first start, gates the probe on the physical link
     * state: if the cable is not connected there is nothing to connect to, and
     * the probe fires immediately once the link comes up — no retry interval
     * delay — because the link state is already a reliable signal.
     *
     * @return true if the adapter is IDLE and ready to probe.
     */
    bool canProbe() const override {
        if (getState() != State::IDLE) return false;
        if (!_ethStarted) return true;  // first start: driver not yet running
        // After a stop() or FAILED, enforce the retry interval so the adapter
        // does not restart immediately (e.g. right after RESTORED stops it).
        uint32_t last = _lastFailedMs.load();
        if (last != 0 && millis() - last < RETRY_INTERVAL) return false;
        return ETH.linkUp();
    }

    /**
     * @brief Checks DHCP timeout.
     *
     * If the adapter is in CONNECTING state and no IP has been obtained
     * within DHCP_TIMEOUT milliseconds, transitions to FAILED.
     */
    void update() override {
        if (getState() != State::CONNECTING) return;
        if (millis() - _connectingStart > DHCP_TIMEOUT) {
            _setState(State::FAILED);
        }
    }

    /** @brief Returns the actual IP address of the adapter. */
    IPAddress getLocalIP() const override { return ETH.localIP(); }

    /** @brief Makes this interface the default route. */
    void setDefaultRoute() override { ETH.setDefault(); }

private:
    EthProfile& _ethProfile;      ///< Typed reference to the Ethernet profile.
    uint32_t    _connectingStart; ///< millis() timestamp when CONNECTING began.

    // _instance and _activeCount are written by start()/stop() on the loop task
    // and read by _eventHandler() on the ESP32 network event task. Declared
    // atomic to guarantee visibility across tasks without a mutex.
    static std::atomic<ESP32EthAdapter*> _instance;     ///< Active adapter instance (singleton).
    static std::atomic<uint8_t>          _activeCount;  ///< Number of active instances (max 1).
    static bool                          _eventRegistered; ///< true if Network.onEvent() was called.
    static bool                          _ethStarted;   ///< true if ETH.begin() succeeded at least once.

    // -------------------------------------------------------------------------
    // Routing priority helpers
    // -------------------------------------------------------------------------

    /**
     * @brief Sets the lwIP routing priority from the profile.
     *
     * Maps profile priority (0 = highest) to lwIP route_prio (higher = preferred):
     *   route_prio = 255 - profile_priority
     *
     * Requires ESP-IDF >= 5.5.0. On earlier versions the call is a no-op.
     */
    void _applyRoutePrio() const {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
        int prio = 255 - static_cast<int>(_ethProfile.getPriority());
        ETH.setRoutePrio(prio);
#endif
    }

    /**
     * @brief Resets the lwIP routing priority to 0 (lowest).
     *
     * Called by stop() so that a logically inactive ETH interface cannot
     * become the default route.
     *
     * Requires ESP-IDF >= 5.5.0. On earlier versions the call is a no-op.
     */
    void _resetRoutePrio() const {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
        ETH.setRoutePrio(0);
#endif
    }

    // -------------------------------------------------------------------------
    // Event handler
    // -------------------------------------------------------------------------

    /** @brief Builds a live status snapshot from the ETH interface. */
    NetworkStatus _buildStatus() const {
        NetworkStatus s;
        s.interfaceType = getProfile().getInterfaceType();
        s.connected     = true;
        s.localIP       = ETH.localIP();
        s.subnetMask    = ETH.subnetMask();
        s.gateway       = ETH.gatewayIP();
        for (uint8_t i = 0; i < NetworkProfile::DNS_SERVER_COUNT; i++) {
            s.dns[i] = ETH.dnsIP(i);
        }
        return s;
    }

    /**
     * @brief Internal ESP32 network event handler.
     *
     * Registered once via Network.onEvent(). Handles:
     *   ARDUINO_EVENT_ETH_START        → sets hostname
     *   ARDUINO_EVENT_ETH_CONNECTED    → applies IP config (DHCP or static)
     *   ARDUINO_EVENT_ETH_GOT_IP       → transitions to CONNECTED
     *   ARDUINO_EVENT_ETH_LOST_IP      → transitions to FAILED
     *   ARDUINO_EVENT_ETH_DISCONNECTED → transitions to FAILED
     *
     * All state-changing cases are guarded: events that arrive while the
     * adapter is IDLE (logically stopped but driver still running) are
     * silently ignored, preventing spurious state transitions.
     *
     * @param event ESP32 Arduino core event ID.
     * @param info  Event-specific data (unused).
     */
    static void _eventHandler(arduino_event_id_t event,
                               arduino_event_info_t info) {
        // Load once into a local pointer: std::atomic<T*> does not provide
        // operator->(), and a single load also avoids a TOCTOU race where
        // _instance could be set to nullptr between the guard check and use.
        ESP32EthAdapter* inst = _instance.load();
        if (!inst) return;

        switch (event) {
            case ARDUINO_EVENT_ETH_START: {
                // Hostname must be set before DHCP starts.
                char hostname[NetworkProfile::MAX_HOSTNAME_SIZE];
                inst->_ethProfile.getHostname(hostname, sizeof(hostname));
                ETH.setHostname(hostname);
                break;
            }
            case ARDUINO_EVENT_ETH_CONNECTED: {
                // Guard: only apply IP config when we are actively connecting.
                // ETH_CONNECTED may fire while the adapter is logically stopped
                // (IDLE) if the driver detects a link event in the background.
                if (inst->getState() != State::CONNECTING) break;

                NetworkProfile::NetworkConfig cfg;
                inst->_ethProfile.getConfig(cfg);
                if (!cfg.dhcp) {
                    // Static IP: stop DHCP client and configure address.
                    // The DHCP client was already stopped in stop(); this call
                    // is a safety net in case the driver restarted it internally.
                    esp_netif_t* netif = ETH.netif();
                    if (netif) esp_netif_dhcpc_stop(netif);
                    if (NetworkProfile::DNS_SERVER_COUNT == 2) {
                        ETH.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0], cfg.dns[1]);
                    } else {
                        ETH.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0]);
                    }
                }
                // DHCP: client was already started in start(); nothing to do here.
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
                if (cfg.dhcp) {
                    esp_sntp_servermode_dhcp(!inst->_ethProfile.isConfiguredNtp());
                }
#endif
                break;
            }
            case ARDUINO_EVENT_ETH_GOT_IP: {
                // Guard: only accept GOT_IP when actively connecting.
                if (inst->getState() != State::CONNECTING) break;
                // _connectingStart left untouched on purpose — see the matching
                // note in ESP32WiFiAdapter: keeping it single-writer (loop task)
                // avoids a cross-task race without needing an atomic.
                inst->_cacheStatus(inst->_buildStatus());
                inst->_setState(State::CONNECTED);
                break;
            }
            case ARDUINO_EVENT_ETH_LOST_IP: {
                if (inst->getState() == State::CONNECTED) {
                    inst->_clearStatus();
                    inst->_setState(State::FAILED);
                }
                break;
            }
            case ARDUINO_EVENT_ETH_DISCONNECTED: {
                if (inst->getState() == State::CONNECTED ||
                    inst->getState() == State::CONNECTING) {
                    inst->_clearStatus();
                    inst->_setState(State::FAILED);
                }
                break;
            }
            default:
                break;
        }
    }
};

// -----------------------------------------------------------------------------
// Static member definitions
// -----------------------------------------------------------------------------

std::atomic<ESP32EthAdapter*> ESP32EthAdapter::_instance       { nullptr };
std::atomic<uint8_t>          ESP32EthAdapter::_activeCount     { 0 };
bool                          ESP32EthAdapter::_eventRegistered = false;
bool                          ESP32EthAdapter::_ethStarted      = false;

#endif // ARDUINO_ARCH_ESP32