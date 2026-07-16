#include <cstdio>
#include <cstring>
// Host stub of Arduino.h — JUST enough for NetworkManager/NetworkAdapter to
// compile and run a behavioural smoke test on a PC. Not used on device.
#pragma once
#include <cstdint>

extern uint32_t g_millis;          // controllable mock clock (defined in test)
inline uint32_t millis() { return g_millis; }

// Minimal IPAddress: the glue only default-constructs it and returns it.
class IPAddress {
public:
    IPAddress() : _v(0) {}
    explicit IPAddress(uint32_t v) : _v(v) {}
    bool operator==(const IPAddress& o) const { return _v == o._v; }
    uint8_t operator[](int i) const { return (uint8_t)((_v >> (8*i)) & 0xFF); }
    uint32_t raw() const { return _v; }
private:
    uint32_t _v;
};