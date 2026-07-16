// test_glue.cpp — compile + behaviour smoke test for the REAL shipping glue.
//
// Unlike nm_harness.cpp (which tests the Core in isolation), this instantiates
// the actual NetworkManagerClass singleton from NetworkManager.h, wired to the
// actual NetworkAdapter base class, driven by a mock concrete adapter. It proves
// two things the Core-only test cannot:
//   1. NetworkManager.h + NetworkAdapter.h actually COMPILE (C++ type/syntax).
//   2. The full event-task / loop-task wiring produces the right events end to
//      end through the real classes.
//
//   NTP/Host code is excluded via NETWORK_PROFILE_NTP_SERVER_COUNT = 0.
//
// Build: g++ -std=c++17 -O0 -Wall test_glue.cpp -o test_glue && ./test_glue

#define NETWORK_PROFILE_NTP_SERVER_COUNT  0
#define NETWORK_MANAGER_MAX_ADAPTERS      4
#define NETWORK_MANAGER_RECONNECT_TIMEOUT 60000

#include "NetworkManager.h"   // pulls in NetworkAdapter.h, NetworkManagerCore.h, NetworkProfile.h, Arduino.h
#include <cstdio>
#include <vector>

uint32_t g_millis = 1000;
static void advance(uint32_t ms) { g_millis += ms; }

using Ev = NetworkManagerClass::Event;
static std::vector<Ev> g_log;

static const char* evName(Ev e){
    switch(e){
        case Ev::CONNECTED:    return "CONNECTED";
        case Ev::DISCONNECTED: return "DISCONNECTED";
        case Ev::FALLBACK:     return "FALLBACK";
        case Ev::RESTORED:     return "RESTORED";
        case Ev::RECONNECTING: return "RECONNECTING";
    }
    return "?";
}

// Mock concrete adapter. Drives the real base-class state machine via
// _setState(); the test controls "is this adapter currently probeable?" through
// canProbeFlag so probing is deterministic (the ETH/WiFi canProbe nuances are
// covered exhaustively in nm_harness.cpp; here we only need the glue to fire
// start() at the right moments).
class MockAdapter : public NetworkAdapter {
public:
    bool canProbeFlag = true;

    explicit MockAdapter(NetworkProfile& p) : NetworkAdapter(p) {}

    // test-side stimulus (simulating the hardware/event task)
    void simConnected() { _setState(State::CONNECTED); }
    void simFailed()    { _setState(State::FAILED); }

    // NetworkAdapter interface
    bool start() override { _setState(State::CONNECTING); return true; }
    void stop()  override { _setState(State::IDLE); }
    void update() override {}
    IPAddress getLocalIP() const override { return IPAddress(); }
    bool canProbe() const override {
        return getState() == State::IDLE && canProbeFlag;
    }
};

static void pump(int ticks){ for (int i=0;i<ticks;i++){ NetworkManager.update(); advance(20);} }

int main(){
    NetworkProfile ethProfile(1);    // lower number = higher priority
    NetworkProfile wifiProfile(10);
    MockAdapter eth(ethProfile);
    MockAdapter wifi(wifiProfile);

    NetworkManager.onEvent([](Ev e, NetworkAdapter&){ g_log.push_back(e); });
    NetworkManager.addAdapter(eth);
    NetworkManager.addAdapter(wifi);
    NetworkManager.begin();          // starts highest-priority adapter (ETH)

    // 1) ETH comes up -> CONNECTED
    eth.simConnected();

    // 2) ETH fails -> stop deferred -> WiFi probed -> FALLBACK
    eth.canProbeFlag = false;        // ETH link down
    eth.simFailed();
    pump(3);                         // update() stops ETH, then probes WiFi
    wifi.simConnected();

    // 3) ETH recovers -> probed above the WiFi frontier -> RESTORED, WiFi stopped
    eth.canProbeFlag = true;
    pump(3);                         // update() probes ETH
    eth.simConnected();
    pump(2);                         // update() stops the superseded WiFi

    // 4) both fail -> nothing probeable -> after 60s -> DISCONNECTED
    eth.canProbeFlag = false;
    wifi.canProbeFlag = false;
    eth.simFailed();
    pump(2);
    for (int i=0;i<70;i++){ advance(1000); NetworkManager.update(); }

    // 5) ETH returns -> CONNECTED
    eth.canProbeFlag = true;
    pump(2);
    eth.simConnected();
    pump(2);                         // deferred events drain on update() (loop task)

    // ---- report ----
    std::vector<Ev> expect = {
        Ev::CONNECTED, Ev::FALLBACK, Ev::RESTORED, Ev::DISCONNECTED, Ev::CONNECTED
    };
    printf("=== real-glue smoke test ===\n  got:");
    for (auto e: g_log) printf(" %s", evName(e));
    printf("\n  exp:");
    for (auto e: expect) printf(" %s", evName(e));
    bool ok = (g_log == expect);
    printf("\n  [%s]\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}