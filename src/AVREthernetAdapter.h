/*
 * Header-only: the Arduino IDE compiles libraries separately from the sketch, so
 * a precompiled unit would not see the configuration macros you define — being
 * header-only, the library is compiled with each includer's macros instead.
 * In a multi-file project, define those macros globally so every translation
 * unit agrees (see the README, "Where to define these").
 */

#pragma once

#if defined(ARDUINO_ARCH_AVR)
#include "NetworkAdapter.h"
#include "EthProfile.h"
#include <Ethernet.h>
#include <Dhcp.h>      // DHCP_CHECK_* constants (Ethernet.maintain())

#ifndef ETH_PHY_CS
#error "CS pin (ETH_PHY_CS) must be defined before including AVREthernetAdapter.h"
#endif

#ifndef AVR_ETH_ADAPTER_DHCP_TIMEOUT
#define AVR_ETH_ADAPTER_DHCP_TIMEOUT 15000
#endif

#ifndef AVR_ETH_ADAPTER_LINK_TIMEOUT
#define AVR_ETH_ADAPTER_LINK_TIMEOUT 5000
#endif

#ifndef AVR_ETH_ADAPTER_POLL_INTERVAL
#define AVR_ETH_ADAPTER_POLL_INTERVAL 500
#endif

#ifndef AVR_ETH_ADAPTER_DHCP_BEGIN_TIMEOUT
#define AVR_ETH_ADAPTER_DHCP_BEGIN_TIMEOUT 8000   // ms; blocking DHCP wait in begin()
#endif
#ifndef AVR_ETH_ADAPTER_DHCP_BEGIN_RESPONSE
#define AVR_ETH_ADAPTER_DHCP_BEGIN_RESPONSE 4000  // ms; per-request response wait
#endif

/**
 * @brief NetworkAdapter implementation for AVR Ethernet (Arduino Ethernet library).
 *
 * Polling-based — no event system available on AVR. The update() method
 * checks link status and DHCP state periodically.
 *
 * Timeouts:
 *   AVR_ETH_ADAPTER_LINK_TIMEOUT  — max time to wait for link up after start()
 *                                    (default: 5000 ms)
 *   AVR_ETH_ADAPTER_DHCP_TIMEOUT  — max time to wait for DHCP IP after link up
 *                                    (default: 15000 ms)
 *   AVR_ETH_ADAPTER_POLL_INTERVAL — how often update() polls the link status
 *                                    (default: 500 ms)
 *
 * @note Requires a MAC address to be set in the EthProfile before start().
 * @note DHCP renewal is handled by Ethernet.maintain() called in update().
 */
class AVREthernetAdapter : public NetworkAdapter {
public:
    /** @brief Maximum time to wait for link up after start() in milliseconds. */
    static constexpr uint32_t LINK_TIMEOUT  = AVR_ETH_ADAPTER_LINK_TIMEOUT;

    /** @brief DHCP acquisition timeout in milliseconds. */
    static constexpr uint32_t DHCP_TIMEOUT  = AVR_ETH_ADAPTER_DHCP_TIMEOUT;

    /** @brief Link status poll interval in milliseconds. */
    static constexpr uint32_t POLL_INTERVAL = AVR_ETH_ADAPTER_POLL_INTERVAL;

    /**
     * @brief Constructs an AVREthernetAdapter with an associated EthProfile.
     * @param profile Reference to the EthProfile describing this interface.
     */
    AVREthernetAdapter(EthProfile& profile)
        : NetworkAdapter(profile)
        , _ethProfile(profile)
        , _csPin(ETH_PHY_CS)
        , _connectingStart(0)
        , _lastPoll(0)
    {}

    // -------------------------------------------------------------------------
    // NetworkAdapter interface
    // -------------------------------------------------------------------------

    /**
     * @brief Starts the Ethernet connection.
     *
     * Reads MAC address from the profile and calls Ethernet.begin().
     * If DHCP is disabled, applies static IP immediately.
     * Transitions to CONNECTING.
     *
     * @return true if Ethernet.begin() was called, false if MAC is invalid.
     */
    bool start() override {
        // Set CS pin before Ethernet.begin()
        Ethernet.init(_csPin);

        // Read MAC from profile
        uint8_t mac[NetworkProfile::MAC_LEN];
        _ethProfile.getMac(mac);
        if (!NetworkProfile::isValidMac(mac)) return false;

        NetworkProfile::NetworkConfig cfg;
        _ethProfile.getConfig(cfg);

        if (cfg.dhcp) {
            // The Arduino Ethernet library's begin(mac) is SYNCHRONOUS and blocks
            // for its internal DHCP timeout (~60 s by default) when no lease can be
            // obtained — e.g. with no link at boot. The timeout overload caps that
            // wait so a cold boot without a link is not stalled for over a minute.
            Ethernet.begin(mac,
                           AVR_ETH_ADAPTER_DHCP_BEGIN_TIMEOUT,
                           AVR_ETH_ADAPTER_DHCP_BEGIN_RESPONSE);
        } else {
            Ethernet.begin(mac, cfg.ip, cfg.dns[0], cfg.gateway, cfg.mask);
        }

        // setHostname() exists only on Ethernet forks that advertise it;
        // the stock Arduino Ethernet library (e.g. 2.0.2) has no such method.
#if defined(ETHERNET_HAS_SETHOSTNAME)
        char hostname[NetworkProfile::MAX_HOSTNAME_SIZE];
        _ethProfile.getHostname(hostname, sizeof(hostname));
        Ethernet.setHostname(hostname);
#endif

        _connectingStart = millis();
        _lastPoll        = millis();
        _setState(State::CONNECTING);
        return true;
    }

    /**
     * @brief Stops the Ethernet connection and transitions to IDLE.
     */
    void stop() override {
        // end() exists only on Ethernet forks that advertise it; the stock
        // Arduino Ethernet library (e.g. 2.0.2) has no such method.
#if defined(ETHERNET_HAS_END)
        Ethernet.end();
#endif
        _clearStatus();
        _connectingStart = 0;
        _lastPoll        = 0;
        _setState(State::IDLE);
    }

    /**
     * @brief Polls link status and drives the state machine.
     *
     * In CONNECTING state:
     *   - Checks for link up and IP assignment
     *   - Transitions to CONNECTED when IP is obtained
     *   - Transitions to FAILED on timeout
     *
     * In CONNECTED state:
     *   - Calls Ethernet.maintain() for DHCP renewal
     *   - Transitions to FAILED if link is lost
     *
     * Must be called regularly from the main loop via NetworkManagerClass::update().
     */
    void update() override {
        uint32_t now = millis();

        // Throttle polling
        if (now - _lastPoll < POLL_INTERVAL) return;
        _lastPoll = now;

        switch (getState()) {
            case State::CONNECTING: {
                if (_hasIp()) {
                    _connectingStart = 0;
                    _cacheStatus(_buildStatus());
                    _setState(State::CONNECTED);
                } else if (Ethernet.linkStatus() == LinkOFF &&
                           now - _connectingStart > LINK_TIMEOUT) {
                    // Link never came up. W5500 has real link detection, so this
                    // fails fast (~LINK_TIMEOUT) instead of waiting out the DHCP
                    // window. On W5100 linkStatus() is never LinkOFF, so this
                    // branch never fires there and DHCP_TIMEOUT governs instead.
                    _setState(State::FAILED);
                } else if (now - _connectingStart > DHCP_TIMEOUT) {
                    // Link is up but no IP arrived within the DHCP window.
                    _setState(State::FAILED);
                }
                break;
            }
            case State::CONNECTED: {
                // DHCP renewal
                const uint8_t m = Ethernet.maintain();
                if (m == DHCP_CHECK_RENEW_FAIL || m == DHCP_CHECK_REBIND_FAIL) {
                    _clearStatus();
                    _setState(State::FAILED);
                    break;
                }
                // Check link
                if (Ethernet.linkStatus() == LinkOFF) {
                    _clearStatus();
                    _setState(State::FAILED);
                    break;
                }
                // A successful renewal may have changed the address — refresh.
                if (Ethernet.localIP() != _status.localIP) {
                    _cacheStatus(_buildStatus());
                }
                break;
            }
            default:
                break;
        }
    }

    /**
     * @brief Returns the actual IP address of the adapter.
     *
     * @note On W5100 boards, Ethernet.localIP() can return a stale,
     *       previously-assigned address immediately after link loss,
     *       since the chip has no hardware link detection (see _hasIp()).
     *       This mirrors the same hardware limitation noted there.
     */
    IPAddress getLocalIP() const override { return Ethernet.localIP(); }

private:
    EthProfile& _ethProfile;      ///< Typed reference to the Ethernet profile.
    uint8_t     _csPin;           ///< SPI chip-select pin for the Ethernet controller.
    uint32_t    _connectingStart; ///< millis() timestamp when CONNECTING began.
    uint32_t    _lastPoll;        ///< millis() timestamp of last poll.

    /**
     * @brief Builds a live status snapshot from the Ethernet interface.
     *
     * The Arduino Ethernet library exposes a single DNS server, so only dns[0]
     * is populated; any further slots stay INADDR_ANY.
     */
    NetworkStatus _buildStatus() const {
        NetworkStatus s;
        s.interfaceType = getProfile().getInterfaceType();
        s.connected     = true;
        s.localIP       = Ethernet.localIP();
        s.subnetMask    = Ethernet.subnetMask();
        s.gateway       = Ethernet.gatewayIP();
        s.dns[0]        = Ethernet.dnsServerIP();
        return s;
    }

    /**
     * @brief Returns true if the Ethernet link is up and an IP is assigned.
     */
    bool _hasIp() const {
        if (Ethernet.localIP() == IPAddress(0, 0, 0, 0)) return false;
        // W5100 does not support hardware link detection — linkStatus()
        // always returns Unknown. On W5100, CONNECTED state may be entered
        // even without a physical cable. This is a hardware limitation.
        if (Ethernet.linkStatus() == LinkOFF) return false;
        return true;
    }
};

#endif // ARDUINO_ARCH_AVR