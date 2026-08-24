// Host stub of NetworkProfile.h — the glue only needs getPriority() (for
// priority sorting); the adapter base only holds a NetworkProfile& reference.
// The real profile layer (persistence, NTP, Host) is irrelevant to the glue's
// decision wiring, which is what this host test validates. The hostname/MAC
// members below exist only so the manager's delegating accessors compile; they
// are deliberately inert.
//
// **What this file owes the real header.** It is a mirror of the *shape* the
// shipping code uses, not of its behaviour: this test compiles the real
// NetworkManager.h and checks the event sequence, so a member that is missing
// here fails the build, while one that merely computes something different would
// go unnoticed. When the shipping code starts using a profile member — or a
// helper the real header pulls in, as happened when statusToJson() took up the
// PROGMEM macros and the JSON tools — add it here. The test cannot prove the
// header compiles if it stops compiling the header.
//
// Kept as a stub rather than including the real NetworkProfile.h so this suite
// runs on its own, without the NetworkProfile and Host libraries installed
// beside it.
//
// **This file's contract is shape, not behaviour.** It exists so the test can
// run without NetworkProfile and Host being installed alongside; the price is
// that whenever the shipping code starts using something new from the profile
// layer, this stub has to follow. It did not, once: `statusToJson()` began
// calling `ipToStr()` and the JSON helpers when the ESP8266 port landed, and
// `test_glue` — whose whole purpose is to prove the real header compiles —
// quietly stopped compiling at all. Nobody noticed, because nobody ran it.
//
// So: if a build fails here, the fix is to add what the real profile provides,
// not to work around it in the code under test. And run these tests.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include "Arduino.h"

// JSON helpers, normally reached through the real NetworkProfile's
// detail/json_tools.h. Implemented rather than left as empty shims so the code
// under test runs as it would on a device — though what this harness actually
// checks is that it *compiles* and that the event sequence is right, not the
// JSON it produces.
inline bool json_fitted(int written, size_t cap) {
    return written >= 0 && (size_t)written < cap;
}

/// Appends only if the whole of @p src fits — all or nothing.
inline bool json_catAtomic(char* dst, const char* src, size_t dstSize) {
    if (!dst || !src) return false;
    const size_t used = strlen(dst);
    const size_t add  = strlen(src);
    if (used + add + 1 > dstSize) return false;
    memcpy(dst + used, src, add + 1);
    return true;
}
inline bool json_catAtomic_P(char* dst, PGM_P src, size_t dstSize) {
    return json_catAtomic(dst, src, dstSize);
}

/// Escapes @p src into @p dst per RFC 8259, truncating rather than overrunning.
inline size_t json_escape(char* dst, size_t dstLen, const char* src) {
    if (!dst || !src || dstLen == 0) return 0;
    size_t w = 0;
    while (*src && w + 1 < dstLen) {
        const unsigned char c = (unsigned char)*src++;
        if (c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') {
            dst[w++] = (char)c;
        } else {
            if (w + 6 >= dstLen) break;
            w += snprintf(dst + w, dstLen - w, "\\u%04X", c);
        }
    }
    dst[w] = '\0';
    return w;
}

/// Escaping append with no scratch buffer; truncates rather than overrunning.
/// Not atomic: what fits is kept and false is returned.
inline bool json_cat_esc(char* dst, const char* src, size_t dstSize) {
    if (!dst || !src) return false;
    size_t w = strlen(dst);
    while (*src) {
        const unsigned char c = (unsigned char)*src++;
        if (c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') {
            if (w + 1 >= dstSize) { dst[w] = '\0'; return false; }
            dst[w++] = (char)c;
        } else {
            if (w + 6 >= dstSize) { dst[w] = '\0'; return false; }
            w += snprintf(dst + w, dstSize - w, "\\u%04X", c);
        }
    }
    dst[w] = '\0';
    return true;
}

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

    /// Dotted-decimal rendering, as the real profile provides it.
    static bool ipToStr(char* out, size_t len, const IPAddress& ip) {
        if (!out || len == 0) return false;
        return json_fitted(snprintf(out, len, "%u.%u.%u.%u",
                                    (unsigned)ip[0], (unsigned)ip[1],
                                    (unsigned)ip[2], (unsigned)ip[3]), len);
    }
    bool getMac(MACAddress mac, ConfigSource = ConfigSource::ACTIVE) const {
        for (uint8_t i = 0; i < MAC_LEN; i++) mac[i] = 0;
        return true;
    }

private:
    uint8_t       _priority;
    InterfaceType _ifType = InterfaceType::ETH;
};