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
#include <esp_wifi.h>     // esp_wifi_get_mac()/esp_wifi_set_mac() — remote on ESP32-P4
#include <esp_netif.h>
#include <atomic>
#include <cmath>

#if NTP_PROFILE_ENABLED
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
 *   The window covers the scan as well as the association and the lease, and
 *   start() scans every channel (see there for why), which costs roughly
 *   1.5-2 s. The default leaves ample room; a build that shortens the timeout
 *   should keep that in mind.
 *
 * MAC address:
 *   start() brings the station's address to the profile's
 *   (getMac(ACTIVE)), writing it only when the two differ — so the radio is
 *   left alone unless an override is set, and what the status reports is what
 *   goes on the air. On an ESP32-P4 the radio is an ESP-Hosted co-processor and
 *   the profile cannot know its address beforehand; start() reads it from the
 *   driver and records it as the factory MAC (WiFiProfile::setFactoryMac()).
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
     * Reads SSID, password, hostname and TX power from the profile, and
     * brings the station MAC to the profile's (see the class description).
     * Registers the internal event handler if not already registered.
     * Transitions to CONNECTING.
     *
     * @return true if SSID is non-empty and WiFi.begin() was called,
     *         false if the profile has no SSID, another instance is active, or
     *         the MAC could not be read or applied.
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

        // Scan every channel before choosing, rather than taking the first
        // matching AP found.
        //
        // The core defaults to WIFI_FAST_SCAN, which stops at the first beacon
        // carrying the right SSID. Its companion setting, sort_method, already
        // defaults to WIFI_CONNECT_AP_BY_SIGNAL — but with one candidate there
        // is nothing to sort, so on a network with several access points the
        // choice is effectively arbitrary. That is fine at home and wrong here:
        // a distant AP may refuse the association outright where a controller
        // enforces a signal floor, and the symptom is a bare
        // WIFI_REASON_ASSOC_FAIL with no hint of its cause. Retrying does not
        // help either, since the next scan is just as arbitrary.
        //
        // The cost is the scan: roughly 1.5-2 s for the 2.4 GHz channels, taken
        // out of the CONNECTING budget below. A library whose purpose is to fail
        // over reliably can afford that far more easily than it can afford
        // associating with the wrong access point.
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);

        // After mode(), which starts the driver (and on a P4 the co-processor
        // link), and before begin(), so the association and the DHCP request
        // already carry the address. Refusing to start is the honest outcome
        // of a failure: the alternative is a status that names one address
        // while the radio sends from another.
        if (!_syncMac()) return false;

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
     * Writes through to the profile (validated + lock-protected) and applies
     * it immediately, so it also survives reconnects (start() re-applies the
     * profile value). Persist it across reboots by saving the profile.
     * On ESP32 a value that is not one of the radio's supported levels is
     * refused rather than adjusted, so getTxPower() always returns exactly
     * what was set.
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

    /**
     * @brief Live link details: SSID, signal strength and the AP's BSSID.
     *
     * Read from the radio, not from the profile: what the device is actually
     * associated with can differ from what was configured, and that difference
     * is the point of a status page.
     */
    bool getLinkInfo(LinkInfo& out) const override {
        if (WiFi.status() != WL_CONNECTED) return false;
        snprintf(out.ssid, sizeof(out.ssid), "%s", WiFi.SSID().c_str());
        out.hasSsid = out.ssid[0] != '\0';
        out.rssi    = (int8_t)WiFi.RSSI();
        out.hasRssi = true;
        const uint8_t* b = WiFi.BSSID();
        if (b) { memcpy(out.bssid, b, sizeof(out.bssid)); out.hasBssid = true; }
        return out.hasSsid || out.hasRssi || out.hasBssid;
    }

private:
    WiFiProfile& _wifiProfile;     ///< Typed reference to the WiFi profile.
    uint32_t     _connectingStart; ///< millis() timestamp when CONNECTING began.
    /**
     * @brief Brings the station MAC to the profile's.
     *
     * Reads the driver's address first. Where the profile could not know the
     * factory address — WiFi on an ESP32-P4, owned by the co-processor — this
     * is the moment it becomes known, and it is recorded as such. Then, only
     * if the profile's effective address differs (an override, or an override
     * just removed), writes it to the driver and to the netif: the netif took
     * its copy of the address when the station started, and lwIP answers ARP
     * and fills the DHCP request from that copy, not from the driver.
     *
     * esp_wifi_set_mac() is documented for a disabled interface; between
     * mode() and begin() the station is started but not associated. If the
     * driver refuses it there, start() fails and says so in the log.
     *
     * @return true if the station now uses the profile's address.
     */
    bool _syncMac() {
        uint8_t have[NetworkProfile::MAC_LEN];
        esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, have);
        if (err != ESP_OK) {
            log_e("station MAC unreadable: %s", esp_err_to_name(err));
            return false;
        }
        if (!_wifiProfile.hasFactoryMac()) _wifiProfile.setFactoryMac(have);

        uint8_t want[NetworkProfile::MAC_LEN];
        if (!_wifiProfile.getMac(want)) return false;
        if (memcmp(have, want, sizeof(have)) == 0) return true;

        err = esp_wifi_set_mac(WIFI_IF_STA, want);
        if (err == ESP_OK) {
            esp_netif_t* netif = WiFi.STA.netif();
            if (netif) err = esp_netif_set_mac(netif, want);
        }
        if (err != ESP_OK) {
            log_e("station MAC not applied: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

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
        // Beside interfaceType because it is the same kind of fact: what this
        // interface is, not what its link is doing. From the profile here, as
        // every adapter in this library owns its MAC that way — start() made
        // the radio's address match it, so the two cannot disagree.
        getProfile().getMac(s.mac);
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
                // Apply static IP if DHCP is disabled. getConfig() copies the
                // whole struct, SSID and password included, for the three
                // address fields used here — a copy small enough for an event
                // handler to afford.
                WiFiProfile::WiFiConfig cfg;
                inst->_wifiProfile.getConfig(cfg);
                if (!cfg.dhcp) {
                    if (NetworkProfile::DNS_SERVER_COUNT == 2) {
                        WiFi.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0], cfg.dns[1]);
                    } else {
                        WiFi.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0]);
                    }
                } else {
#if NTP_PROFILE_ENABLED
                    esp_sntp_servermode_dhcp(inst->ntpFromDhcp());
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
//
// inline (C++17): these are definitions, so without it every translation unit
// that includes this header emits its own and the link fails with "multiple
// definition of". A single-file Arduino sketch never hits that — everything
// lands in one unit — and even a multi-file project usually includes an adapter
// from just one .cpp, which is why this went unnoticed. It was a limit nobody
// chose and nobody wrote down: where such a constraint is real this library
// says so, as NetworkManagerNtpSyncHook.h does ("exactly one translation
// unit"), so silence here read as "include it wherever you like".
//
// (The multi-file note at the top of this file is about something else: making
// the configuration macros visible to every unit alike. It does not ask anyone
// to include this header twice.)
//
// inline keeps the header-only layout the Arduino IDE's build order requires
// while letting the linker fold the definitions into one object.
// -----------------------------------------------------------------------------

inline std::atomic<ESP32WiFiAdapter*> ESP32WiFiAdapter::_instance        { nullptr };
inline std::atomic<uint8_t>           ESP32WiFiAdapter::_activeCount     { 0 };
inline bool                           ESP32WiFiAdapter::_eventRegistered = false;

#endif // ARDUINO_ARCH_ESP32
