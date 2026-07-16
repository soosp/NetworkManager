// Host stub of NetworkProfile.h — the glue only needs getPriority() (for
// priority sorting); the adapter base only holds a NetworkProfile& reference.
// The real profile layer (persistence, NTP, Host) is irrelevant to the glue's
// decision wiring, which is what this host test validates. The hostname/MAC
// members below exist only so the manager's delegating accessors compile; they
// are deliberately inert.
#pragma once
#include <cstdint>
#include <cstddef>
#include "Arduino.h"

class NetworkProfile {
public:
    enum class InterfaceType : uint8_t { UNKNOWN, WIFI, ETH };
    enum class ConfigSource  : uint8_t { ACTIVE = 0, FACTORY = 1 };

    static constexpr uint8_t DNS_SERVER_COUNT = 2;
    static constexpr uint8_t MAC_LEN          = 6;
    typedef uint8_t MACAddress[MAC_LEN];

    explicit NetworkProfile(uint8_t priority = 10) : _priority(priority) {}
    uint8_t getPriority() const { return _priority; }
    InterfaceType getInterfaceType() const { return _ifType; }
    void setInterfaceType(InterfaceType t) { _ifType = t; } // test helper

    // Inert hostname/MAC accessors — present so NetworkAdapter's and the
    // manager's delegating getters compile against the stub.
    bool getHostname(char* buf, size_t len, ConfigSource = ConfigSource::ACTIVE) const {
        if (!buf || len == 0) return false;
        buf[0] = '\0';
        return true;
    }
    bool setHostname(const char*) { return true; }
    bool getMac(MACAddress mac, ConfigSource = ConfigSource::ACTIVE) const {
        for (uint8_t i = 0; i < MAC_LEN; i++) mac[i] = 0;
        return true;
    }

private:
    uint8_t       _priority;
    InterfaceType _ifType = InterfaceType::ETH;
};