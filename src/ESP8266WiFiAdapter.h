/*
 * Header-only: the Arduino IDE compiles libraries separately from the sketch, so
 * a precompiled unit would not see the configuration macros you define — being
 * header-only, the library is compiled with each includer's macros instead.
 * In a multi-file project, define those macros globally so every translation
 * unit agrees (see the README, "Where to define these").
 */

#pragma once

#if defined(ARDUINO_ARCH_ESP8266)

#include "NetworkAdapter.h"
#include "WiFiProfile.h"
#include <ESP8266WiFi.h>
#include <lwip/netif.h>   // netif_set_default() — re-point the default route

#ifndef ESP8266_WIFI_ADAPTER_DHCP_TIMEOUT
#define ESP8266_WIFI_ADAPTER_DHCP_TIMEOUT 15000
#endif

/**
 * @brief NetworkAdapter implementation for ESP8266 WiFi (STA mode).
 *
 * Event-driven — uses WiFi.onStationModeGotIP() and related callbacks
 * internally. The update() method is lightweight and only checks DHCP timeout.
 *
 * DHCP timeout:
 *   If no IP is obtained within ESP8266_WIFI_ADAPTER_DHCP_TIMEOUT milliseconds
 *   after start(), the adapter transitions to FAILED.
 *   Override ESP8266_WIFI_ADAPTER_DHCP_TIMEOUT in platformio.ini or before
 *   including this header (default: 15000 ms).
 *
 * @note Only one ESP8266WiFiAdapter instance may be active at a time,
 *       as the ESP8266 supports a single STA interface. start() returns
 *       false if another instance is already active.
 */
class ESP8266WiFiAdapter : public NetworkAdapter {
public:
    /** @brief DHCP acquisition timeout in milliseconds. */
    static constexpr uint32_t DHCP_TIMEOUT = ESP8266_WIFI_ADAPTER_DHCP_TIMEOUT;

    /**
     * @brief Constructs an ESP8266WiFiAdapter with an associated WiFiProfile.
     * @param profile Reference to the WiFiProfile describing this interface.
     */
    explicit ESP8266WiFiAdapter(WiFiProfile& profile)
        : NetworkAdapter(profile)
        , _wifiProfile(profile)
        , _connectingStart(0)
    {}

    // -------------------------------------------------------------------------
    // NetworkAdapter interface
    // -------------------------------------------------------------------------

    /**
     * @brief Starts the WiFi connection in STA mode.
     *
     * Reads SSID, password, hostname and TX power from the profile.
     * Registers internal event callbacks if not already registered.
     * Transitions to CONNECTING.
     *
     * @return true if SSID is non-empty and WiFi.begin() was called,
     *         false if the profile has no SSID or another instance is active.
     */
    bool start() override {
        if (_activeCount > 0) return false;

        // Profile must have a valid SSID
        char ssid[WiFiProfile::MAX_SSID_SIZE];
        if (!_wifiProfile.getSsid(ssid, sizeof(ssid))) return false;
        if (ssid[0] == '\0') return false;

        // Register event callbacks once
        if (!_eventRegistered) {
            _gotIpHandler = WiFi.onStationModeGotIP(_onGotIp);
            _disconnectedHandler = WiFi.onStationModeDisconnected(_onDisconnected);
            _eventRegistered = true;
        }

        // NetworkManager is the sole authority on retry/fallback timing —
        // disable the ESP8266 core's own reconnect so it cannot race with
        // our state machine (mirrors the ESP32 adapter's approach).
        WiFi.setAutoReconnect(false);

        // Apply the cached TX power (initialised from the profile, overridable
        // at runtime via setTxPower()); re-applied here so it survives reconnects.
        _applyTxPower();

        // Set hostname before connect
        char hostname[NetworkProfile::MAX_HOSTNAME_SIZE];
        _wifiProfile.getHostname(hostname, sizeof(hostname));
        WiFi.hostname(hostname);

        // Apply static IP if DHCP disabled.
        // Note: WiFiProfile::getConfig() only overloads WiFiConfig, not the
        // base NetworkConfig — it must be used here even though only the
        // base (IP/DHCP) fields are read.
        WiFiProfile::WiFiConfig cfg;
        _wifiProfile.getConfig(cfg);
        if (!cfg.dhcp) {
            WiFi.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0]);
        }

        // Start STA
        WiFi.mode(WIFI_STA);

        char password[WiFiProfile::MAX_PASSWORD_SIZE];
        _wifiProfile.getPassword(password, sizeof(password));
        WiFi.begin(ssid, password[0] != '\0' ? password : nullptr);

        _instance        = this;
        _activeCount++;
        _connectingStart = millis();
        _setState(State::CONNECTING);
        return true;
    }

    /**
     * @brief Disconnects WiFi and transitions to IDLE.
     *
     * Calls WiFi.disconnect() and resets the DHCP timer.
     */
    void stop() override {
        // Transition to IDLE first — mirrors the ESP32 adapters. Although the
        // ESP8266 is single-threaded, _setState(IDLE) before WiFi.disconnect()
        // ensures that the _onDisconnected callback (which may fire synchronously
        // during disconnect on some core versions) sees IDLE and returns early
        // via its state guard, rather than re-transitioning to FAILED.
        _setState(State::IDLE);
        _clearStatus();
        WiFi.setAutoReconnect(false);
        if (_instance == this) _instance = nullptr;
        if (_activeCount > 0) _activeCount--;
        _connectingStart = 0;
        WiFi.disconnect();
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
    IPAddress getLocalIP() const override { return WiFi.localIP(); }

    /**
     * @brief Makes the station interface the default route (ESP8266).
     *
     * lwIP keeps a single netif_default. The wired driver marks its netif
     * LINK_UP statically and only expresses a default preference; stopping it
     * does not hand the default to the station, so after a fallback the default
     * can stay on the now-unused wired netif and WiFi has no route out. The
     * station netif is identified by its IP, independent of core netif naming.
     */
    void setDefaultRoute() override {
        IPAddress ip = WiFi.localIP();
        if ((uint32_t)ip == 0) return;
        for (netif* n = netif_list; n != nullptr; n = n->next) {
            if (ip4_addr_get_u32(ip_2_ip4(&n->ip_addr)) == (uint32_t)ip) {
                netif_set_default(n);
                return;
            }
        }
    }

    /**
     * @brief Sets the WiFi transmit power in dBm at runtime.
     *
     * Writes through to the profile (validated + lock-protected) and applies it
     * immediately, so it also survives reconnects (start() re-applies the profile
     * value). Persist it across reboots by saving the profile.
     *
     * @return false if @p dBm is not accepted by the platform (nothing changed).
     */
    bool setTxPower(float dBm) {
        if (!_wifiProfile.setTxPower(dBm)) return false;   // validated + stored in the profile
        _applyTxPower();
        return true;
    }

    /** @brief Configured TX power in dBm (from the profile), or NaN for the hardware default. */
    float getTxPower() const { return _wifiProfile.getTxPower(); }

    /** @brief Current RSSI in dBm (only meaningful while connected). */
    int getRssi() const { return WiFi.RSSI(); }

private:
    WiFiProfile& _wifiProfile;     ///< Typed reference to the WiFi profile.
    uint32_t     _connectingStart; ///< millis() timestamp when CONNECTING began.
    /** @brief Applies the profile's TX power to the radio (no-op when NaN). */
    void _applyTxPower() {
        const float dBm = _wifiProfile.getTxPower();
        if (isnan(dBm)) return;
        WiFi.setOutputPower(dBm);
    }

    static ESP8266WiFiAdapter* _instance;      ///< Active adapter instance (singleton).
    static uint8_t             _activeCount;   ///< Number of active instances (max 1).
    static bool                _eventRegistered; ///< true if callbacks were registered.

    // Event callback handles — needed for unregistering on stop()
    static WiFiEventHandler _gotIpHandler;
    static WiFiEventHandler _disconnectedHandler;

    /**
     * @brief Called when the station obtains an IP address.
     * @param event Event data containing the assigned IP.
     */
    static void _onGotIp(const WiFiEventStationModeGotIP& /*event*/) {
        if (!_instance) return;
        _instance->_connectingStart = 0;
        // Capture the live snapshot before the CONNECTED event is delivered, so
        // an app event handler that reads getStatus() already sees valid data.
        _instance->_cacheStatus(_instance->_buildStatus());
        _instance->_setState(State::CONNECTED);
    }

    /**
     * @brief Called when the station disconnects.
     *
     * Only transitions to FAILED when the adapter is currently CONNECTED
     * or CONNECTING — this event also fires as a side effect of our own
     * stop() (WiFi.disconnect()), at which point the adapter is already
     * IDLE and the event must be ignored. Without this check, stop()
     * followed by this callback would immediately re-mark a deliberately
     * stopped adapter as FAILED, which NetworkManager would then try to
     * restart. Mirrors the equivalent check in ESP32WiFiAdapter.
     *
     * @param event Event data containing the disconnect reason.
     */
    static void _onDisconnected(const WiFiEventStationModeDisconnected& /*event*/) {
        if (!_instance) return;
        if (_instance->getState() == State::CONNECTED ||
            _instance->getState() == State::CONNECTING) {
            _instance->_clearStatus();
            _instance->_setState(State::FAILED);
        }
    }

    /** @brief Builds a live status snapshot from the WiFi interface. */
    NetworkStatus _buildStatus() const {
        NetworkStatus s;
        s.interfaceType = getProfile().getInterfaceType();
        s.connected     = true;
        s.localIP       = WiFi.localIP();
        s.subnetMask    = WiFi.subnetMask();
        s.gateway       = WiFi.gatewayIP();
        for (uint8_t i = 0; i < NetworkProfile::DNS_SERVER_COUNT; i++) {
            s.dns[i] = WiFi.dnsIP(i);
        }
        return s;
    }
};

// -----------------------------------------------------------------------------
// Static member definitions
// -----------------------------------------------------------------------------

ESP8266WiFiAdapter* ESP8266WiFiAdapter::_instance         = nullptr;
uint8_t             ESP8266WiFiAdapter::_activeCount      = 0;
bool                ESP8266WiFiAdapter::_eventRegistered  = false;
WiFiEventHandler    ESP8266WiFiAdapter::_gotIpHandler;
WiFiEventHandler    ESP8266WiFiAdapter::_disconnectedHandler;

#endif // ARDUINO_ARCH_ESP8266