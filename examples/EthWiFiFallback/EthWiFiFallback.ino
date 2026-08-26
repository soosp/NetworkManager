// EthWiFiFallback.ino
//
// Demonstrates ETH → WiFi priority-based fallback using NetworkManager.
// Runs on ESP32 and ESP8266 boards.
//
// ETH is the primary interface (priority 0). If ETH fails or is unavailable,
// NetworkManager automatically starts WiFi (priority 1). When ETH recovers,
// it takes over and WiFi is stopped. If you reverse the priorities, the roles
// of the interfaces are also reversed.
// This sketch also demonstrates basic configuration and operation of NTP client.

// -----------------------------------------------------------------------------
// Compile time ESP32 board-specific ETH configuration
// -----------------------------------------------------------------------------

// ETH_PHY_* definitions must appear before including the corresponding Adapter
// header (ESP32EthAdapter.h or ESP8266EthAdapter.h via EthAdapter.h)
//
// If your board is supported by the ESP32 Arduino core (e.g. Olimex ESP32-POE,
// ESP32-Gateway), set your board in 'Tools → Board' and the ETH_PHY_*
// definitions are provided automatically — comment out the block below.
//
// For custom or unsupported boards, uncomment and adjust one of the examples
// below. Consult your board's schematic and ESP32 Arduino core documentation
// for the correct values.

/* WIZnet W5xxx PHY example (SPI) */
// #define ETH_PHY_TYPE         ETH_PHY_W5500
// #define ETH_PHY_SPI_HOST     SPI3_HOST
// #define ETH_PHY_SPI_FREQ_MHZ 25
// #define ETH_PHY_ADDR          1
// #define ETH_PHY_CS           13
// #define ETH_PHY_IRQ          12
// #define ETH_PHY_RST          14
// #define ETH_PHY_SPI_SCK      15
// #define ETH_PHY_SPI_MISO     11
// #define ETH_PHY_SPI_MOSI     16

/* Microchip LAN8720 PHY example (RMII) */
// #define ETH_PHY_TYPE         ETH_PHY_LAN8720
// #define ETH_PHY_ADDR          0
// #define ETH_PHY_MDC          23
// #define ETH_PHY_MDIO         18
// #define ETH_PHY_POWER        -1
// #define ETH_CLK_MODE         ETH_CLOCK_GPIO0_IN

/* IC Plus IP101 PHY example (RMII) */
// #define ETH_PHY_TYPE         ETH_PHY_IP101
// #define ETH_PHY_ADDR          0
// #define ETH_PHY_MDC          31
// #define ETH_PHY_MDIO         52
// #define ETH_PHY_POWER        51
// #define ETH_CLK_MODE         ETH_CLOCK_GPIO0_IN

// -----------------------------------------------------------------------------
// Compile time ESP8266 board-specific ETH configuration
// -----------------------------------------------------------------------------

// ESP8266 use the default SPI pins, so you have to define PHY type and
// CS pin only.

/* WIZnet W5500 PHY */
// #define ETH_PHY_TYPE         ETH_PHY_W5500
// #define ETH_PHY_CS           16 

/* WIZnet W5100 PHY */
// #define ETH_PHY_TYPE         ETH_PHY_W5100
// #define ETH_PHY_CS           16 

/* Microchip ENC28J60 PHY */
// #define ETH_PHY_TYPE         ETH_PHY_ENC28J60
// #define ETH_PHY_CS           16 

// -----------------------------------------------------------------------------
// Compile time WiFi configuration
// -----------------------------------------------------------------------------

// Some buggy ESP32 C3 and S3 boards with poor antenna design need to decrease
// the WiFi transmit power at most 13-15 dBm to avoid RF interference and
// made WiFi to work. The default is 19.5 dBm (maximum power) on ESP32.
// Uncomment and adjust the line below to modify this default.
// WIFI_PROFILE_DEFAULT_WIFI_TX_POWER macro must be defined before including
// the corresponding WiFi Adapter (ESP32WiFiAdapter.h or ESP8266WiFiAdapter.h
// via WiFiAdapter.h). Of course, you can also configure the TX power on the
// ESP8266 in the same way.
// #define WIFI_PROFILE_DEFAULT_WIFI_TX_POWER 13.0f

// -----------------------------------------------------------------------------
// Compile time NTP configuration
// -----------------------------------------------------------------------------

// Set the maximum number of configurable NTP servers. If you set this to 0,
// the NTP function is completely disabled and the associated code is not
// compiled, thereby reducing resource requirements. In this case, you have to
// remove or comment out all NTP-related code sections from the sketch, as
// these were not placed within #if guards to improve the sketch’s readability.
// #define NTP_PROFILE_ENABLED 0

// Include headers of adapters and NetworkManager after configuration macros
// are defined.
#include <EthAdapter.h>
#include <WiFiAdapter.h>
#include <NetworkManager.h>
#if NTP_PROFILE_ENABLED
#  include <NtpProfile.h>
#endif

// If you want to set NTP sync interval on ESP8266, you must include this
// header to provide the necessary hook for ESP8266 SDK. It is safe to include
// it on other platforms too (no-op).
// It defines NETWORK_MANAGER_NTP_SYNC_INTERVAL_SETTER to check the presence
// of hook. On other platforms it is defined by default.
#include <NetworkManagerNtpSyncHook.h>

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

// Ethernet use DHCP (true) or the static configuration below (false).
static const bool      ETH_USE_DHCP  = true;

// Static configuration for Ethernet — used only when ETH_USE_DHCP is false.
static const IPAddress ETH_STATIC_IP      (192, 168,   0, 100);
static const IPAddress ETH_STATIC_MASK    (255, 255, 255,   0);
static const IPAddress ETH_STATIC_GATEWAY (192, 168,   0,   1);
static const IPAddress ETH_STATIC_DNS     (192, 168,   0,   1);

// Configure your WiFi credentials.
static const char*     WIFI_SSID      = "MyNetwork";
static const char*     WIFI_PASSWORD  = "MyPassword";

// WiFi use DHCP (true) or the static configuration below (false).
static const bool      WIFI_USE_DHCP  = true;

// Static configuration for WiFi — used only when WIFI_USE_DHCP is false.
static const IPAddress WIFI_STATIC_IP     (192, 168,   0, 101);
static const IPAddress WIFI_STATIC_MASK   (255, 255, 255,   0);
static const IPAddress WIFI_STATIC_GATEWAY(192, 168,   0,   1);
static const IPAddress WIFI_STATIC_DNS    (192, 168,   0,   1);

// Set interface priorities
static const uint8_t   ETH_PRIO  = 0;
static const uint8_t   WIFI_PRIO = 1;

// NTP configuration. Set ETH_NTP_DHCP and/or WIFI_NTP_DHCP true to let that
// interface take its time server from DHCP option 42; leave it false to use the
// statically configured servers below. Each flag needs its interface to be on
// DHCP to have any effect — a statically addressed interface never asks.
//
// Setting the two differently is worth trying only deliberately: see the note in
// onNetworkEvent() about what a handoff can and cannot re-decide.
static const bool      ETH_NTP_DHCP  = false;
static const bool      WIFI_NTP_DHCP = false;
static const char*     NTP0          = "pool.ntp.org";
static const char*     NTP1          = "time.google.com";
static const char*     NTP2          = "time.cloudflare.com";

#if NTP_PROFILE_ENABLED
// One profile per interface, so ETH_NTP_DHCP and WIFI_NTP_DHCP above can be set
// independently. Each is bound to its adapter in setup(). Leaving one empty is
// how this sketch says "let DHCP supply the server for this interface" — with
// nothing configured, the manager asks for option 42 instead.
//
// If both interfaces should use the same servers, drop one of these and call
// NetworkManager.setNtpProfile() once in setup() instead.
NtpProfile             ethNtp;
NtpProfile             wifiNtp;
static constexpr char  NS_NTP_ETH[]  = "ntp_eth";
static constexpr char  NS_NTP_WIFI[] = "ntp_wifi";
#endif
static const uint32_t  NTP_POLL      = 3600000; // NTP poll interval, default 1 hour

// -----------------------------------------------------------------------------
// Profiles and adapters
// -----------------------------------------------------------------------------

EthProfile  ethProfile;
WiFiProfile wifiProfile;
EthAdapter  ethAdapter(ethProfile);
WiFiAdapter wifiAdapter(wifiProfile);

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------
#if NTP_PROFILE_ENABLED

void onTimeSync() {
    Serial.println(F("NTP: synced"));
    time_t now = time(nullptr);
    Serial.print(F("  time:   ")); Serial.print(ctime(&now));  // ctime() ends in '\n'

    // The SDK exposes SNTP servers by slot, not which one delivered the sync, so
    // list the active servers — this also reveals whether DHCP-provided or
    // statically configured servers are in use.
    for (uint8_t i = 0; i < NtpProfile::SERVER_COUNT; i++) {
        IPAddress ip = NetworkManager.getActiveNtpIP(i);
        if (ip == IPAddress(0, 0, 0, 0)) continue;   // unset / not yet resolved
        char name[Host::MAX_FQDN_SIZE];
        Serial.print(F("  server: "));
        if (NetworkManager.getActiveNtpName(i, name, sizeof(name))) {
            Serial.print(name); Serial.print(F(" ("));
            Serial.print(ip);   Serial.println(F(")"));
        } else {
            Serial.println(ip);                      // numeric (e.g. DHCP) server
        }
    }
}
#endif

void onNetworkEvent(NetworkManagerClass::Event event, NetworkAdapter& adapter) {
    NetworkStatus s = NetworkManager.getStatus();

    switch (event) {
        case NetworkManagerClass::Event::CONNECTED:
            Serial.println("Network: connected");
            break;
        case NetworkManagerClass::Event::FALLBACK:
            Serial.println("Network: primary interface failed — fallback active");
            break;
        case NetworkManagerClass::Event::RESTORED:
            Serial.println("Network: primary interface recovered — fallback stopped");
            break;
        case NetworkManagerClass::Event::DISCONNECTED:
            Serial.println("Network: disconnected — all interfaces failed");
            break;
        default:
            break;
    }
    if (event != NetworkManagerClass::Event::DISCONNECTED) {
        char host[NetworkProfile::MAX_HOSTNAME_SIZE];
        if (adapter.getHostname(host, sizeof(host))) {
            Serial.print("  Host: "); Serial.println(host);
        }
        Serial.print("  Interface: ");
        Serial.println(s.interfaceType
                        == NetworkProfile::InterfaceType::ETH ? "ETH" : "WiFi");
        NetworkProfile::MACAddress mac;
        if (adapter.getMac(mac)) {
            char macStr[18];
            snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            Serial.print("  MAC: "); Serial.println(macStr);
        }

        // WiFi-specific status, shown only when the serving interface is WiFi.
        // The callback always receives the base NetworkAdapter&, so check the
        // interface type before down-casting to reach the WiFi accessors.
        if (adapter.getProfile().getInterfaceType()
                == NetworkProfile::InterfaceType::WIFI) {
            WiFiAdapter& w = static_cast<WiFiAdapter&>(adapter);
            char ssid[WiFiProfile::MAX_SSID_SIZE];
            if (wifiProfile.getSsid(ssid, sizeof(ssid))) {
                Serial.print("  SSID: "); Serial.println(ssid);
            }
            Serial.print("  RSSI: ");    Serial.print(w.getRssi());       Serial.println(" dBm");
            Serial.print("  TxPower: "); Serial.print(w.getTxPower(), 1); Serial.println(" dBm (set)");
        }

        Serial.print("  DHCP: ");
        Serial.println(adapter.getProfile().isDhcp() ? "on" : "off");
        Serial.print("  IP: ");      Serial.println(s.localIP);
        Serial.print("  Netmask: "); Serial.println(s.subnetMask);
        Serial.print("  Gateway: "); Serial.println(s.gateway);
        for (uint8_t i = 0; i < NetworkProfile::DNS_SERVER_COUNT; i++) {
            if (s.dns[i] != IPAddress(0,0,0,0)) {
                Serial.printf("  DNS%u: ", i);
                Serial.println(s.dns[i]);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
// Configuration guard
// -----------------------------------------------------------------------------

// setConfig() validates the whole struct and stores nothing if any field fails,
// so an unchecked call leaves the profile at its defaults while the sketch runs
// on as if it had been configured — an empty SSID looks like a WiFi fault rather
// than a typo at the top of this file. checkConfig() names the field that
// failed; halting is right for a sketch, where there is no one to re-enter it.
static void applyOrHalt(bool ok, const __FlashStringHelper* what,
                        NetworkProfile::ConfigCheck why) {
    if (ok) return;
    Serial.print(what);
    Serial.print(F(" config rejected: "));
    Serial.println(NetworkProfile::configCheckName(why));
    Serial.println(F("Check the settings at the top of this sketch."));
    while (true) delay(1000);
}

// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    delay(500);
    Serial.println("\nNetworkManager Eth+WiFi fallback example starts.");

    // Persistence demo (both profiles): on the first run nothing is stored, so
    // the defaults below are applied and saved. On the next boot the saved
    // profiles are loaded and then cleared, so the cycle repeats — a simple way
    // to exercise saveCfg() / loadCfg() / clearCfg(). Remove the clearCfg()
    // calls to keep the saved profiles permanently.

    // Ethernet profile
    if (ethProfile.loadCfg("eth")) {
        Serial.println("ETH:  saved configuration loaded - clearing it.");
        ethProfile.clearCfg("eth");
        #if NTP_PROFILE_ENABLED
        // Cleared together with the network profile so the two namespaces stay
        // in step. Left behind, the stored NTP settings would outlive every
        // cycle and the constants at the top of this sketch would stop deciding
        // anything.
        ethNtp.clearCfg(NS_NTP_ETH);
        #endif
    } else {
        Serial.println("ETH:  no saved configuration - applying defaults.");
        NetworkProfile::NetworkConfig ethCfg;
        ethCfg.dhcp     = ETH_USE_DHCP;
        ethCfg.priority = ETH_PRIO;
        // Use static configuration if DHCP is disabled
        if (!ETH_USE_DHCP) {
            ethCfg.ip      = ETH_STATIC_IP;
            ethCfg.mask    = ETH_STATIC_MASK;
            ethCfg.gateway = ETH_STATIC_GATEWAY;
            ethCfg.dns[0]  = ETH_STATIC_DNS;
        }
        applyOrHalt(ethProfile.setConfig(ethCfg),
                    F("ETH"), NetworkProfile::checkConfig(ethCfg));
        ethProfile.saveCfg("eth");
    }

    // WiFi profile
    if (wifiProfile.loadCfg("wifi")) {
        Serial.println("WiFi: saved configuration loaded - clearing it.");
        wifiProfile.clearCfg("wifi");
        #if NTP_PROFILE_ENABLED
        // Cleared together with the network profile so the two namespaces stay
        // in step. Left behind, the stored NTP settings would outlive every
        // cycle and the constants at the top of this sketch would stop deciding
        // anything.
        wifiNtp.clearCfg(NS_NTP_WIFI);
        #endif
    } else {
        Serial.println("WiFi: no saved configuration - applying defaults.");
        WiFiProfile::WiFiConfig wifiCfg;
        wifiCfg.dhcp     = WIFI_USE_DHCP;
        wifiCfg.priority = WIFI_PRIO;
        // Use static configuration if DHCP is disabled
        if (!WIFI_USE_DHCP) {
            wifiCfg.ip      = WIFI_STATIC_IP;
            wifiCfg.mask    = WIFI_STATIC_MASK;
            wifiCfg.gateway = WIFI_STATIC_GATEWAY;
            wifiCfg.dns[0]  = WIFI_STATIC_DNS;
        }
        strncpy(wifiCfg.ssid,     WIFI_SSID,     sizeof(wifiCfg.ssid)     - 1);
        strncpy(wifiCfg.password, WIFI_PASSWORD, sizeof(wifiCfg.password) - 1);
        applyOrHalt(wifiProfile.setConfig(wifiCfg),
                    F("WiFi"), WiFiProfile::checkConfig(wifiCfg));
        wifiProfile.saveCfg("wifi");
    }

    // Register callbacks
    NetworkManager.onEvent(onNetworkEvent);
#if NTP_PROFILE_ENABLED
    NetworkManager.onNtpSync(onTimeSync);
#endif

    // Add adapters and start
#if NTP_PROFILE_ENABLED
    // Ethernet: three servers, or none at all when the lease should provide them.
    if (!ethNtp.loadCfg(NS_NTP_ETH)) {
        // setServer() refuses anything that is not a valid FQDN or IP literal,
        // so a typo would otherwise leave the slot empty and the clock quietly
        // unsynchronised.
        if (!ETH_USE_DHCP || !ETH_NTP_DHCP) {
            bool ok = ethNtp.setServer(NTP0, 0);
            if (NtpProfile::SERVER_COUNT >= 2) ok &= ethNtp.setServer(NTP1, 1);
            if (NtpProfile::SERVER_COUNT >= 3) ok &= ethNtp.setServer(NTP2, 2);
            if (!ok) Serial.println(F("ETH: an NTP server was rejected."));
        }
        ethNtp.saveCfg(NS_NTP_ETH);
    }
    // WiFi: one server, or none.
    if (!wifiNtp.loadCfg(NS_NTP_WIFI)) {
        if (!WIFI_USE_DHCP || !WIFI_NTP_DHCP) {
            if (!wifiNtp.setServer(NTP2, 0))
                Serial.println(F("WiFi: the NTP server was rejected."));
        }
        wifiNtp.saveCfg(NS_NTP_WIFI);
    }
    // Bind each profile to its interface. The manager then knows, before it
    // starts either one, whether that interface should ask DHCP for a time
    // server — which is the only moment lwIP can still be told. Nothing has to
    // be re-pointed on a handoff.
    //
    // A device whose interfaces share one time source would instead call
    // NetworkManager.setNtpProfile() once and skip these two lines.
    ethAdapter.setNtpProfile(ethNtp);
    wifiAdapter.setNtpProfile(wifiNtp);
#endif

    NetworkManager.addAdapter(ethAdapter);
    NetworkManager.addAdapter(wifiAdapter);
    NetworkManager.begin();
    NetworkManager.setNtpSyncInterval(NTP_POLL);
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    NetworkManager.update();
    delay(10);
}
