// test_link_health.cpp — the per-adapter link-health counters.
//
// They are diagnostics only, and that is most of what is tested here: a drop
// the manager rides out is counted while the application sees no DISCONNECTED,
// and counting changes no event the manager emits. The rest is the counting
// rules: only from CONNECTED, LINK and ADDRESS apart, the reason kept for LINK
// alone, the time of either.
//
// Drives the real NetworkManagerClass singleton, like test_glue.cpp (a separate
// binary, so a fresh singleton). The mock reports its losses the way the real
// adapters do: _noteDrop() first, then _setState(FAILED).
//
// Build: g++ -std=c++17 -O0 -Wall -I. -I../src test_link_health.cpp -o test_link_health

#define NTP_PROFILE_ENABLED  0
#define NETWORK_MANAGER_MAX_ADAPTERS      1
#define NETWORK_MANAGER_RECONNECT_TIMEOUT 60000

#include "NetworkManager.h"
#include <cstdio>
#include <vector>

uint32_t g_millis = 1000;

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { g_failures++; \
    printf("  FAIL line %d: %s\n", __LINE__, #expr); } } while (0)

using Ev = NetworkManagerClass::Event;
static std::vector<Ev> g_log;

class MockAdapter : public NetworkAdapter {
public:
    explicit MockAdapter(NetworkProfile& p) : NetworkAdapter(p) {}

    void simConnected() { _setState(State::CONNECTED); }
    void simLost(DropKind k, uint16_t reason = 0) {
        _noteDrop(k, reason);
        _setState(State::FAILED);
    }

    bool start() override { _setState(State::CONNECTING); return true; }
    void stop()  override { _setState(State::IDLE); }
    void update() override {}
    IPAddress getLocalIP() const override { return IPAddress(); }
};

static void run(uint32_t ms, uint32_t step = 20) {
    for (uint32_t t = 0; t < ms; t += step) {
        NetworkManager.update();
        g_millis += step;
    }
}

static size_t count(Ev e) {
    size_t n = 0;
    for (Ev x : g_log) if (x == e) n++;
    return n;
}

int main() {
    printf("=== link-health counters ===\n");
    NetworkProfile wifiProfile(10);
    MockAdapter wifi(wifiProfile);
    using K = NetworkAdapter::DropKind;

    NetworkManager.onEvent([](Ev e, NetworkAdapter&) { g_log.push_back(e); });
    NetworkManager.addAdapter(wifi);

    // Nothing yet
    CHECK(wifi.linkDrops() == 0 && wifi.addressDrops() == 0);
    CHECK(wifi.lastDropReason() == 0 && wifi.lastDropMs() == 0);

    // A failed attempt is not a drop
    CHECK(NetworkManager.begin());
    CHECK(wifi.getState() == NetworkAdapter::State::CONNECTING);
    wifi.simLost(K::LINK, 201);
    CHECK(wifi.linkDrops() == 0 && wifi.lastDropReason() == 0);

    // Connect
    run(20 * 1000);
    wifi.simConnected();
    run(100);
    CHECK(count(Ev::CONNECTED) == 1);

    // A link drop ridden out: counted, and no DISCONNECTED for the app
    const uint32_t t1 = g_millis;
    wifi.simLost(K::LINK, 200);
    CHECK(wifi.linkDrops() == 1);
    CHECK(wifi.lastDropReason() == 200);
    CHECK(wifi.lastDropMs() == t1);
    run(NetworkAdapter::RETRY_INTERVAL + 1000);
    wifi.simConnected();
    run(100);
    CHECK(count(Ev::DISCONNECTED) == 0);

    // An address drop: its own count, the link reason left alone
    run(5000);
    const uint32_t t2 = g_millis;
    wifi.simLost(K::ADDRESS);
    CHECK(wifi.addressDrops() == 1);
    CHECK(wifi.linkDrops() == 1);
    CHECK(wifi.lastDropReason() == 200);
    CHECK(wifi.lastDropMs() == t2);

    // The snapshot agrees with the getters
    {
        const NetworkAdapter::LinkHealth h = wifi.linkHealth();
        CHECK(h.linkDrops == 1 && h.addressDrops == 1);
        CHECK(h.lastDropReason == 200 && h.lastDropMs == t2);
    }

    // Repeated failures while down are not further drops
    run(NetworkAdapter::RETRY_INTERVAL + 1000);       // restarted: CONNECTING
    wifi.simLost(K::LINK, 15);
    CHECK(wifi.linkDrops() == 1 && wifi.lastDropReason() == 200);

    // A long outage: the app now hears DISCONNECTED, and the counters still
    // say one LINK and one ADDRESS — the event and the counts are independent
    run(NETWORK_MANAGER_RECONNECT_TIMEOUT + 5000);
    CHECK(count(Ev::DISCONNECTED) == 1);
    CHECK(wifi.linkDrops() == 1 && wifi.addressDrops() == 1);

    // A stop() of our own is not a drop
    wifi.simConnected();
    run(100);
    wifi.stop();
    CHECK(wifi.linkDrops() == 1 && wifi.addressDrops() == 1);

    if (g_failures == 0) printf("  all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
