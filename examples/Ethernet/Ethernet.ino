// Ethernet.ino
//
// Universal Ethernet example for NetworkManager — one sketch for ESP32,
// ESP8266 and AVR. The program body is identical on every platform (it uses the
// EthAdapter alias); only the board-specific PHY macros in the configuration
// blocks below differ. It demonstrates DHCP or static IP configuration,
// persistence (save/load/clear), connection events with status reporting, and —
// where enabled — the SNTP client.
//
// Hardware: any supported wired PHY. On AVR and ESP8266 the W5500 is recommended
// (it has hardware link detection); W5100 and ENC28J60 also work.

// -----------------------------------------------------------------------------
// Compile-time ESP32 board-specific ETH configuration
// -----------------------------------------------------------------------------
//
// ETH_PHY_* definitions must appear before including EthAdapter.h.
//
// If your board is supported by the ESP32 Arduino core (e.g. Olimex ESP32-POE,
// ESP32-Gateway), select it in 'Tools -> Board' and the ETH_PHY_* definitions
// are provided automatically — leave the block below commented out.
//
// For custom or unsupported boards, uncomment and adjust one of the examples
// below. Consult your board schematic and the ESP32 Arduino core docs.

/* WIZnet W5xxx PHY (SPI) */
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

/* Microchip LAN8720 PHY (RMII) */
// #define ETH_PHY_TYPE         ETH_PHY_LAN8720
// #define ETH_PHY_ADDR          0
// #define ETH_PHY_MDC          23
// #define ETH_PHY_MDIO         18
// #define ETH_PHY_POWER        -1
// #define ETH_CLK_MODE         ETH_CLOCK_GPIO0_IN

// -----------------------------------------------------------------------------
// Compile-time ESP8266 board-specific ETH configuration
// -----------------------------------------------------------------------------
//
// ESP8266 uses the default SPI pins, so only the PHY type and CS pin are needed.

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
// Compile-time AVR board-specific ETH configuration
// -----------------------------------------------------------------------------
//
// Specify the SPI chip-select pin of your Ethernet shield/module
// (e.g. 10 for most Arduino Ethernet shields).
// #define ETH_PHY_CS 10
//
// On AVR chips without a factory-unique serial number (any chip other than
// ATmega328PB/48PB/88PB/168PB), define a fixed, locally-administered MAC before
// the includes:
// #define NETWORK_PROFILE_DEFAULT_ETH_MAC "4E:52:47:30:30:31"
//
// RAM is scarce on AVR. NTP is opt-in (see below); the FQDN/hostname buffers
// default large and can be trimmed via build_flags or #define here:
// #define HOST_FQDN_LEN            15   // fits a dotted-decimal IP
// #define HOST_FQDN_LABEL_LEN      15
// #define NETWORK_PROFILE_HOSTNAME_LEN 15

// -----------------------------------------------------------------------------
// Compile-time NTP configuration
// -----------------------------------------------------------------------------
//
// Number of configurable NTP servers. Defaults: 3 on ESP32/ESP8266, 0 on AVR
// (NTP opt-in to save RAM). Set to 0 to disable NTP entirely; set to 1 on AVR
// to enable the SNTP client shown below. All NTP-specific code in this sketch is
// wrapped in #if guards, so it compiles cleanly whether NTP is on or off.
//
// Note: DHCP-provided NTP (DHCP option 42) is available on ESP only. On AVR a
// server is always configured explicitly.
// #define NETWORK_PROFILE_NTP_SERVER_COUNT 1

// Include adapter and manager headers AFTER the configuration macros above.
#include <EthAdapter.h>
#include <NetworkManager.h>

// Needed on ESP8266 for setNtpSyncInterval(); a harmless no-op elsewhere.
#include <NetworkManagerNtpSyncHook.h>

// -----------------------------------------------------------------------------
// Configuration — edit these to fit your network
// -----------------------------------------------------------------------------

// Use DHCP (true) or the static configuration below (false).
static const bool      USE_DHCP  = true;

// Static configuration — used only when USE_DHCP is false.
static const IPAddress STATIC_IP     (192, 168,   0, 100);
static const IPAddress STATIC_MASK   (255, 255, 255,   0);
static const IPAddress STATIC_GATEWAY(192, 168,   0,   1);
static const IPAddress STATIC_DNS    (192, 168,   0,   1);

// Interface priority (relevant when combined with other adapters).
static const uint8_t   ETH_PRIO  = 0;

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
// NTP server used when configured explicitly. On AVR prefer a literal IP to
// avoid a blocking DNS lookup on the first sync.
static const char*     NTP       = "pool.ntp.org";
static const uint32_t  NTP_POLL  = 3600000;  // poll interval, ms (default 1 h)
#if !defined(ARDUINO_ARCH_AVR)
// DHCP-provided NTP (DHCP option 42) — ESP only. Set true to take the NTP server
// from DHCP instead of the static NTP above. Not supported on AVR.
static const bool      NTP_DHCP  = false;
#endif
#endif

// -----------------------------------------------------------------------------
// Profile and adapter
// -----------------------------------------------------------------------------

EthProfile ethProfile;
EthAdapter ethAdapter(ethProfile);

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
// Fired by the SNTP client on each successful sync. getEpoch() returns the Unix
// epoch (seconds since 1970), kept advancing between syncs.
void onTimeSync() {
    Serial.println(F("NTP: synced"));
    uint32_t epoch = NetworkManager.getEpoch();
    Serial.print(F("  epoch:  ")); Serial.println(epoch);

    // Convert the Unix epoch to a UTC date/time without time.h / ctime(), which
    // are unreliable on AVR. Civil-from-days algorithm — pure integer math, so
    // it behaves identically on every platform.
    uint32_t days = epoch / 86400UL, rem = epoch % 86400UL;
    uint8_t  hh = rem / 3600, mm = (rem % 3600) / 60, ss = rem % 60;
    int32_t  z   = (int32_t)days + 719468;
    int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t  y   = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp  = (5 * doy + 2) / 153;
    uint32_t d   = doy - (153 * mp + 2) / 5 + 1;
    uint32_t mo  = mp < 10 ? mp + 3 : mp - 9;
    y += (mo <= 2);
    // Sized for the compiler's worst-case format-truncation estimate (it assumes
    // the long/unsigned-long args span their full width); the real output is 23.
    char buf[48];
    snprintf(buf, sizeof(buf), "%04ld-%02lu-%02lu %02u:%02u:%02u UTC",
             (long)y, (unsigned long)mo, (unsigned long)d, hh, mm, ss);
    Serial.print(F("  UTC:    ")); Serial.println(buf);

    // The SDK exposes SNTP servers by slot, not which one delivered the sync, so
    // list the active servers — this also reveals whether DHCP-provided or
    // statically configured servers are in use.
    for (uint8_t i = 0; i < NetworkProfile::NTP_SERVER_COUNT; i++) {
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
            Serial.println(F("Network: connected"));
            {
                // Hostname and MAC come from the profile via the adapter, so they
                // are available even for a fresh (DHCP) connection. ACTIVE returns
                // the effective value (override if set, else the generated default).
                char host[NetworkProfile::MAX_HOSTNAME_SIZE];
                if (adapter.getHostname(host, sizeof(host))) {
                    Serial.print(F("  Host:    ")); Serial.println(host);
                }
                NetworkProfile::MACAddress mac;
                if (adapter.getMac(mac)) {
                    char macStr[18];
                    snprintf(macStr, sizeof(macStr),
                             "%02X:%02X:%02X:%02X:%02X:%02X",
                             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                    Serial.print(F("  MAC:     ")); Serial.println(macStr);
                }
            }
            Serial.print(F("  DHCP:    ")); Serial.println((adapter.getProfile().isDhcp()) ? F("on") : F("off"));
            Serial.print(F("  IP:      ")); Serial.println(s.localIP);
            Serial.print(F("  Netmask: ")); Serial.println(s.subnetMask);
            Serial.print(F("  Gateway: ")); Serial.println(s.gateway);
            for (uint8_t i = 0; i < NetworkProfile::DNS_SERVER_COUNT; i++) {
                if (s.dns[i] != IPAddress(0, 0, 0, 0)) {
                    Serial.print(F("  DNS:     ")); Serial.println(s.dns[i]);
                }
            }
            break;
        case NetworkManagerClass::Event::DISCONNECTED:
            Serial.println(F("Network: disconnected"));
            break;
        default:
            break;
    }
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    Serial.println(F("\nNetworkManager Ethernet example starts."));

    // Persistence demo: on the first run nothing is stored, so the defaults
    // above are applied and saved. On the next boot the saved profile is loaded
    // and then cleared, so the cycle repeats — a simple way to exercise
    // saveCfg() / loadCfg() / clearCfg(). Remove the clearCfg() call to keep the
    // saved profile permanently.
    if (ethProfile.loadCfg("eth")) {
        Serial.println(F("Saved configuration found and loaded."));
        Serial.println(F("Clearing configuration (it will be saved again next boot)."));
        ethProfile.clearCfg("eth");
    } else {
        Serial.println(F("No saved configuration - applying defaults."));

        NetworkProfile::NetworkConfig cfg;
        cfg.dhcp     = USE_DHCP;
        cfg.priority = ETH_PRIO;
        if (!USE_DHCP) {
            cfg.ip      = STATIC_IP;
            cfg.mask    = STATIC_MASK;
            cfg.gateway = STATIC_GATEWAY;
            cfg.dns[0]  = STATIC_DNS;
        }
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    #if defined(ARDUINO_ARCH_AVR)
        // AVR has no DHCP-provided NTP — always configure a server.
        strncpy(cfg.ntp[0], NTP, Host::MAX_FQDN_LEN);
    #else
        if (!USE_DHCP || !NTP_DHCP) strncpy(cfg.ntp[0], NTP, Host::MAX_FQDN_LEN);
    #endif
#endif
        ethProfile.setConfig(cfg);

        Serial.println(F("Saving configuration."));
        ethProfile.saveCfg("eth");
    }

    NetworkManager.onEvent(onNetworkEvent);
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    NetworkManager.onNtpSync(onTimeSync);
#endif

    NetworkManager.addAdapter(ethAdapter);
    NetworkManager.begin();
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    NetworkManager.setNtpSyncInterval(NTP_POLL);
#endif
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    NetworkManager.update();
    delay(10);
}
