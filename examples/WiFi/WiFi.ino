// WiFi.ino
//
// Universal WiFi example for NetworkManager — ESP32 and ESP8266. WiFi is not
// available on AVR, so this sketch targets the ESP platforms only (the
// WiFiAdapter alias #errors on AVR). It demonstrates DHCP or static IP over
// WiFi, connection events with WiFi-specific status (SSID, RSSI, TX power),
// persistence (save/load/clear), and the SNTP client. The body uses the
// WiFiAdapter alias, so it is identical on ESP32 and ESP8266.

// -----------------------------------------------------------------------------
// Compile-time WiFi configuration
// -----------------------------------------------------------------------------
//
// Some ESP32-C3/S3 modules have a weak PCB antenna and associate more reliably
// at a reduced transmit power. To lower the default TX power, define this before
// the includes (in dBm); leave it commented to use the SDK default. TX power can
// also be changed at runtime with WiFiAdapter::setTxPower().
// #define WIFI_PROFILE_DEFAULT_WIFI_TX_POWER 8.5

// -----------------------------------------------------------------------------
// Compile-time NTP configuration
// -----------------------------------------------------------------------------
//
// Number of configurable NTP servers (default 3 on ESP). Set to 0 to disable NTP
// entirely; all NTP-specific code below is wrapped in #if guards.
// #define NETWORK_PROFILE_NTP_SERVER_COUNT 3

// Include adapter and manager headers AFTER the configuration macros above.
#include <WiFiAdapter.h>
#include <NetworkManager.h>

// Needed on ESP8266 for setNtpSyncInterval(); a harmless no-op elsewhere.
#include <NetworkManagerNtpSyncHook.h>

// -----------------------------------------------------------------------------
// Configuration — edit these to fit your network
// -----------------------------------------------------------------------------

static const char*     WIFI_SSID     = "MyNetwork";
static const char*     WIFI_PASSWORD = "MyPassword";

// Use DHCP (true) or the static configuration below (false).
static const bool      USE_DHCP  = true;

// Static configuration — used only when USE_DHCP is false.
static const IPAddress STATIC_IP     (192, 168,   0, 100);
static const IPAddress STATIC_MASK   (255, 255, 255,   0);
static const IPAddress STATIC_GATEWAY(192, 168,   0,   1);
static const IPAddress STATIC_DNS    (192, 168,   0,   1);

// Interface priority (relevant when combined with other adapters).
static const uint8_t   WIFI_PRIO = 0;

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
// Set NTP_DHCP true to take the NTP server from DHCP (option 42) instead of the
// static server below.
static const bool      NTP_DHCP  = false;
static const char*     NTP       = "pool.ntp.org";
static const uint32_t  NTP_POLL  = 3600000;  // poll interval, ms (default 1 h)
#endif

// -----------------------------------------------------------------------------
// Profile and adapter
// -----------------------------------------------------------------------------

WiFiProfile wifiProfile;
WiFiAdapter wifiAdapter(wifiProfile);

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
void onTimeSync() {
    Serial.println(F("NTP: synced"));
    time_t now = time(nullptr);
    Serial.print(F("  time:   ")); Serial.print(ctime(&now));  // ctime() ends in '\n'

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
                // Hostname and MAC come from the profile via the adapter.
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
            {
                // WiFi-specific status: SSID comes from the profile; RSSI and TX
                // power come from the WiFi adapter (WiFi.RSSI() and the configured
                // dBm). These accessors are specific to the WiFi adapter type, so
                // they are read from the typed globals rather than the base
                // NetworkAdapter& handed to this callback.
                char ssid[WiFiProfile::MAX_SSID_SIZE];
                if (wifiProfile.getSsid(ssid, sizeof(ssid))) {
                    Serial.print(F("  SSID:    ")); Serial.println(ssid);
                }
                Serial.print(F("  RSSI:    "));
                Serial.print(wifiAdapter.getRssi());    Serial.println(F(" dBm"));
                Serial.print(F("  TxPower: "));
                Serial.print(wifiAdapter.getTxPower(), 1); Serial.println(F(" dBm"));
            }
            Serial.print(F("  DHCP:    "));
            Serial.println(adapter.getProfile().isDhcp() ? F("on") : F("off"));
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
    Serial.println(F("\nNetworkManager WiFi example starts."));

    // Persistence demo: on the first run nothing is stored, so the defaults
    // above are applied and saved. On the next boot the saved profile is loaded
    // and then cleared, so the cycle repeats — a simple way to exercise
    // saveCfg() / loadCfg() / clearCfg(). Remove the clearCfg() call to keep the
    // saved profile permanently.
    if (wifiProfile.loadCfg("wifi")) {
        Serial.println(F("Saved configuration found and loaded."));
        Serial.println(F("Clearing configuration (it will be saved again next boot)."));
        wifiProfile.clearCfg("wifi");
    } else {
        Serial.println(F("No saved configuration - applying defaults."));

        WiFiProfile::WiFiConfig cfg;
        cfg.dhcp     = USE_DHCP;
        cfg.priority = WIFI_PRIO;
        strncpy(cfg.ssid,     WIFI_SSID,     sizeof(cfg.ssid)     - 1);
        strncpy(cfg.password, WIFI_PASSWORD, sizeof(cfg.password) - 1);
        if (!USE_DHCP) {
            cfg.ip      = STATIC_IP;
            cfg.mask    = STATIC_MASK;
            cfg.gateway = STATIC_GATEWAY;
            cfg.dns[0]  = STATIC_DNS;
        }
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
        if (!USE_DHCP || !NTP_DHCP) strncpy(cfg.ntp[0], NTP, Host::MAX_FQDN_LEN);
#endif
        wifiProfile.setConfig(cfg);

        Serial.println(F("Saving configuration."));
        wifiProfile.saveCfg("wifi");
    }

    NetworkManager.onEvent(onNetworkEvent);
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    NetworkManager.onNtpSync(onTimeSync);
#endif

    NetworkManager.addAdapter(wifiAdapter);
    NetworkManager.begin();
#if (NETWORK_PROFILE_NTP_SERVER_COUNT > 0)
    NetworkManager.setNtpSyncInterval(NTP_POLL);
#endif

    // WiFi TX power can also be set at runtime (dBm), e.g. for a weak antenna:
    // wifiAdapter.setTxPower(8.5);
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    NetworkManager.update();
    delay(10);
}
