// test_start_retry.cpp — an adapter whose start() fails must not starve the
// others, and is retried with a growing interval.
//
// Like test_glue.cpp this drives the real NetworkManagerClass singleton (a
// separate binary, so a fresh singleton). The mock adapter keeps the base
// class canProbe(), which is the code under test: a start() that returns
// false (an Ethernet chip that does not answer, wrong pins) used to leave the
// adapter IDLE and probeable at once, so the manager retried it on every
// update() and never reached the next adapter.
//
// Build: g++ -std=c++17 -O0 -Wall -I. -I../src test_start_retry.cpp -o test_start_retry

#define NTP_PROFILE_ENABLED  0
#define NETWORK_MANAGER_MAX_ADAPTERS      2
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
    bool startOk = true;
    std::vector<uint32_t> startTimes;          // every start() call

    explicit MockAdapter(NetworkProfile& p) : NetworkAdapter(p) {}

    void simConnected() { _setState(State::CONNECTED); }
    uint32_t retryStartInterval() const { return _retryStartInterval(); }

    bool start() override {
        startTimes.push_back(g_millis);
        if (!startOk) return false;            // stays IDLE, like ESP32EthAdapter
        _setState(State::CONNECTING);
        return true;
    }
    void stop()  override { _setState(State::IDLE); }
    void update() override {}
    IPAddress getLocalIP() const override { return IPAddress(); }
    // canProbe(): the base class, deliberately
};

static void run(uint32_t ms, uint32_t step = 20) {
    for (uint32_t t = 0; t < ms; t += step) {
        NetworkManager.update();
        g_millis += step;
    }
}

int main() {
    printf("=== start() failure retry ===\n");
    NetworkProfile ethProfile(1);
    NetworkProfile wifiProfile(10);
    MockAdapter eth(ethProfile);
    MockAdapter wifi(wifiProfile);

    NetworkManager.onEvent([](Ev e, NetworkAdapter&) { g_log.push_back(e); });
    NetworkManager.addAdapter(eth);
    NetworkManager.addAdapter(wifi);

    // The primary's chip does not answer
    eth.startOk = false;
    CHECK(!NetworkManager.begin());            // begin() still reports the primary
    CHECK(eth.startTimes.size() == 1);
    CHECK(eth.retryStartInterval() == NetworkAdapter::RETRY_INTERVAL);

    // The secondary is started on the next update(), not after the interval
    run(40);
    CHECK(eth.startTimes.size() == 1);         // not hammered
    CHECK(wifi.startTimes.size() == 1);
    wifi.simConnected();
    run(40);
    CHECK(wifi.getState() == NetworkAdapter::State::CONNECTED);

    // While the secondary serves, the primary is retried after 15, 30, 60,
    // 120, 240, then every 300 s
    run(30UL * 60UL * 1000UL, 100);
    const uint32_t expectS[] = {15, 30, 60, 120, 240, 300, 300};
    CHECK(eth.startTimes.size() >= 8);
    for (size_t i = 0; i + 1 < eth.startTimes.size() && i < 7; i++) {
        const uint32_t gap = eth.startTimes[i + 1] - eth.startTimes[i];
        // one update() step of slack
        if (gap < expectS[i] * 1000UL || gap > expectS[i] * 1000UL + 200) {
            printf("  retry %u after %lu ms, expected %lu s\n", (unsigned)(i + 1),
                   (unsigned long)gap, (unsigned long)expectS[i]);
            g_failures++;
        }
    }
    CHECK(eth.retryStartInterval() == NetworkAdapter::START_RETRY_MAX);
    CHECK(wifi.startTimes.size() == 1);        // never disturbed
    CHECK(wifi.getState() == NetworkAdapter::State::CONNECTED);

    // The chip answers again: the next retry starts it, RESTORED, WiFi stopped,
    // and the failure count is gone
    eth.startOk = true;
    const size_t before = eth.startTimes.size();
    run(NetworkAdapter::START_RETRY_MAX + 1000, 100);
    CHECK(eth.startTimes.size() == before + 1);
    CHECK(eth.getState() == NetworkAdapter::State::CONNECTING);
    CHECK(eth.retryStartInterval() == 0);
    eth.simConnected();
    run(100);
    CHECK(wifi.getState() == NetworkAdapter::State::IDLE);

    const std::vector<Ev> expect = {Ev::CONNECTED, Ev::RESTORED};
    CHECK(g_log == expect);
    if (g_log != expect) {
        printf("  events:");
        for (Ev e : g_log) printf(" %d", (int)e);
        printf("\n");
    }

    printf("  [%s]\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
