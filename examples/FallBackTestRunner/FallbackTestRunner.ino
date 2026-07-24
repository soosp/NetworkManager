// FallbackTestRunner.ino
//
// Semi-automatic fallback/restore test rig for NetworkManager (ESP32/ESP8266).
//
// It walks a 32-case matrix — 2 priority arrangements x 2 addressing modes x 8
// scenarios — running ONE case per boot and restarting itself, so no reflashing
// is needed between cases. The only manual part is the physical action: the
// runner prompts (asks to disconnect the cable or disable the WiFi) and waits
// for ENTER to ensure the asked action was done and starts the timing.
//
// For every transition it checks TWO things:
//
//   * event  — the expected high-level event (CONNECTED / FALLBACK / RESTORED /
//              DISCONNECTED) on the expected interface;
//   * NTP    — that a time sync follows within a timeout. This doubles as an
//              end-to-end liveness probe: an NTP sync against an FQDN can only
//              succeed if the default route AND the DNS resolver survived the
//              handoff. Both of those broke in ways that were invisible at the
//              event level, which is exactly why this column exists.
//
// It also records timings: ENTER->event (approximate: includes your reaction
// time, so treat it as an upper bound) and event->NTP sync (exact).
//
// Important: Pressing ENTER does not trigger the wait for the event; that
// process runs in the background. Pressing ENTER simply indicates that the
// event trigger (e.g., unplugging the cable) has occurred and the timer can
// start.
//
// Results are persisted across reboots and printed as a summary table when the
// matrix completes.
//
// In the event of an accidental error (e.g., enabling Ethernet instead of
// WiFi), the current test case can be restarted by rebooting the MCU (e.g.,
// using the reset button).
//
// SUGGESTED BUILD OPTIONS FOR THE RIG (not for production):
//   -DNETWORK_MANAGER_RECONNECT_TIMEOUT=20000   // shorter DISCONNECT escalation
// Without it the "disconnected" steps each wait the full 60 s default. In some
// cases, the Wi-Fi connection may take longer than 20 seconds, which can cause
// a FAIL in WiFi connection tests. In this case, try increasing the value to,
// for example, 30000.
// You may define it here before including NetworkManager.h.
// #define NETWORK_MANAGER_RECONNECT_TIMEOUT 20000

#if defined(ARDUINO_ARCH_ESP32)
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
// #define ETH_PHY_IRQ          -1
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

#elif defined(ARDUINO_ARCH_ESP8266)
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

#else
#   error "This test runner targets ESP32 and ESP8266 (WiFi is required)."
#endif

#include <EthAdapter.h>
#include <WiFiAdapter.h>
#include <NetworkManager.h>
#include <NetworkManagerNtpSyncHook.h>   // ESP8266 needs this for setNtpSyncInterval()

// Preferences is native on ESP32 and provided by the external Preferences
// library on ESP8266 — the same API NetworkProfile persists through, so the
// runner's own state lives alongside it.
#include <Preferences.h>

#if defined(ARDUINO_ARCH_ESP32)
#   include <esp_netif.h>
#   include <lwip/dns.h>
#endif

// -----------------------------------------------------------------------------
// Site configuration — edit to match your network
// -----------------------------------------------------------------------------

static const char*     WIFI_SSID      = "MyNetwork";
static const char*     WIFI_PASSWORD  = "MyPassword";

static const IPAddress ETH_STATIC_IP      (192, 168,   0, 100);
static const IPAddress ETH_STATIC_MASK    (255, 255, 255,   0);
static const IPAddress ETH_STATIC_GATEWAY (192, 168,   0,   1);
static const IPAddress ETH_STATIC_DNS     (192, 168,   0,   1);

static const IPAddress WIFI_STATIC_IP     (192, 168,   0, 101);
static const IPAddress WIFI_STATIC_MASK   (255, 255, 255,   0);
static const IPAddress WIFI_STATIC_GATEWAY(192, 168,   0,   1);
static const IPAddress WIFI_STATIC_DNS    (192, 168,   0,   1);

// Different servers per interface make it obvious in the log which profile the
// SNTP client is actually using after a handoff.
static const char*     ETH_NTP        = "pool.ntp.org";
static const char*     WIFI_NTP       = "time.cloudflare.com";

// Poll interval for the rig. Short enough that a lost first request still
// retries inside the NTP timeout below — so a "slow" sync shows up as a large
// time rather than a failure, which is the interesting signal.
static const uint32_t  NTP_POLL       = 30000;

// -----------------------------------------------------------------------------
// Timeouts
// -----------------------------------------------------------------------------

static const uint32_t  EVENT_TIMEOUT  = 90000;   // wait for an expected event
static const uint32_t  NTP_TIMEOUT    = 60000;   // wait for a sync after a handoff
static const uint32_t  QUIET_WINDOW   = 20000;   // "no event expected" observation

// Forward declarations (waitEnter() can abort a case, which restarts the device).
static void nextCase(bool evOk, bool ntpOk, uint32_t tSwitchMax, uint32_t tNtpMax);
static void storeReset();

// -----------------------------------------------------------------------------
// Test matrix
// -----------------------------------------------------------------------------
//
// 32 cases = 2 priority arrangements x 2 addressing modes x 8 scenarios.
//   caseIdx = prio * 16 + addr * 8 + scenario
//     prio: 0 = ETH primary  (ETH prio 0, WiFi prio 1)
//           1 = WiFi primary (WiFi prio 0, ETH prio 1)
//     addr: 0 = DHCP, 1 = static
//
// Scenarios are written in terms of the PRIMARY (P) and SECONDARY (S) interface,
// so the same eight patterns cover both priority arrangements:
//
//   1  P -> S -> P -> S -> P
//   2  P -> S -> disc -> S -> P
//   3  P -> S -> disc -> P -> S -> P
//   4  S -> P -> S -> P
//   5  S -> disc -> S -> P -> S -> P
//   6  S -> disc -> P -> S -> P
//   7  disc -> S -> P -> S -> P
//   8  disc -> P -> S -> P

static const uint8_t   CASE_COUNT     = 32;

enum class Act  : uint8_t { P_UP, P_DOWN, S_UP, S_DOWN };
enum class Ev   : uint8_t { NONE, CONNECTED, DISCONNECTED, FALLBACK, RESTORED };
enum class Serv : uint8_t { NONE, P, S };

struct Step {
    Act  act;        // physical action the operator performs
    Ev   expect;     // event expected afterwards (NONE = expect silence)
    Serv serving;    // which interface should be serving after the event
};

struct Scenario {
    const char* name;
    bool        pUpAtBoot;    // required physical state BEFORE reset
    bool        sUpAtBoot;
    Ev          bootExpect;   // event expected from the boot itself
    Serv        bootServing;
    Step        steps[6];
    uint8_t     stepCount;
};

static const Scenario SCENARIOS[8] = {
    { "1  P > S > P > S > P", true, true, Ev::CONNECTED, Serv::P, {
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S },
        { Act::P_UP,   Ev::RESTORED,     Serv::P },
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S },
        { Act::P_UP,   Ev::RESTORED,     Serv::P },
    }, 4 },

    { "2  P > S > disc > S > P", true, true, Ev::CONNECTED, Serv::P, {
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S    },
        { Act::S_DOWN, Ev::DISCONNECTED, Serv::NONE },
        { Act::S_UP,   Ev::CONNECTED,    Serv::S    },
        { Act::P_UP,   Ev::RESTORED,     Serv::P    },
    }, 4 },

    { "3  P > S > disc > P > S > P", true, true, Ev::CONNECTED, Serv::P, {
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S    },
        { Act::S_DOWN, Ev::DISCONNECTED, Serv::NONE },
        { Act::P_UP,   Ev::CONNECTED,    Serv::P    },
        { Act::S_UP,   Ev::NONE,         Serv::P    },  // S returns; P keeps serving
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S    },
        { Act::P_UP,   Ev::RESTORED,     Serv::P    },
    }, 6 },

    { "4  S > P > S > P", false, true, Ev::CONNECTED, Serv::S, {
        { Act::P_UP,   Ev::RESTORED,     Serv::P },
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S },
        { Act::P_UP,   Ev::RESTORED,     Serv::P },
    }, 3 },

    { "5  S > disc > S > P > S > P", false, true, Ev::CONNECTED, Serv::S, {
        { Act::S_DOWN, Ev::DISCONNECTED, Serv::NONE },
        { Act::S_UP,   Ev::CONNECTED,    Serv::S    },
        { Act::P_UP,   Ev::RESTORED,     Serv::P    },
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S    },
        { Act::P_UP,   Ev::RESTORED,     Serv::P    },
    }, 5 },

    { "6  S > disc > P > S > P", false, true, Ev::CONNECTED, Serv::S, {
        { Act::S_DOWN, Ev::DISCONNECTED, Serv::NONE },
        { Act::P_UP,   Ev::CONNECTED,    Serv::P    },
        { Act::S_UP,   Ev::NONE,         Serv::P    },  // S returns; P keeps serving
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S    },
        { Act::P_UP,   Ev::RESTORED,     Serv::P    },
    }, 5 },

    { "7  disc > S > P > S > P", false, false, Ev::DISCONNECTED, Serv::NONE, {
        { Act::S_UP,   Ev::CONNECTED,    Serv::S },
        { Act::P_UP,   Ev::RESTORED,     Serv::P },
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S },
        { Act::P_UP,   Ev::RESTORED,     Serv::P },
    }, 4 },

    { "8  disc > P > S > P", false, false, Ev::DISCONNECTED, Serv::NONE, {
        { Act::P_UP,   Ev::CONNECTED,    Serv::P },
        { Act::S_UP,   Ev::NONE,         Serv::P },     // S returns; P keeps serving
        { Act::P_DOWN, Ev::FALLBACK,     Serv::S },
        { Act::P_UP,   Ev::RESTORED,     Serv::P },
    }, 4 },
};

// -----------------------------------------------------------------------------
// Persisted results
// -----------------------------------------------------------------------------

struct Result {
    uint8_t  done;        // 0 = not run yet
    uint8_t  evOk;        // all events matched
    uint8_t  ntpOk;       // every serving transition produced a sync
    uint8_t  pad;
    uint16_t tSwitchMax;  // worst ENTER->event, ms/100 (approximate)
    uint16_t tNtpMax;     // worst event->sync,  ms/100
};

struct Store {
    uint32_t magic;
    uint8_t  caseIdx;
    uint8_t  single;      // 1 = re-running one case; return to summary afterwards
    uint8_t  pad[2];
    Result   results[CASE_COUNT];
};

static const uint32_t STORE_MAGIC = 0x4E4D5401;  // "NMT\x01"
static Store          g_store;

static void storeLoad() {
    Preferences p;
    p.begin("nmtest", true);
    size_t n = p.getBytes("s", &g_store, sizeof(g_store));
    p.end();
    if (n != sizeof(g_store) || g_store.magic != STORE_MAGIC) {
        memset(&g_store, 0, sizeof(g_store));
        g_store.magic = STORE_MAGIC;
    }
}

static void storeSave() {
    Preferences p;
    p.begin("nmtest", false);
    p.putBytes("s", &g_store, sizeof(g_store));
    p.end();
}

static void storeReset() {
    memset(&g_store, 0, sizeof(g_store));
    g_store.magic = STORE_MAGIC;
    storeSave();
}

// -----------------------------------------------------------------------------
// Profiles and adapters
// -----------------------------------------------------------------------------

EthProfile  ethProfile;
WiFiProfile wifiProfile;
EthAdapter  ethAdapter(ethProfile);
WiFiAdapter wifiAdapter(wifiProfile);

// -----------------------------------------------------------------------------
// Event and NTP capture
// -----------------------------------------------------------------------------

static volatile bool     g_evPending = false;
static volatile uint32_t g_evAtMs    = 0;
static Ev                g_evLast    = Ev::NONE;
static NetworkProfile::InterfaceType g_evIface = NetworkProfile::InterfaceType::UNKNOWN;

static volatile bool     g_ntpPending = false;

void onNetworkEvent(NetworkManagerClass::Event event, NetworkAdapter& adapter) {
    switch (event) {
        case NetworkManagerClass::Event::CONNECTED:    g_evLast = Ev::CONNECTED;    break;
        case NetworkManagerClass::Event::DISCONNECTED: g_evLast = Ev::DISCONNECTED; break;
        case NetworkManagerClass::Event::FALLBACK:     g_evLast = Ev::FALLBACK;     break;
        case NetworkManagerClass::Event::RESTORED:     g_evLast = Ev::RESTORED;     break;
        default:                                       return;
    }
    g_evIface   = adapter.getProfile().getInterfaceType();
    g_evAtMs    = millis();
    g_evPending = true;
}

void onTimeSync() {
    g_ntpPending = true;
}

// -----------------------------------------------------------------------------
// Case decoding
// -----------------------------------------------------------------------------

static uint8_t caseIdx()    { return g_store.caseIdx; }
static bool    wifiPrimary(){ return caseIdx() >= 16; }
static bool    useStatic()  { return ((caseIdx() / 8) % 2) == 1; }
static const Scenario& scenario() { return SCENARIOS[caseIdx() % 8]; }

// Map the abstract P/S roles onto the real interfaces for this case.
static bool servIsEth(Serv s) {
    if (s == Serv::P) return !wifiPrimary();
    return wifiPrimary();               // Serv::S
}
static bool actIsEth(Act a) {
    bool primary = (a == Act::P_UP || a == Act::P_DOWN);
    return primary ? !wifiPrimary() : wifiPrimary();
}
static bool actIsUp(Act a) { return (a == Act::P_UP || a == Act::S_UP); }

// -----------------------------------------------------------------------------
// Console helpers
// -----------------------------------------------------------------------------

static const char* evName(Ev e) {
    switch (e) {
        case Ev::CONNECTED:    return "CONNECTED";
        case Ev::DISCONNECTED: return "DISCONNECTED";
        case Ev::FALLBACK:     return "FALLBACK";
        case Ev::RESTORED:     return "RESTORED";
        default:               return "(none)";
    }
}

// Waits for a line on the serial console. NetworkManager.update() keeps running,
// so the stack stays alive while the operator works.
static void waitEnter(const char* prompt) {
    Serial.print(F("\n>>> ")); Serial.print(prompt);
    Serial.println(F("  [any key = done | s = skip case | r = reset suite]"));
    while (Serial.available()) Serial.read();
    for (;;) {
        NetworkManager.update();          // keep the stack alive while you work —
                                          // the event may well arrive before you
                                          // get back to the keyboard, which is why
                                          // waitEvent() must not discard it.
        if (!Serial.available()) { delay(5); continue; }
        int c = Serial.read();
        if (c == 's' || c == 'S') { Serial.println(F("--- case skipped")); nextCase(false, false, 0, 0); }
        if (c == 'r' || c == 'R') { Serial.println(F("--- suite reset"));  storeReset(); ESP.restart(); }
        // Any other key confirms. Do not require a newline: with the serial
        // monitor set to "No line ending", pressing Enter sends nothing at all.
        while (Serial.available()) Serial.read();
        return;
    }
}

static void prompt(Act a) {
    bool eth = actIsEth(a);
    bool up  = actIsUp(a);
    char buf[90];
    if (eth) {
        snprintf(buf, sizeof(buf), "%s the ETHERNET link (%s the cable / switch port)",
                 up ? "RESTORE" : "TAKE DOWN",
                 up ? "reconnect" : "disconnect");
    } else {
        snprintf(buf, sizeof(buf), "%s the WiFi AP (%s the SSID)",
                 up ? "RESTORE" : "TAKE DOWN",
                 up ? "enable" : "disable");
    }
    waitEnter(buf);
}

// -----------------------------------------------------------------------------
// Waiting primitives — all of them pump NetworkManager.update()
// -----------------------------------------------------------------------------

// tRef is the moment the operator confirmed the action. The event often fires
// BEFORE that (update() runs during the prompt), so the pending flag must not be
// cleared here — it is cleared at the start of the step instead. An event that
// beat the keypress is reported as 0 ms.
static bool waitEvent(Ev expect, Serv serving, uint32_t tRef, uint32_t& elapsed) {
    uint32_t t0 = millis();
    while (millis() - t0 < EVENT_TIMEOUT) {
        NetworkManager.update();
        if (!g_evPending) { delay(5); continue; }
        g_evPending = false;
        uint32_t at = g_evAtMs;
        elapsed = (int32_t)(at - tRef) > 0 ? (at - tRef) : 0;

        bool ok = (g_evLast == expect);
        if (ok && serving != Serv::NONE) {
            bool wantEth = servIsEth(serving);
            bool gotEth  = (g_evIface == NetworkProfile::InterfaceType::ETH);
            ok = (wantEth == gotEth);
        }
        Serial.print(F("    event: ")); Serial.print(evName(g_evLast));
        if (g_evLast == Ev::DISCONNECTED) {
            // No interface is serving, so the adapter handed to the callback is
            // not meaningful — do not print it as if it were.
            Serial.print(F(" (no serving interface)"));
        } else {
            Serial.print(F(" on "));
            Serial.print(g_evIface == NetworkProfile::InterfaceType::ETH ? F("ETH") : F("WiFi"));
        }
        if (elapsed == 0) {
            // The manager reacted before you got back to the keyboard, so the
            // ENTER-relative latency is meaningless here. The event->NTP time
            // below is machine-measured and is the number that matters.
            Serial.print(F("  (arrived during the prompt)  "));
        } else {
            Serial.print(F("  after ")); Serial.print(elapsed); Serial.print(F(" ms  "));
        }
        if (ok) { Serial.println(F("[OK]")); return true; }
        Serial.print(F("[FAIL] expected ")); Serial.println(evName(expect));
        // A wrong interface usually means the primary failed to associate (a
        // transient AP/auth failure), in which case the manager was right to take
        // the other one. Print the adapter states so the log says which it was.
        Serial.print(F("    diag:  ETH state=")); Serial.print((int)ethAdapter.getState());
        Serial.print(F("  WiFi state="));         Serial.print((int)wifiAdapter.getState());
        Serial.println(F("   (0=IDLE 1=CONNECTING 2=CONNECTED 3=FAILED)"));
        return false;
    }
    elapsed = EVENT_TIMEOUT;
    Serial.print(F("    event: TIMEOUT waiting for ")); Serial.println(evName(expect));
    return false;
}

// Verifies that NO event arrives during the quiet window (used when a returning
// secondary must not disturb a serving primary).
static bool waitQuiet() {
    uint32_t t0 = millis();
    // Do NOT clear the flag here: it was armed before the prompt, so a spurious
    // event that arrived while the operator was working must still be caught.
    while (millis() - t0 < QUIET_WINDOW) {
        NetworkManager.update();
        if (g_evPending) {
            g_evPending = false;
            Serial.print(F("    event: ")); Serial.print(evName(g_evLast));
            Serial.println(F("  [FAIL] no event expected here"));
            return false;
        }
        delay(5);
    }
    Serial.println(F("    quiet as expected  [OK]"));
    return true;
}

// Called when a sync fails to arrive: separates the three things that can break
// a handoff — the default route, the DNS resolver, and SNTP itself. A TCP probe
// to a literal IP tests the route without touching DNS; hostByName can answer
// from the lwIP cache, so it is not used here.
static void diagnose() {
    Serial.print(F("    diag:  "));
#if defined(ARDUINO_ARCH_ESP32)
    esp_netif_t* def = esp_netif_get_default_netif();
    Serial.print(F("default="));
    Serial.print(def ? esp_netif_get_desc(def) : "(none)");
    const ip_addr_t* d0 = dns_getserver(0);
    Serial.print(F("  lwip-dns0="));
    Serial.print(d0 ? ipaddr_ntoa(d0) : "(null)");
#elif defined(ARDUINO_ARCH_ESP8266)
    // No esp_netif here; lwIP's resolver is read back through WiFi.dnsIP(), and
    // the serving interface shows up via the station/eth dns getters.
    Serial.print(F("sta-dns0=")); Serial.print(WiFi.dnsIP(0));
    Serial.print(F(" dns1="));     Serial.print(WiFi.dnsIP(1));
#endif
    WiFiClient c;
    Serial.print(F("  tcp 1.1.1.1:53="));
#if defined(ARDUINO_ARCH_ESP32)
    bool ok = c.connect(IPAddress(1, 1, 1, 1), 53, 2000);
#else
    c.setTimeout(2000);   // ESP8266 connect() has no timeout argument
    bool ok = c.connect(IPAddress(1, 1, 1, 1), 53);
#endif
    Serial.println(ok ? F("OK") : F("FAIL"));
    c.stop();
}

// An NTP sync after a handoff proves the route AND the resolver survived it.
static bool waitNtp(uint32_t& elapsed) {
    uint32_t t0 = millis();
    while (millis() - t0 < NTP_TIMEOUT) {
        NetworkManager.update();
        if (g_ntpPending) {
            g_ntpPending = false;
            elapsed = millis() - t0;
            char name[Host::MAX_FQDN_SIZE];
            NetworkManager.getActiveNtpName(0, name, sizeof(name));
            Serial.print(F("    ntp:   sync from "));
            Serial.print(name[0] ? name : "(numeric)");
            Serial.print(F("  after ")); Serial.print(elapsed);
            Serial.println(F(" ms  [OK]"));
            return true;
        }
        delay(5);
    }
    elapsed = NTP_TIMEOUT;
    Serial.println(F("    ntp:   NO SYNC within timeout  [FAIL]"));
    diagnose();
    return false;
}

// -----------------------------------------------------------------------------
// Case sequencing
// -----------------------------------------------------------------------------

static void nextCase(bool evOk, bool ntpOk, uint32_t tSwitchMax, uint32_t tNtpMax) {
    Result& r   = g_store.results[caseIdx()];
    r.done      = 1;
    r.evOk      = evOk  ? 1 : 0;
    r.ntpOk     = ntpOk ? 1 : 0;
    r.tSwitchMax = (uint16_t)(tSwitchMax / 100);
    r.tNtpMax    = (uint16_t)(tNtpMax    / 100);
    if (g_store.single) {
        g_store.single  = 0;
        g_store.caseIdx = CASE_COUNT;   // back to the summary, not the next case
    } else {
        g_store.caseIdx++;
    }
    storeSave();
    Serial.println(F("\n--- restarting ---\n"));
    delay(300);
    ESP.restart();
}

static void printSummary() {
    Serial.println(F("\n================================= SUMMARY ================================="));
    Serial.println(F("case  prio  addr    scenario                     events  ntp   sw(s) ntp(s)"));
    uint8_t passed = 0;
    for (uint8_t i = 0; i < CASE_COUNT; i++) {
        const Result& r = g_store.results[i];
        char line[110];
        snprintf(line, sizeof(line),
                 " %3u  %-4s  %-6s  %-27s  %-6s  %-4s  %5.1f  %5.1f",
                 i,
                 (i >= 16) ? "WiFi" : "ETH",
                 (((i / 8) % 2) == 1) ? "static" : "dhcp",
                 SCENARIOS[i % 8].name,
                 !r.done ? "-"    : (r.evOk  ? "PASS" : "FAIL"),
                 !r.done ? "-"    : (r.ntpOk ? "PASS" : "FAIL"),
                 r.tSwitchMax / 10.0f,
                 r.tNtpMax    / 10.0f);
        Serial.println(line);
        if (r.done && r.evOk && r.ntpOk) passed++;
    }
    Serial.print(F("\n  ")); Serial.print(passed);
    Serial.print(F("/")); Serial.print(CASE_COUNT); Serial.println(F(" cases fully passed"));
    Serial.println(F("  (prio = primary interface; sw = worst ENTER->event, approximate;"));
    Serial.println(F("   ntp = worst event->sync.)"));
    Serial.println(F("  Type a case number + ENTER to re-run just that case;"));
    Serial.println(F("  'r' resets and runs the whole matrix again."));
    Serial.println(F("===========================================================================\n"));
}

// -----------------------------------------------------------------------------
// Profile configuration for the current case
// -----------------------------------------------------------------------------

static void configureProfiles() {
    bool wifiFirst = wifiPrimary();
    bool statik    = useStatic();

    NetworkProfile::NetworkConfig eth;
    eth.dhcp     = !statik;
    eth.priority = wifiFirst ? 1 : 0;
    if (statik) {
        eth.ip = ETH_STATIC_IP; eth.mask = ETH_STATIC_MASK;
        eth.gateway = ETH_STATIC_GATEWAY; eth.dns[0] = ETH_STATIC_DNS;
    }
    strncpy(eth.ntp[0], ETH_NTP, Host::MAX_FQDN_LEN);
    ethProfile.setConfig(eth);

    WiFiProfile::WiFiConfig wifi;
    wifi.dhcp     = !statik;
    wifi.priority = wifiFirst ? 0 : 1;
    if (statik) {
        wifi.ip = WIFI_STATIC_IP; wifi.mask = WIFI_STATIC_MASK;
        wifi.gateway = WIFI_STATIC_GATEWAY; wifi.dns[0] = WIFI_STATIC_DNS;
    }
    strncpy(wifi.ssid,     WIFI_SSID,     sizeof(wifi.ssid)     - 1);
    strncpy(wifi.password, WIFI_PASSWORD, sizeof(wifi.password) - 1);
    strncpy(wifi.ntp[0],   WIFI_NTP,      Host::MAX_FQDN_LEN);
    wifiProfile.setConfig(wifi);
}

// -----------------------------------------------------------------------------
// Setup — one case per boot, run start to finish, then restart
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    delay(400);

    storeLoad();

    if (caseIdx() >= CASE_COUNT) {
        printSummary();
        // Idle: 'r' re-runs the whole matrix; a case number + ENTER re-runs just
        // that case (its result is overwritten, the rest of the summary is kept).
        int  num  = -1;
        for (;;) {
            if (Serial.available()) {
                int c = Serial.read();
                if (c == 'r' || c == 'R') { storeReset(); ESP.restart(); }
                else if (c >= '0' && c <= '9') {
                    num = (num < 0 ? 0 : num) * 10 + (c - '0');
                }
                else if (c == '\n' || c == '\r') {
                    if (num >= 0 && num < CASE_COUNT) {
                        g_store.caseIdx = (uint8_t)num;   // jump back to that case
                        g_store.single  = 1;              // return to summary after it
                        storeSave();
                        Serial.print(F("\n--- re-running case ")); Serial.println(num);
                        delay(200);
                        ESP.restart();
                    }
                    num = -1;
                }
            }
            delay(20);
        }
    }

    const Scenario& sc = scenario();

    Serial.println(F("\n================================================"));
    Serial.print(F("CASE "));   Serial.print(caseIdx());
    Serial.print(F("/"));       Serial.print(CASE_COUNT - 1);
    Serial.print(F("   primary=")); Serial.print(wifiPrimary() ? F("WiFi") : F("ETH"));
    Serial.print(F("   addressing=")); Serial.println(useStatic() ? F("static") : F("dhcp"));
    Serial.print(F("SCENARIO ")); Serial.println(sc.name);
    Serial.println(F("  (P = primary interface, S = secondary, disc = total outage)"));
    Serial.println(F("================================================"));

    // The boot itself is part of the scenario, so the physical state must be set
    // BEFORE the manager starts.
    {
        bool ethUp  = wifiPrimary() ? sc.sUpAtBoot : sc.pUpAtBoot;
        bool wifiUp = wifiPrimary() ? sc.pUpAtBoot : sc.sUpAtBoot;
        char buf[100];
        snprintf(buf, sizeof(buf), "SET THE INITIAL STATE:  ETH = %s,  WiFi = %s",
                 ethUp ? "UP" : "DOWN", wifiUp ? "UP" : "DOWN");
        Serial.print(F("\n>>> ")); Serial.println(buf);
        Serial.println(F(">>> Then press ENTER to boot the manager."));
        while (Serial.available()) Serial.read();
        for (;;) {
            if (!Serial.available()) { delay(5); continue; }
            int c = Serial.read();
            if (c == 's' || c == 'S') { Serial.println(F("--- case skipped")); nextCase(false, false, 0, 0); }
            if (c == 'r' || c == 'R') { storeReset(); ESP.restart(); }
            if (c == '\n' || c == '\r') { while (Serial.available()) Serial.read(); break; }
        }
    }

    configureProfiles();

    NetworkManager.onEvent(onNetworkEvent);
    NetworkManager.onNtpSync(onTimeSync);
    NetworkManager.addAdapter(ethAdapter);
    NetworkManager.addAdapter(wifiAdapter);
    NetworkManager.begin();
    NetworkManager.setNtpSyncInterval(NTP_POLL);

    bool     evOk = true, ntpOk = true;
    uint32_t tSwitchMax = 0, tNtpMax = 0, t = 0;

    // ---- the boot transition -------------------------------------------------
    Serial.println(F("\n  [boot]"));
    uint32_t tRef = millis();
    if (!waitEvent(sc.bootExpect, sc.bootServing, tRef, t)) evOk = false;
    if (t > tSwitchMax) tSwitchMax = t;
    if (sc.bootServing != Serv::NONE) {
        if (!waitNtp(t)) ntpOk = false;
        if (t > tNtpMax) tNtpMax = t;
    }

    // ---- the scripted steps --------------------------------------------------
    for (uint8_t i = 0; i < sc.stepCount; i++) {
        const Step& st = sc.steps[i];
        Serial.print(F("\n  [step ")); Serial.print(i + 1);
        Serial.print(F("/")); Serial.print(sc.stepCount); Serial.println(F("]"));

        // Arm the capture BEFORE the prompt: the action happens while we wait,
        // so both the event and the sync can land during waitEnter().
        g_evPending  = false;
        g_ntpPending = false;
        prompt(st.act);
        tRef = millis();

        if (st.expect == Ev::NONE) {
            if (!waitQuiet()) evOk = false;
            continue;
        }

        if (!waitEvent(st.expect, st.serving, tRef, t)) evOk = false;
        if (t > tSwitchMax) tSwitchMax = t;

        if (st.serving != Serv::NONE) {
            if (!waitNtp(t)) ntpOk = false;
            if (t > tNtpMax) tNtpMax = t;
        }
    }

    Serial.print(F("\n  RESULT: events "));
    Serial.print(evOk ? F("PASS") : F("FAIL"));
    Serial.print(F(",  ntp "));
    Serial.println(ntpOk ? F("PASS") : F("FAIL"));

    nextCase(evOk, ntpOk, tSwitchMax, tNtpMax);
}

void loop() {
    NetworkManager.update();   // never reached; setup() restarts the device
    delay(10);
}
