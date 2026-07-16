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
#include "EthProfile.h"
#include <SPI.h>
#include <ESP8266WiFi.h>   // lwIP integration + WL_CONNECTED status enum
#include <lwip/netif.h>    // netif_set_link_up/down (re-arm DHCP re-discovery)

// -----------------------------------------------------------------------------
// Ethernet PHY/chip selection (compile-time)
//
// The ESP8266 lwIP wired drivers are distinct C++ *types* (Wiznet5500lwIP,
// Wiznet5100lwIP, ENC28J60lwIP), so the chip must be chosen at compile time via
// ETH_PHY_TYPE — it cannot be a runtime value. Define ETH_PHY_TYPE before
// including this header, e.g.:
//
//   #define ETH_PHY_TYPE ETH_PHY_W5500
//   #include "ESP8266EthAdapter.h"
//
// The selector symbols are given explicit values below (guarded with #ifndef)
// so that an undefined ETH_PHY_TYPE does not silently expand to 0 and compare
// equal to an undefined selector.
// -----------------------------------------------------------------------------

#ifndef ETH_PHY_W5500
#define ETH_PHY_W5500     1
#endif
#ifndef ETH_PHY_W5100
#define ETH_PHY_W5100     2
#endif
#ifndef ETH_PHY_ENC_28J60
#define ETH_PHY_ENC_28J60 3
#endif

#ifndef ETH_PHY_TYPE
#error "ETH_PHY_TYPE must be defined before including ESP8266EthAdapter.h " \
       "(ETH_PHY_W5500, ETH_PHY_W5100, or ETH_PHY_ENC_28J60)"
#elif !defined(ETH_PHY_CS)
#error "CS pin (ETH_PHY_CS) must be defined before including ESP8266EthAdapter.h"
#endif

#if   ETH_PHY_TYPE == ETH_PHY_W5500
#include <W5500lwIP.h>
typedef Wiznet5500lwIP Esp8266EthChip;
#elif ETH_PHY_TYPE == ETH_PHY_W5100
#include <W5100lwIP.h>
typedef Wiznet5100lwIP Esp8266EthChip;
#elif ETH_PHY_TYPE == ETH_PHY_ENC_28J60
#include <ENC28J60lwIP.h>
typedef ENC28J60lwIP Esp8266EthChip;
#else
#error "Unknown ETH_PHY_TYPE. Valid values: ETH_PHY_W5500, ETH_PHY_W5100, ETH_PHY_ENC_28J60"
#endif

#ifndef ESP8266_ETH_ADAPTER_DHCP_TIMEOUT
#define ESP8266_ETH_ADAPTER_DHCP_TIMEOUT 15000
#endif

#ifndef ESP8266_ETH_ADAPTER_SPI_FREQUENCY
#define ESP8266_ETH_ADAPTER_SPI_FREQUENCY 4000000
#endif

/**
 * @brief NetworkAdapter implementation for ESP8266 wired Ethernet.
 *
 * Uses the ESP8266 core's lwIP-integrated wired drivers (W5500lwIP /
 * W5100lwIP / ENC28J60lwIP), so the interface is a real lwIP netif: it shares
 * the SDK TCP/IP stack with WiFi, and NTP via the SDK SNTP stack works exactly
 * as it does for ESP8266 WiFi (no separate client is needed).
 *
 * Unlike the WiFi adapter, the wired netif has no SDK event callbacks, so this
 * adapter is **poll-driven**: update() polls the link/IP status and drives the
 * state machine. This mirrors the AVR adapter's detection model on top of the
 * ESP8266/WiFi adapter's structure.
 *
 * Lifecycle: the lwIP driver is started once (first start()) and kept running;
 * stop() does not call end(). This avoids the fragile begin/end churn (re-adding
 * the same netif struct and re-initialising the chip on every probe) and keeps
 * the PHY readable so canProbe() can gate on the physical link. canProbe() only
 * returns true when the cable is in (isLinked()), so the interface is never
 * re-armed while unplugged.
 *
 * For a DHCP configuration, default route, DNS and SNTP are handled by
 * LwipIntfDev itself: its status callback runs check_route() (sets this netif
 * as default when connected and preferred, clears it when disconnected) and
 * (re)starts SNTP on connect, so the adapter only expresses a preference via
 * setDefault(true/false). A static configuration is the exception: LwipIntfDev
 * installs the default route only on the DHCP bind path, and stop() takes the
 * netif link down, so for a static address the adapter installs the gateway and
 * default route by hand (netif_set_gw + netif_set_default) and re-ups the netif
 * link on re-arm — otherwise off-link traffic (e.g. NTP to a public server) has
 * no route after a static bind or a reconnect. An ETH + WiFi fallback still
 * hands the default route over automatically as each interface's connected
 * state changes.
 *
 * DHCP timeout: if no IP is obtained within ESP8266_ETH_ADAPTER_DHCP_TIMEOUT
 * milliseconds after start(), the adapter transitions to FAILED.
 */
class ESP8266EthAdapter : public NetworkAdapter {
public:
    /** @brief DHCP acquisition timeout in milliseconds. */
    static constexpr uint32_t DHCP_TIMEOUT   = ESP8266_ETH_ADAPTER_DHCP_TIMEOUT;

    /** @brief SPI clock frequency for the Ethernet controller, in Hz. */
    static constexpr uint32_t SPI_FREQUENCY  = ESP8266_ETH_ADAPTER_SPI_FREQUENCY;

    /**
     * @brief Constructs an ESP8266EthAdapter.
     *
     * @param profile Reference to the EthProfile describing this interface.
     */
    ESP8266EthAdapter(EthProfile& profile)
        : NetworkAdapter(profile)
        , _ethProfile(profile)
        , _eth(ETH_PHY_CS)
        , _connectingStart(0)
        , _chipStarted(false)
    {}

    // -------------------------------------------------------------------------
    // NetworkAdapter interface
    // -------------------------------------------------------------------------

    /**
     * @brief Starts (or re-arms) the Ethernet interface.
     *
     * The lwIP driver is brought up only once, on the first start: SPI and the
     * controller are initialised, the netif is added, and DHCP (or the static
     * config) is started via begin(). The driver is then kept running across
     * stop/start cycles. Repeatedly calling end()+begin() on the same netif is
     * fragile (it re-adds the same netif struct and re-initialises the chip on
     * every probe), so it is avoided — mirroring the ESP32 adapter.
     * On a subsequent start (a re-probe after the link returned) the still-running
     * netif recovers: for DHCP the netif link is toggled to trigger a fresh
     * discovery; for a static address the netif link is brought back up (stop()
     * took it down) and the default route is re-installed. We then wait for
     * connected().
     *
     * setDefault(true) marks Ethernet as the preferred default route. For DHCP the
     * routing switch and SNTP (re)start are performed by LwipIntfDev's status
     * callback once connected; for a static address the adapter installs the
     * gateway and default route by hand here (see the class description).
     *
     * @return true on success; false if the MAC is invalid or, on the first
     *         start, no controller was detected.
     */
    bool start() override {
        NetworkProfile::NetworkConfig cfg;
        _ethProfile.getConfig(cfg);
        if (!_chipStarted) {
            SPI.begin();
            SPI.setBitOrder(MSBFIRST);
            SPI.setDataMode(SPI_MODE0);
            SPI.setFrequency(SPI_FREQUENCY);

            uint8_t mac[NetworkProfile::MAC_LEN];
            _ethProfile.getMac(mac);
            if (!NetworkProfile::isValidMac(mac)) return false;

            // Static configuration must be applied with config() *before*
            // begin(); for DHCP we skip it and let the DHCP client run.
            if (!cfg.dhcp) {
#if (NETWORK_PROFILE_DNS_SERVER_COUNT >= 2)
                _eth.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0], cfg.dns[1]);
#else
                _eth.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0]);
#endif
            }

            if (!_eth.begin(mac)) return false;   // no controller detected
            _chipStarted = true;
        } else {
            // Re-arm after the link returned. LwipIntfDev brings the wired netif
            // up statically (begin() calls netif_set_link_up unconditionally) and
            // never reflects the physical cable afterwards, so replugging produces
            // no link-up event and the DHCP client would not re-discover on its
            // own — the CONNECTING window would then expire with no IP (observed
            // as ETH never restoring after a cable-out boot). Force a fresh DHCP
            // discovery by toggling the netif link, which drives
            // dhcp_network_changed() internally.
            // Static: stop() took the netif link down; bring it back up.
            // The address is retained; the default route is re-established
            // after setDefault() below.
            netif* n = const_cast<netif*>(_eth.getNetIf());
            if (n) {
                if (cfg.dhcp) {
                    netif_set_link_down(n);
                    netif_set_link_up(n);   // -> dhcp_network_changed -> fresh DISCOVER
                } else {
                    netif_set_link_up(n);
                }
            }
        }

        // Prefer Ethernet as the default route while it is the serving interface.
        _eth.setDefault(true);

        // On a static config LwipIntfDev's check_route() does not install this
        // netif as the default route (only the DHCP bind path does), so off-link
        // traffic (e.g. NTP to a public server) has no route. Set it explicitly
        // here, after setDefault(), so it is not overwritten.
        if (!cfg.dhcp) {
            netif* n = const_cast<netif*>(_eth.getNetIf());
            if (n) {
                ip4_addr_t gw; gw.addr = (uint32_t)cfg.gateway;
                netif_set_gw(n, &gw);
                netif_set_default(n);
            }
        }
        _connectingStart = millis();
        _setState(State::CONNECTING);
        return true;
    }

    /**
     * @brief Logically stops the Ethernet interface (transitions to IDLE).
     *
     * Deliberately does NOT call end(). The lwIP driver and netif are kept alive
     * so that (a) canProbe() can read the physical link — isLinked() needs the
     * chip powered — and (b) the DHCP client recovers automatically when the
     * link returns. We only drop the default-route preference; LwipIntfDev's
     * check_route() already removes a disconnected interface from the default
     * route on its own. _lastFailedMs is stamped so canProbe() enforces the
     * retry interval after a clean stop and the adapter does not immediately
     * re-probe a still-linked interface (e.g. right after a RESTORED).
     */
    void stop() override {
        _setState(State::IDLE);
        _clearStatus();
        _eth.setDefault(false);
        // Reflect the (cable-out) link-down to lwIP so the still-present wired
        // netif is excluded from routing. LwipIntfDev leaves the netif up with
        // its last IP; without this, that stale IP keeps subnet-matching the same
        // network as the WiFi fallback (e.g. both on /16), so lwIP would route
        // local traffic — such as DHCP-provided NTP — out the dead interface and
        // it never reaches the server (DHCP-NTP dead in fallback, static works).
        // The re-arm in start() calls netif_set_link_up() again (which also kicks
        // a fresh DHCP discovery), so this pairs with that toggle.
        netif* n = const_cast<netif*>(_eth.getNetIf());
        if (n) netif_set_link_down(n);
        _connectingStart = 0;
        _lastFailedMs.store(millis());
    }

    /**
     * @brief Polls link/IP status and drives the state machine.
     *
     * CONNECTING -> CONNECTED once the netif has an IP (connected()), or
     * CONNECTING -> FAILED on DHCP timeout. CONNECTED -> FAILED if the
     * connection is later lost. The wired netif has no SDK events, so this
     * polling is the equivalent of the WiFi adapter's GOT_IP / disconnect
     * callbacks.
     */
    void update() override {
        const State s = getState();
        if (s == State::CONNECTING) {
            if (_eth.connected()) {
                _connectingStart = 0;
                // Capture the live snapshot before announcing CONNECTED.
                _cacheStatus(_buildStatus());
                _setState(State::CONNECTED);
            } else if (millis() - _connectingStart > DHCP_TIMEOUT) {
                _setState(State::FAILED);
            }
        } else if (s == State::CONNECTED) {
            // connected() goes false when the netif loses its address (link
            // down / lease lost). Cable-unplug detection latency depends on the
            // lwIP/controller behaviour — verify on hardware; if it is too slow,
            // poll the link status as well.
            if (!_eth.connected()) {
                _clearStatus();
                _setState(State::FAILED);
            } else if (_eth.localIP() != _status.localIP) {
                // DHCP renewed with a different address — refresh the snapshot.
                // Reading _status here is safe: this poll runs on the loop task
                // and ESP8266 is single-threaded (the adapter lock is a no-op).
                _cacheStatus(_buildStatus());
            }
        }
    }

    /** @brief Returns the current IP address of the Ethernet interface. */
    IPAddress getLocalIP() const override { return _eth.localIP(); }

    /** @brief Makes this interface the default route. */
    void setDefaultRoute() override {
        _eth.setDefault(true);
        // Static path: setDefault(true) is not enough — install the route by
        // hand, mirroring what start() does for a static address.
        netif* n = const_cast<netif*>(_eth.getNetIf());
        if (n) netif_set_default(n);
    }

    /**
     * @brief Whether the adapter is a valid probe candidate right now.
     *
     * On the first probe the driver is not yet running, so the probe is allowed
     * unconditionally to let start() initialise it. Afterwards the probe is
     * gated on the physical link: there is nothing to connect to without a
     * cable, and gating here is what stops the manager from re-arming the
     * interface (and the begin/DHCP machinery) while it is unplugged. The retry
     * interval still applies so a clean stop does not re-probe instantly.
     */
    bool canProbe() const override {
        if (getState() != State::IDLE) return false;
        if (!_chipStarted) return true;               // first start brings the driver up
        const uint32_t last = _lastFailedMs.load();
        if (last != 0 && millis() - last < RETRY_INTERVAL) return false;
        return _eth.isLinked();                       // only probe with the cable in
    }

private:
    EthProfile&            _ethProfile;     ///< Typed reference to the Ethernet profile.
    mutable Esp8266EthChip _eth;            ///< lwIP wired interface (mutable: isLinked() in const canProbe()).
    uint32_t               _connectingStart;///< millis() timestamp when CONNECTING began.
    bool                   _chipStarted;    ///< true once begin() has initialised the driver (begin-once).

    /** @brief Builds a live status snapshot from the wired lwIP interface. */
    NetworkStatus _buildStatus() const {
        NetworkStatus s;
        s.interfaceType = getProfile().getInterfaceType();
        s.connected     = true;
        s.localIP       = _eth.localIP();
        s.subnetMask    = _eth.subnetMask();
        s.gateway       = _eth.gatewayIP();
        // LwipIntfDev exposes no DNS getter. On ESP8266 the DNS servers live in
        // lwIP's single global table (dns_getserver), which this interface's
        // DHCP client populated; WiFi.dnsIP(i) reads exactly that table, so it
        // returns the wired interface's DNS here regardless of WiFi state.
        for (uint8_t i = 0; i < NetworkProfile::DNS_SERVER_COUNT; i++) {
            s.dns[i] = WiFi.dnsIP(i);
        }
        return s;
    }
};

#endif // ARDUINO_ARCH_ESP8266