/*
 * Header-only: the Arduino IDE compiles libraries separately from the sketch, so
 * a precompiled unit would not see the configuration macros you define — being
 * header-only, the library is compiled with each includer's macros instead.
 * In a multi-file project, define those macros globally so every translation
 * unit agrees (see the README, "Where to define these").
 */

#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include "NetworkAdapter.h"
#include "WiFiProfile.h"
#include <WiFi.h>
#include <Network.h>
#include <atomic>
#include <cmath>

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
#include <esp_sntp.h>
#endif

#ifndef ESP32_WIFI_ADAPTER_DHCP_TIMEOUT
#define ESP32_WIFI_ADAPTER_DHCP_TIMEOUT 15000
#endif

/**
 * @brief NetworkAdapter implementation for ESP32 WiFi (STA mode).
 *
 * Event-driven — uses Network.onEvent() internally. The update() method
 * is lightweight and only checks DHCP timeout.
 *
 * DHCP timeout:
 *   If no IP is obtained within ESP32_WIFI_ADAPTER_DHCP_TIMEOUT milliseconds
 *   after the link comes up, the adapter transitions to FAILED.
 *   Override ESP32_WIFI_ADAPTER_DHCP_TIMEOUT in platformio.ini or before
 *   including this header (default: 15000 ms).
 *
 * @note Only one ESP32WiFiAdapter instance may be active at a time,
 *       as the ESP32 supports a single STA interface. start() returns
 *       false if another instance is already active.
 */
class ESP32WiFiAdapter : public NetworkAdapter {
public:
    /** @brief DHCP acquisition timeout in milliseconds. */
    static constexpr uint32_t DHCP_TIMEOUT = ESP32_WIFI_ADAPTER_DHCP_TIMEOUT;

    /**
     * @brief Constructs an ESP32WiFiAdapter with an associated WiFiProfile.
     * @param profile Reference to the WiFiProfile describing this interface.
     */
    explicit ESP32WiFiAdapter(WiFiProfile& profile)
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
     * Registers the internal event handler if not already registered.
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

        // Register event handler once
        if (!_eventRegistered) {
            Network.onEvent(_eventHandler);
            _eventRegistered = true;
        }

        // Start STA
        WiFi.setAutoReconnect(false);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);

        char password[WiFiProfile::MAX_PASSWORD_SIZE];
        _wifiProfile.getPassword(password, sizeof(password));
        WiFi.begin(ssid, password[0] != '\0' ? password : nullptr);

        // TX power must be applied after begin()
        // Apply the cached TX power (initialised from the profile, overridable
        // at runtime via setTxPower()); re-applied here so it survives reconnects.
        _applyTxPower();

        _instance        = this;
        _activeCount++;
        _connectingStart = millis();
        _setState(State::CONNECTING);
        return true;
    }

    /**
     * @brief Disconnects WiFi and transitions to IDLE.
     *
     * Calls WiFi.disconnect(true) and resets the DHCP timer.
     */
    void stop() override {
        // Transition to IDLE first so that any asynchronous
        // WIFI_STA_DISCONNECTED / WIFI_STA_LOST_IP events fired during
        // teardown are rejected by the state guard in _eventHandler() before
        // any other teardown step runs.
        _setState(State::IDLE);
        _clearStatus();
        WiFi.setAutoReconnect(false);
        _connectingStart = 0;
        if (_activeCount > 0) _activeCount--;
        if (_instance == this) _instance = nullptr;
        WiFi.disconnect(true);
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

    /** @brief Makes this interface the default route. */
    void setDefaultRoute() override { WiFi.STA.setDefault(); }

    /**
     * @brief Sets the WiFi transmit power in dBm at runtime.
     *
     * Writes through to the profile (validated + lock-protected) and applies it
     * immediately, so it also survives reconnects (start() re-applies the profile
     * value). Persist it across reboots by saving the profile. The value is
     * quantised to the radio's step; getTxPower() returns the configured dBm.
     *
     * @return false if @p dBm is outside the valid range (nothing changed).
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
        WiFi.setTxPower(static_cast<wifi_power_t>(
            dBm * WiFiProfile::WIFI_TX_POWER_MULTIPLIER));
    }

    // _instance and _activeCount are written by start()/stop() on the loop task
    // and read by _eventHandler() on the ESP32 network event task. Declared
    // atomic to guarantee visibility across tasks without a mutex.
    static std::atomic<ESP32WiFiAdapter*> _instance;    ///< Active adapter instance (singleton).
    static std::atomic<uint8_t>           _activeCount; ///< Number of active instances (max 1).
    static bool                           _eventRegistered; ///< true if Network.onEvent() was called.

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

    /**
     * @brief Internal ESP32 network event handler.
     *
     * Registered once via Network.onEvent(). Handles:
     *   ARDUINO_EVENT_WIFI_STA_START       → sets hostname
     *   ARDUINO_EVENT_WIFI_STA_CONNECTED   → applies static IP if DHCP disabled
     *   ARDUINO_EVENT_WIFI_STA_GOT_IP      → transitions to CONNECTED
     *   ARDUINO_EVENT_WIFI_STA_LOST_IP     → transitions to FAILED
     *   ARDUINO_EVENT_WIFI_STA_DISCONNECTED → transitions to FAILED
     *
     * @param event ESP32 Arduino core event ID.
     * @param info  Event-specific data.
     */
    static void _eventHandler(arduino_event_id_t event,
                               arduino_event_info_t info) {
        // Load once into a local pointer: std::atomic<T*> does not provide
        // operator->(), and loading once also avoids a TOCTOU race where
        // _instance could be set to nullptr between the guard check and use.
        ESP32WiFiAdapter* inst = _instance.load();
        if (!inst) return;

        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_START: {
                // Hostname must be set before DHCP
                char hostname[NetworkProfile::MAX_HOSTNAME_SIZE];
                inst->_wifiProfile.getHostname(hostname, sizeof(hostname));
                WiFi.setHostname(hostname);
                break;
            }
            case ARDUINO_EVENT_WIFI_STA_CONNECTED: {
                // Apply static IP if DHCP is disabled
                // Note: getConfig() also copies NTP fields — acceptable overhead here.
                WiFiProfile::WiFiConfig cfg;
                inst->_wifiProfile.getConfig(cfg);
                if (!cfg.dhcp) {
                    if (NetworkProfile::DNS_SERVER_COUNT == 2) {
                        WiFi.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0], cfg.dns[1]);
                    } else {
                        WiFi.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0]);
                    }
                } else {
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
                    esp_sntp_servermode_dhcp(!inst->_wifiProfile.isConfiguredNtp());
#endif
                }
                break;
            }
            case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
                // _connectingStart is intentionally NOT cleared here. update()
                // only reads it in CONNECTING state, and we are leaving that
                // state now; start() always sets it fresh. Not writing it from
                // the event task keeps _connectingStart single-writer (loop task
                // only), which removes a cross-task data race without an atomic.
                inst->_cacheStatus(inst->_buildStatus());
                inst->_setState(State::CONNECTED);
                break;
            }
            case ARDUINO_EVENT_WIFI_STA_LOST_IP: {
                if (inst->getState() == State::CONNECTED) {
                    inst->_clearStatus();
                    inst->_setState(State::FAILED);
                }
                break;
            }
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
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

std::atomic<ESP32WiFiAdapter*> ESP32WiFiAdapter::_instance        { nullptr };
std::atomic<uint8_t>           ESP32WiFiAdapter::_activeCount     { 0 };
bool                           ESP32WiFiAdapter::_eventRegistered = false;

#endif // ARDUINO_ARCH_ESP32