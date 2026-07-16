// nm_harness.cpp — host regression suite for NetworkManagerCore.
//
// This drives the REAL NetworkManagerCore (not a copy) through a thin test glue
// that mirrors exactly what NetworkManager.h does on the device:
//
//   event task -> onConnected()/onFailed()  (classify; defer stops via a bitmap)
//   loop  task -> tick()                     (apply deferred stops, probe, timeout)
//
// Each scenario asserts the exact sequence of high-level events. Run with:
//   g++ -std=c++17 -O0 -Wall nm_harness.cpp -o nm_harness && ./nm_harness
//
// Keep this in the repo. Any change to the fallback/restore/reconnect logic
// should be validated here in seconds before it ever touches hardware.

#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <functional>

#define NETWORK_MANAGER_MAX_ADAPTERS      4
#define NETWORK_MANAGER_RECONNECT_TIMEOUT 60000
#include "NetworkManagerCore.h"

using State = NetworkManagerCore::State;
using Event = NetworkManagerCore::Event;

// ---- mocked clock ----------------------------------------------------------
static uint32_t g_now = 0;
static uint32_t millis() { return g_now; }
static void advance(uint32_t ms) { g_now += ms; }

static const uint32_t RETRY_INTERVAL = 15000;

static const char* evName(Event e){
    switch(e){
        case Event::CONNECTED:    return "CONNECTED";
        case Event::DISCONNECTED: return "DISCONNECTED";
        case Event::FALLBACK:     return "FALLBACK";
        case Event::RESTORED:     return "RESTORED";
        default:                  return "NONE";
    }
}

// ---- fake adapter ----------------------------------------------------------
// Models only what the Core's snapshot needs plus enough lifecycle to simulate
// the device. linkUp models an Ethernet physical link; isEth selects the
// canProbe() flavour (ETH = link-gated; WiFi = retry-interval only).
struct FakeAdapter {
    State    state        = State::IDLE;
    bool     isEth        = false;
    bool     ethStarted   = false;
    bool     linkUp       = true;
    uint32_t lastFailedMs = 0;
    const char* name      = "?";

    void setState(State s){
        if (s == state) return;
        state = s;
        if (s == State::FAILED) lastFailedMs = millis();
    }
    bool canProbe() const {
        if (state != State::IDLE) return false;
        if (isEth) {
            if (!ethStarted) return true;
            if (lastFailedMs != 0 && millis() - lastFailedMs < RETRY_INTERVAL) return false;
            return linkUp;
        }
        if (lastFailedMs == 0) return true;
        return millis() - lastFailedMs > RETRY_INTERVAL;
    }
    void start(){ if (isEth) ethStarted = true; setState(State::CONNECTING); }
    void stop(){  setState(State::IDLE); if (isEth) lastFailedMs = millis(); }
};

// ---- test glue: the same shape as NetworkManager.h ------------------------
struct Glue {
    std::vector<FakeAdapter*> a;     // pre-sorted by priority (index 0 = best)
    NetworkManagerCore core;
    bool pendingStop[NETWORK_MANAGER_MAX_ADAPTERS] = {};
    std::vector<Event> log;

    Glue() : core(NETWORK_MANAGER_RECONNECT_TIMEOUT) {}

    NetworkManagerCore::StateView snapshot(){
        NetworkManagerCore::StateView v;
        v.count = (uint8_t)a.size();
        for (uint8_t i=0;i<v.count;i++){ v.state[i]=a[i]->state; v.canProbe[i]=a[i]->canProbe(); }
        return v;
    }
    int8_t indexOf(FakeAdapter* x){
        for (size_t i=0;i<a.size();i++) if (a[i]==x) return (int8_t)i;
        return -1;
    }
    void applyEmit(const NetworkManagerCore::Decision& d){
        if (d.emit != Event::NONE) log.push_back(d.emit);
    }

    // event task entry points
    void begin(){ if(!a.empty()) a[0]->start(); }
    void onConnected(FakeAdapter* ad){
        auto v = snapshot();
        auto d = core.onConnected((uint8_t)indexOf(ad), v, millis());
        if (d.stopIdx >= 0) pendingStop[d.stopIdx] = true;   // deferred
        applyEmit(d);                                        // synchronous, like the device
    }
    void onFailed(FakeAdapter* ad){
        auto v = snapshot();
        auto d = core.onFailed((uint8_t)indexOf(ad), v, millis());
        if (d.stopIdx >= 0) pendingStop[d.stopIdx] = true;
        applyEmit(d);
    }

    // loop task: update()
    void tick(){
        // apply deferred stops first, so tick() sees settled state
        for (size_t i=0;i<a.size();i++) if (pendingStop[i]){ pendingStop[i]=false; a[i]->stop(); }
        auto v = snapshot();
        auto d = core.tick(v, millis());
        if (d.startIdx >= 0) a[d.startIdx]->start();
        applyEmit(d);
    }
};

// ---- scenario framework ----------------------------------------------------
struct Scenario {
    std::string name;
    std::function<void(Glue&)> run;
    std::vector<Event> expect;
};

static void settle(Glue& m, int ticks, uint32_t step=20){
    for (int i=0;i<ticks;i++){ m.tick(); advance(step); }
}

static std::vector<Scenario> scenarios(){
    std::vector<Scenario> S;

    S.push_back({"1  single adapter connects", [](Glue& m){
        auto* A=new FakeAdapter{}; A->name="A"; m.a={A};
        m.begin(); A->setState(State::CONNECTED); m.onConnected(A);
    }, {Event::CONNECTED}});

    S.push_back({"2  primary connects, fallback stays idle (no spurious FALLBACK)", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=true;
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin(); E->setState(State::CONNECTED); m.onConnected(E);
        for (int i=0;i<10;i++){ m.tick(); advance(20);
            if (W->state==State::CONNECTING){ W->setState(State::CONNECTED); m.onConnected(W); } }
    }, {Event::CONNECTED}});

    S.push_back({"3  boot: primary fails, fallback -> CONNECTED (not FALLBACK)", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=true;
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin();
        E->linkUp=false; E->setState(State::FAILED); m.onFailed(E);
        m.tick(); m.tick();
        if (W->state==State::CONNECTING){ W->setState(State::CONNECTED); m.onConnected(W); }
    }, {Event::CONNECTED}});

    S.push_back({"4  ETH fails -> WiFi FALLBACK", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=true;
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin(); E->setState(State::CONNECTED); m.onConnected(E); settle(m,3);
        E->linkUp=false; E->setState(State::FAILED); m.onFailed(E);
        m.tick(); m.tick(); W->setState(State::CONNECTED); m.onConnected(W);
    }, {Event::CONNECTED, Event::FALLBACK}});

    S.push_back({"5  ETH recovers -> RESTORED", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=true;
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin(); E->setState(State::CONNECTED); m.onConnected(E); settle(m,3);
        E->linkUp=false; E->setState(State::FAILED); m.onFailed(E);
        m.tick(); m.tick(); W->setState(State::CONNECTED); m.onConnected(W);   // FALLBACK
        advance(RETRY_INTERVAL+100); E->linkUp=true;
        m.tick();                                  // probe ETH (frontier=1 -> probe idx 0)
        E->setState(State::CONNECTED); m.onConnected(E);                       // RESTORED
        m.tick();                                  // stop WiFi
    }, {Event::CONNECTED, Event::FALLBACK, Event::RESTORED}});

    S.push_back({"6  both fail -> DISCONNECTED", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=true;
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin(); E->setState(State::CONNECTED); m.onConnected(E); settle(m,3);
        E->linkUp=false; E->setState(State::FAILED); m.onFailed(E);
        m.tick(); m.tick(); W->setState(State::CONNECTED); m.onConnected(W);   // FALLBACK
        W->setState(State::FAILED); m.onFailed(W); m.tick();
        for (int i=0;i<70;i++){ advance(1000); m.tick(); }
    }, {Event::CONNECTED, Event::FALLBACK, Event::DISCONNECTED}});

    S.push_back({"7  return after DISCONNECTED -> CONNECTED", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=true;
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin(); E->setState(State::CONNECTED); m.onConnected(E); settle(m,3);
        E->linkUp=false; E->setState(State::FAILED); m.onFailed(E);
        m.tick(); m.tick(); W->setState(State::CONNECTED); m.onConnected(W);   // FALLBACK
        W->setState(State::FAILED); m.onFailed(W); m.tick();
        for (int i=0;i<70;i++){ advance(1000); m.tick(); }                     // DISCONNECTED
        advance(RETRY_INTERVAL+100); E->linkUp=true;
        m.tick();
        E->setState(State::CONNECTED); m.onConnected(E);                       // CONNECTED
    }, {Event::CONNECTED, Event::FALLBACK, Event::DISCONNECTED, Event::CONNECTED}});

    S.push_back({"8  persistent fallback, no false DISCONNECTED", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=true;
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin(); E->setState(State::CONNECTED); m.onConnected(E); settle(m,3);
        E->linkUp=false; E->setState(State::FAILED); m.onFailed(E);
        m.tick(); m.tick(); W->setState(State::CONNECTED); m.onConnected(W);   // FALLBACK
        for (int i=0;i<120;i++){ advance(1000); m.tick(); }                    // WiFi stays up
    }, {Event::CONNECTED, Event::FALLBACK}});

    S.push_back({"9  boot: nothing available -> DISCONNECTED", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=false;  // no link
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin();
        for (int i=0;i<70;i++){ advance(1000); m.tick();
            if (W->state==State::CONNECTING){ W->setState(State::FAILED); m.onFailed(W); } }
    }, {Event::DISCONNECTED}});

    S.push_back({"10 boot: nothing, then a network appears -> DISCONNECTED, CONNECTED", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=false;
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin();
        for (int i=0;i<70;i++){ advance(1000); m.tick();
            if (W->state==State::CONNECTING){ W->setState(State::FAILED); m.onFailed(W); } }  // DISCONNECTED
        advance(RETRY_INTERVAL+100); E->linkUp=true;
        m.tick();
        E->setState(State::CONNECTED); m.onConnected(E);                                      // cold CONNECTED
    }, {Event::DISCONNECTED, Event::CONNECTED}});

    S.push_back({"11 persistent total outage reports DISCONNECTED once", [](Glue& m){
        auto* E=new FakeAdapter{}; E->name="ETH"; E->isEth=true; E->linkUp=true;
        auto* W=new FakeAdapter{}; W->name="WiFi";
        m.a={E,W};
        m.begin(); E->setState(State::CONNECTED); m.onConnected(E); settle(m,3);
        E->linkUp=false; E->setState(State::FAILED); m.onFailed(E);
        W->setState(State::FAILED); m.onFailed(W);
        for (int i=0;i<240;i++){ advance(1000); m.tick();                 // 4 minutes
            if (W->state==State::CONNECTING){ W->setState(State::FAILED); m.onFailed(W); } }
    }, {Event::CONNECTED, Event::DISCONNECTED}});

    return S;
}

static bool runOne(const Scenario& sc){
    g_now = 1000;
    Glue m;
    sc.run(m);
    bool ok = (m.log == sc.expect);
    printf("  [%s] %s\n        got:", ok?"PASS":"FAIL", sc.name.c_str());
    for (auto e: m.log) printf(" %s", evName(e));
    if (!ok){ printf("\n        exp:"); for (auto e: sc.expect) printf(" %s", evName(e)); }
    printf("\n");
    return ok;
}

int main(){
    auto S = scenarios();
    int pass=0;
    printf("=== NetworkManagerCore regression suite ===\n");
    for (auto& sc: S) pass += runOne(sc);
    printf("\n  %d/%zu passed\n", pass, S.size());
    return pass == (int)S.size() ? 0 : 1;
}