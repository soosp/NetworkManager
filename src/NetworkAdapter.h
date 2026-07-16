/*
 * Header-only: the Arduino IDE compiles libraries separately from the sketch, so
 * a precompiled unit would not see the configuration macros you define — being
 * header-only, the library is compiled with each includer's macros instead.
 * In a multi-file project, define those macros globally so every translation
 * unit agrees (see the README, "Where to define these").
 */

#pragma once

#include "NetworkProfile.h"

#ifndef NETWORK_ADAPTER_MUTEX_TIMEOUT
#define NETWORK_ADAPTER_MUTEX_TIMEOUT 1000
#endif

#ifndef NETWORK_ADAPTER_RETRY_INTERVAL
#define NETWORK_ADAPTER_RETRY_INTERVAL 15000
#endif

#ifndef ARDUINO_ARCH_AVR
#  include <atomic>
#endif
#if !defined(ARDUINO_ARCH_AVR) && !defined(ARDUINO_ARCH_ESP8266)
#  include <mutex>
#  include <chrono>
#endif

/**
 * @brief Immutable snapshot of an interface's live network parameters.
 *
 * Returned by NetworkAdapter::getStatus() and NetworkManagerClass::getStatus().
 * Holds the values that are only meaningful while connected and that DHCP may
 * change at runtime: address, mask, gateway and DNS servers. NTP is NOT included
 * — it is a configured profile value, not a live netif property, and is read
 * separately via getNtp(). When the interface is not connected, `connected` is
 * false and every address is INADDR_ANY (0.0.0.0).
 */
struct NetworkStatus {
    NetworkProfile::InterfaceType interfaceType =
        NetworkProfile::InterfaceType::ETH;            ///< Which interface this describes.
    bool      connected = false;                       ///< True while a valid IP is held.
    IPAddress localIP;                                 ///< Assigned address (0.0.0.0 if down).
    IPAddress subnetMask;                              ///< Subnet mask.
    IPAddress gateway;                                 ///< Default gateway.
    IPAddress dns[NetworkProfile::DNS_SERVER_COUNT];   ///< DNS servers.
};

/**
 * @brief Abstract base class for network interface adapters.
 *
 * Represents a single network interface (WiFi, Ethernet, cellular modem)
 * and its associated profile. Subclasses implement platform-specific
 * connection logic while exposing a uniform interface to NetworkManager.
 *
 * Responsibilities:
 *  - Managing the connection state machine (IDLE → CONNECTING → CONNECTED)
 *  - Hardware calls (WiFi.begin(), ETH.config(), AT commands, etc.)
 *  - DHCP timeout detection and signalling via state change callback
 *  - Event handling (ESP32/ESP8266) or polling (AVR, modem) internally
 *
 * The NetworkManager is responsible for:
 *  - Deciding which adapter to start based on priority
 *  - Reacting to state changes (fallback, restore)
 *  - Calling update() in the main loop
 *
 * @note Subclasses must not block in start() or update().
 * @note Only one NetworkManager instance is supported (singleton).
 */
class NetworkAdapter {
public:
    /** @brief Default mutex acquisition timeout in milliseconds. */
    static constexpr uint32_t MUTEX_TIMEOUT = NETWORK_ADAPTER_MUTEX_TIMEOUT;

    /** @brief Minimum interval between connection attempts after a failure. */
    static constexpr uint32_t RETRY_INTERVAL = NETWORK_ADAPTER_RETRY_INTERVAL;

    // -------------------------------------------------------------------------
    // State machine
    // -------------------------------------------------------------------------

    /**
     * @brief Connection states of a network adapter.
     *
     * Transitions:
     *   IDLE       → CONNECTING : start() called successfully
     *   CONNECTING → CONNECTED  : link up and IP obtained
     *   CONNECTING → FAILED     : timeout or authentication failure
     *   CONNECTED  → FAILED     : link lost or IP lost
     *   CONNECTED  → IDLE       : stop() called
     *   FAILED     → IDLE       : stop() called
     *   FAILED     → CONNECTING : start() called again by manager
     *
     * @note The NetworkManager monitors the CONNECTING state to prevent
     *       starting multiple adapters simultaneously. Subclasses must
     *       transition to CONNECTING in start() before returning.
     */
    enum class State : uint8_t {
        IDLE,       ///< Not started or cleanly stopped.
        CONNECTING, ///< Started, waiting for link and IP.
        CONNECTED,  ///< Link up and IP obtained.
        FAILED,     ///< Connection lost or could not be established.
    };

    // -------------------------------------------------------------------------
    // Callback
    // -------------------------------------------------------------------------

    /**
     * @brief Callback type invoked by the adapter on every state change.
     *
     * Registered by NetworkManager via setOnStateChange().
     * The callback is called outside the adapter's internal mutex.
     *
     * @param adapter Reference to the adapter that changed state.
     * @param state   The new state.
     */
    using StateChangeCb = void(*)(NetworkAdapter& adapter, State state);

    // -------------------------------------------------------------------------
    // Constructor / Destructor
    // -------------------------------------------------------------------------

    /**
     * @brief Constructs a NetworkAdapter with an associated profile.
     * @param profile Reference to the profile describing this interface.
     */
    explicit NetworkAdapter(NetworkProfile& profile)
        : _profile(profile)
        , _state(State::IDLE)
        , _onStateChange(nullptr)
    {
        _status.interfaceType = profile.getInterfaceType();
    }

    virtual ~NetworkAdapter() = default;

    // -------------------------------------------------------------------------
    // Interface — must be implemented by subclasses
    // -------------------------------------------------------------------------

    /**
     * @brief Starts the connection process.
     *
     * Must not block. The adapter transitions to CONNECTING and
     * completes the process asynchronously via events or update().
     *
     * @return true if the start was initiated, false on failure
     *         (e.g. hardware not available, profile invalid).
     */
    virtual bool start() = 0;

    /**
     * @brief Stops the connection and releases the interface.
     *
     * Transitions the adapter to IDLE. Safe to call in any state.
     */
    virtual void stop() = 0;

    /**
     * @brief Returns true if this adapter is a valid candidate to (re)start.
     *
     * Called by NetworkManager::update() to decide whether to attempt a
     * connection on this adapter. The default implementation returns true
     * when the adapter is IDLE and the retry interval has elapsed since the
     * last failure, preventing rapid reconnect loops on persistent failures.
     *
     * Subclasses may override this to add hardware-level checks (e.g.
     * Ethernet link state) so that a probe is skipped when the underlying
     * hardware is clearly unavailable.
     *
     * @return true if start() is worth calling on this adapter now.
     */
    virtual bool canProbe() const {
        if (getState() != State::IDLE) return false;
#ifndef ARDUINO_ARCH_AVR
        uint32_t last = _lastFailedMs.load();
        if (last == 0) return true;  // never failed — allow immediately
        return millis() - last > RETRY_INTERVAL;
#else
        if (_lastFailedMs == 0) return true;
        return millis() - _lastFailedMs > RETRY_INTERVAL;
#endif
    }

    /**
     * @brief Drives the adapter state machine.
     *
     * On ESP32/ESP8266: lightweight — checks DHCP timeout only.
     * On AVR and modem: performs polling to detect state changes.
     *
     * Called by NetworkManagerClass::update() from the main loop.
     */
    virtual void update() = 0;

    /** @brief Returns the actual IP address of the adapter. */
    virtual IPAddress getLocalIP() const = 0;

#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)
    /**
     * @brief Makes this interface the default route (ESP only).
     *
     * lwIP keeps a single default route/netif that is not re-elected when a
     * superseded adapter is stopped; the manager calls this on the serving
     * adapter after a teardown so outgoing traffic uses the live interface.
     * Default is a no-op (e.g. an adapter whose stack manages this itself).
     */
    virtual void setDefaultRoute() {}
#endif

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the current connection state.
     *
     * The CONNECTING state is used by NetworkManager to detect whether
     * an adapter is already starting up, preventing duplicate start() calls.
     */
    State getState() const { return _state; }

    /**
     * @brief Returns a reference to the associated network profile.
     */
    NetworkProfile& getProfile() { return _profile; }

    /**
     * @brief Returns a const reference to the associated network profile.
     */
    const NetworkProfile& getProfile() const { return _profile; }

    /**
     * @brief Returns a consistent snapshot of the interface's live parameters.
     *
     * The snapshot is captured by the adapter when it acquires or renews an
     * address, so reading it never touches hardware and is safe under any lock.
     * When the interface is not connected, the result has connected=false and
     * every address INADDR_ANY.
     */
    NetworkStatus getStatus() const {
        NetworkStatus s;
        if (_lock()) { s = _status; _unlock(); }
        return s;
    }

    /** @brief Subnet mask from the last snapshot (0.0.0.0 if not connected). */
    IPAddress getSubnetMask() const { return getStatus().subnetMask; }

    /** @brief Default gateway from the last snapshot (0.0.0.0 if not connected). */
    IPAddress getGatewayIP() const { return getStatus().gateway; }

    /**
     * @brief DNS server @p i from the last snapshot (0.0.0.0 if not connected).
     * @param i Index in [0, NetworkProfile::DNS_SERVER_COUNT); clamped to 0 if out of range.
     */
    IPAddress getDns(uint8_t i = 0) const {
        if (i >= NetworkProfile::DNS_SERVER_COUNT) i = 0;
        return getStatus().dns[i];
    }

    /**
     * @brief Returns the interface hostname (delegates to the profile).
     *
     * With ConfigSource::ACTIVE (default) this is the effective hostname — the
     * user override if set, otherwise the generated default; with
     * ConfigSource::FACTORY it is always the generated default. It is
     * deterministic configuration, not runtime state, so it is available even
     * before the interface connects.
     *
     * @param buf    Destination buffer.
     * @param len    Capacity of @p buf.
     * @param source ConfigSource::ACTIVE (default) or ConfigSource::FACTORY.
     * @return true on success; false if @p buf is null or too small.
     */
    bool getHostname(char* buf, size_t len,
                     NetworkProfile::ConfigSource source =
                         NetworkProfile::ConfigSource::ACTIVE) const {
        return getProfile().getHostname(buf, len, source);
    }

    /**
     * @brief Returns the interface MAC address (delegates to the profile).
     *
     * With ConfigSource::ACTIVE (default) this is the effective MAC — the user
     * override if set, otherwise the generated default; with
     * ConfigSource::FACTORY it is always the generated default.
     *
     * @param mac    Destination buffer (MAC_LEN bytes).
     * @param source ConfigSource::ACTIVE (default) or ConfigSource::FACTORY.
     * @return true on success.
     */
    bool getMac(NetworkProfile::MACAddress mac,
                NetworkProfile::ConfigSource source =
                    NetworkProfile::ConfigSource::ACTIVE) const {
        return getProfile().getMac(mac, source);
    }

    /**
     * @brief Registers a callback to be invoked on state changes.
     *
     * Called by NetworkManagerClass::begin(). Only one callback is supported.
     *
     * @param cb Function pointer to invoke, or nullptr to unregister.
     */
    void setOnStateChange(StateChangeCb cb) { _onStateChange = cb; }

protected:
    // -------------------------------------------------------------------------
    // State transition
    // -------------------------------------------------------------------------

    /**
     * @brief Transitions to a new state and invokes the callback if registered.
     *
     * Skips transition and callback if the new state equals the current state.
     * Records the timestamp when transitioning to FAILED, for use by canProbe()
     * to enforce the retry interval.
     * The callback is invoked outside the mutex to avoid deadlocks.
     *
     * @param newState The state to transition to.
     */
    void _setState(State newState) {
        if (!_lock()) return;
        State prev = _state;
        StateChangeCb cb = nullptr;
        if (prev != newState) {
            _state = newState;
            cb = _onStateChange;
            if (newState == State::FAILED) {
#ifndef ARDUINO_ARCH_AVR
                _lastFailedMs.store(millis());
#else
                _lastFailedMs = millis();
#endif
            }
        }
        _unlock();
        if (cb) cb(*this, newState);
    }

    /**
     * @brief Caches a fresh status snapshot (subclasses call this at GOT_IP /
     *        poll-detected connect, and on renew when the address changes).
     *
     * Stores the live netif parameters under the adapter lock so getStatus() and
     * the derived getters return a consistent snapshot without touching hardware.
     */
    void _cacheStatus(const NetworkStatus& s) {
        if (!_lock()) return;
        _status = s;
        _unlock();
    }

    /** @brief Clears the cached snapshot (subclasses call this on disconnect/stop). */
    void _clearStatus() {
        if (!_lock()) return;
        NetworkProfile::InterfaceType t = _status.interfaceType;
        _status = NetworkStatus{};
        _status.interfaceType = t;
        _unlock();
    }

    NetworkProfile& _profile; ///< Associated network profile.
    NetworkStatus   _status;  ///< Cached live snapshot, refreshed by subclasses at GOT_IP.

#ifndef ARDUINO_ARCH_AVR
    // Timestamp of the last transition to FAILED (or explicit stop), used by
    // canProbe() to enforce the retry interval. Subclasses may also write this
    // (e.g. ESP32EthAdapter::stop()) to prevent immediate restart after RESTORED.
    std::atomic<uint32_t> _lastFailedMs { 0 };
#else
    uint32_t _lastFailedMs = 0;
#endif

    // -------------------------------------------------------------------------
    // Deferred profile re-apply
    // -------------------------------------------------------------------------

    /**
     * @brief Requests a profile re-apply from any task.
     *
     * Thread-safe alternative to NetworkManager::applyProfile() for use from
     * tasks other than the network management task (e.g. a web server task).
     * The actual stop()/start() sequence is deferred to the next update() call
     * on the network management task.
     *
     * Only one pending request is held at a time. Calling requestApply() again
     * before update() processes the previous request is safe but overwrites it
     * (both calls would result in the same action anyway).
     *
     * @note The profile must be updated before calling this method.
     */
    void requestApply() { _pendingApply = true; }

private:
    /**
     * @brief Requests a deferred stop from the event task.
     *
     * Called by NetworkManager::_handleStateChange() from the event task
     * when the adapter transitions to FAILED. The actual stop() is deferred
     * to the next update() call on the loop task to avoid calling
     * WiFi.disconnect() from the event task (stack overflow risk).
     */
    void _requestStop() {
#ifndef ARDUINO_ARCH_AVR
        _pendingStop.store(true);
#else
        _pendingStop = true;
#endif
    }

    /**
     * @brief Atomically reads and clears the pending stop flag.
     * @return true if a stop was requested since the last call.
     */
    bool _consumePendingStop() {
#if defined(ARDUINO_ARCH_ESP32)
        return _pendingStop.exchange(false);
#elif defined(ARDUINO_ARCH_ESP8266)
        // Cooperative scheduling: nothing preempts between load and store, so a
        // non-atomic read-then-clear is safe and avoids __atomic_exchange (no
        // libatomic on xtensa-lx106). Atomic load/store still ensure visibility.
        bool v = _pendingStop.load();
        _pendingStop.store(false);
        return v;
#else
        bool v = _pendingStop;
        _pendingStop = false;
        return v;
#endif
    }

    bool _consumePendingApply() {
#if defined(ARDUINO_ARCH_ESP32)
        return _pendingApply.exchange(false);
#elif defined(ARDUINO_ARCH_ESP8266)
        // Cooperative scheduling: nothing preempts between load and store, so a
        // non-atomic read-then-clear is safe and avoids __atomic_exchange (no
        // libatomic on xtensa-lx106). Atomic load/store still ensure visibility.
        bool v = _pendingApply.load();
        _pendingApply.store(false);
        return v;
#else
        bool v  = _pendingApply;
        _pendingApply = false;
        return v;
#endif
    }

    // -------------------------------------------------------------------------
    // Mutex
    // -------------------------------------------------------------------------

    // Real timed mutex only where there is preemptive concurrency (ESP32 and
    // the host-test build). On AVR (single-threaded) and ESP8266 (cooperative
    // scheduling, no preemption) the lock is a no-op; the atomics above still
    // guard flags written from callback context.
#if !defined(ARDUINO_ARCH_AVR) && !defined(ARDUINO_ARCH_ESP8266)
    mutable std::timed_mutex _mutex;

    bool _lock() const {
        return _mutex.try_lock_for(
            std::chrono::milliseconds(MUTEX_TIMEOUT));
    }
    void _unlock() const { _mutex.unlock(); }
#else
    bool _lock()   const { return true; }
    void _unlock() const {}
#endif

    friend class NetworkManagerClass; ///< Accesses _consumePendingApply(), _consumePendingStop() and _onStateChange.

    State         _state;         ///< Current connection state.
    StateChangeCb _onStateChange; ///< Callback registered by NetworkManager.

#ifndef ARDUINO_ARCH_AVR
    std::atomic<bool> _pendingApply { false };
    std::atomic<bool> _pendingStop  { false };
#else
    bool _pendingApply = false;
    bool _pendingStop  = false;
#endif
};