/*
 * Header-only: the Arduino IDE compiles libraries separately from the sketch, so
 * a precompiled unit would not see the configuration macros you define — being
 * header-only, the library is compiled with each includer's macros instead.
 * In a multi-file project, define those macros globally so every translation
 * unit agrees (see the README, "Where to define these").
 */

#pragma once

#include "NetworkProfile.h"

#if NTP_PROFILE_ENABLED
#  include <NtpProfile.h>
#endif

#ifndef NETWORK_ADAPTER_MUTEX_TIMEOUT
#define NETWORK_ADAPTER_MUTEX_TIMEOUT 1000
#endif

#ifndef NETWORK_ADAPTER_RETRY_INTERVAL
#define NETWORK_ADAPTER_RETRY_INTERVAL 15000
#endif

#ifndef NETWORK_ADAPTER_START_RETRY_MAX
#define NETWORK_ADAPTER_START_RETRY_MAX 300000
#endif

#ifndef ARDUINO_ARCH_AVR
#  include <atomic>
#endif
#if !defined(ARDUINO_ARCH_AVR) && !defined(ARDUINO_ARCH_ESP8266)
#  include <mutex>
#  include <chrono>
#endif

/**
 * @brief Live link details that only some interfaces have.
 *
 * Kept out of NetworkStatus on purpose. That struct is cached in every adapter
 * and returned by value, so a derived "WiFiStatus" would be sliced away at both
 * points, and widening it would grow the cache on interfaces that have none of
 * these fields — including AVR builds.
 *
 * Each field carries its own validity flag: an interface may report a signal but
 * no BSSID, and "no reading" is not the same fact as a reading of zero.
 */
struct LinkInfo {
    /// 802.11 caps an SSID at 32 bytes; kept local so this header does not have
    /// to pull in WiFiProfile on interfaces that have no Wi-Fi at all.
    static constexpr size_t MAX_SSID_SIZE = 33;

    char    ssid[MAX_SSID_SIZE] = {};                  ///< Associated network name.
    int8_t  rssi     = 0;                              ///< Signal strength, dBm.
    uint8_t bssid[6] = {};                             ///< Access point MAC.
    bool    hasSsid  = false;
    bool    hasRssi  = false;
    bool    hasBssid = false;
};

/**
 * @brief Immutable snapshot of an interface's live network parameters.
 *
 * Returned by NetworkAdapter::getStatus() and NetworkManagerClass::getStatus().
 * Holds the values that are only meaningful while connected and that DHCP may
 * change at runtime: address, mask, gateway and DNS servers. NTP is NOT included
 * — it is a configured value rather than a live netif property, and belongs to
 * the device instead of the interface: read it from the NtpProfile, or through
 * NetworkManagerClass::getNtp(). When the interface is not connected, `connected` is
 * false and every address is INADDR_ANY (0.0.0.0).
 */
struct NetworkStatus {
    /// Which interface this describes. UNKNOWN only in the snapshot the manager
    /// returns when no adapter is active; every adapter's own snapshot carries
    /// its profile's type, connected or not.
    NetworkProfile::InterfaceType interfaceType =
        NetworkProfile::InterfaceType::UNKNOWN;
    bool      connected = false;                       ///< True while a valid IP is held.
    IPAddress localIP;                                 ///< Assigned address (0.0.0.0 if down).
    IPAddress subnetMask;                              ///< Subnet mask.
    IPAddress gateway;                                 ///< Default gateway.
    IPAddress dns[NetworkProfile::DNS_SERVER_COUNT];   ///< DNS servers.

    /**
     * @brief The interface's own MAC address.
     *
     * Part of the snapshot rather than a separate call, so it always describes
     * the same interface as the addresses beside it. Asking an adapter for its
     * status and then asking the manager for a MAC is two independent
     * selections, and on a device with more than one interface they can
     * disagree — which is precisely what a snapshot exists to prevent.
     *
     * Filled by each adapter's snapshot builder, not by the base class: every
     * adapter here owns its MAC through its profile, but an interface that
     * learns its address from the hardware would have to undo a base-class
     * default rather than simply state its own.
     *
     * Valid whether or not the interface is connected. It is set at
     * construction, so a snapshot carries it before the interface has ever
     * started, and kept across a disconnect, since losing an address is what
     * being disconnected means and the hardware still has a MAC.
     */
    NetworkProfile::MACAddress mac = {0, 0, 0, 0, 0, 0};
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

    /**
     * @brief Longest wait between start() attempts after repeated start()
     *        failures (see _retryStartInterval()).
     */
    static constexpr uint32_t START_RETRY_MAX = NETWORK_ADAPTER_START_RETRY_MAX;

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
        // A starting value, so the snapshot carries a MAC before the interface
        // has ever been started and built one. From the profile, which is where
        // every adapter in this library takes it from; one that does not will
        // replace it on its first _buildStatus().
        profile.getMac(_status.mac);
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
     *         (e.g. hardware not available, profile invalid). The manager
     *         records a false return: the adapter is not probed again until
     *         _retryStartInterval() has passed, and the manager tries the
     *         next adapter meanwhile.
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
     * when the adapter is IDLE, the retry interval has elapsed since the
     * last failure, and the start retry interval since the last failed
     * start() (_startRetryDue()), preventing rapid reconnect loops on
     * persistent failures.
     *
     * Subclasses may override this to add hardware-level checks (e.g.
     * Ethernet link state) so that a probe is skipped when the underlying
     * hardware is clearly unavailable. An override must keep the
     * _startRetryDue() check: without it an adapter whose start() keeps
     * failing is probed on every update(), and the adapters after it never
     * get a turn.
     *
     * @return true if start() is worth calling on this adapter now.
     */
    virtual bool canProbe() const {
        if (getState() != State::IDLE) return false;
        if (!_startRetryDue()) return false;
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
     *
     * Takes no lock. The state is written from whichever task classified the
     * change, so the member is atomic and this is a single atomic load — safe
     * to call from any task, and what the manager's _snapshot() relies on. It
     * is a point-in-time answer, not a reservation: the state may change the
     * instant it returns, so act on the value rather than re-reading it.
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
     * @brief Fills @p out with live link details, if this interface has any.
     *
     * Default: none. Wi-Fi adapters override it. The manager asks the active
     * adapter, so no downcast and no interface-type switch is needed, and a new
     * interface type adds its own answer rather than another branch here.
     *
     * @param out Filled only when the function returns true.
     * @return true if any field was set.
     */
    virtual bool getLinkInfo(LinkInfo& out) const { (void)out; return false; }

#if NTP_PROFILE_ENABLED
    /**
     * @brief Whether this interface should take its NTP servers from DHCP.
     *
     * Set by the manager before start(), and read where the interface kicks off
     * its DHCP query — which is the only moment it can be acted on. lwIP decides
     * from this flag whether to keep DHCP option 42 at all, so a decision made
     * after the lease arrives comes too late: the data is already gone.
     *
     * Defaults to true, matching "nothing configured, so take what the network
     * offers".
     */
    void setNtpFromDhcp(bool fromDhcp) { _ntpFromDhcp = fromDhcp; }

    /** @copydoc setNtpFromDhcp */
    bool ntpFromDhcp() const { return _ntpFromDhcp; }

    /**
     * @brief Binds this interface's own NTP servers.
     *
     * Optional. With no profile bound, the manager falls back to the one given
     * to its own setNtpProfile(), so a device whose interfaces should share a
     * clock source needs nothing here.
     *
     * Bind one per interface when they must differ — typically when one network
     * offers a time server over DHCP (option 42) and the other does not. The
     * manager needs to know which it is dealing with *before* it starts the
     * interface, because lwIP decides whether to keep option 42 during the DHCP
     * handshake and cannot be told afterwards. Only the adapter knows that
     * early; a device-level profile cannot, since on a fallback the application
     * has not yet been told which interface is taking over.
     *
     * Set it before begin(), or from the loop task. The pointer is read on the
     * loop task without a lock, so re-aiming it from another task (a web
     * handler) is not synchronised.
     *
     * @param profile Servers for this interface. Must outlive the adapter.
     */
    void setNtpProfile(NtpProfile& profile) { _ntpProfile = &profile; }

    /// Unbinds, returning this interface to the manager's device-level profile.
    void clearNtpProfile() { _ntpProfile = nullptr; }

    /// The bound profile, or nullptr when the device-level one applies.
    NtpProfile* ntpProfile() const { return _ntpProfile; }
#endif

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

    /**
     * @brief Asks for the profile to be re-applied, from any task.
     *
     * Sets a flag; update() does the stop()/start() on the task that drives the
     * manager. Use this whenever the caller is not on that task — a web request
     * handler on ESP32 runs on the async server's task, and tearing an
     * interface down from there is the kind of cross-task hardware work the
     * whole deferral machinery exists to avoid.
     *
     * NetworkManager::applyProfile() is the other half of the pair: it stops
     * and starts immediately, which is what a caller already on the manager's
     * task wants, and only that caller.
     *
     * Update the profile first — this re-applies whatever it holds when
     * update() gets to it, not a snapshot taken here. Only one request is
     * pending at a time; a second before update() runs replaces the first,
     * which changes nothing, since both ask for the same thing.
     */
    void requestApply() { _pendingApply = true; }

protected:
#if NTP_PROFILE_ENABLED
    bool        _ntpFromDhcp = true;    ///< see setNtpFromDhcp()
    NtpProfile* _ntpProfile  = nullptr; ///< see setNtpProfile(); null = use the manager's
#endif

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
        // Both of these survive the clear, and for the same reason: they say
        // what the interface is, not what its link is doing. Kept rather than
        // re-read, so whatever the adapter last reported stands — a MAC taken
        // from hardware is not replaced here by one from the profile.
        NetworkProfile::InterfaceType t = _status.interfaceType;
        NetworkProfile::MACAddress    m;
        memcpy(m, _status.mac, NetworkProfile::MAC_LEN);

        _status = NetworkStatus{};
        _status.interfaceType = t;
        memcpy(_status.mac, m, NetworkProfile::MAC_LEN);
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

    /**
     * @brief Wait before the next start() after consecutive start() failures.
     *
     * RETRY_INTERVAL after the first failure, doubled for each further one,
     * capped at START_RETRY_MAX (15 s, 30 s, 60 s, ... 5 min by default);
     * 0 when the last start() succeeded. Separate from the fixed interval
     * after a FAILED connection: a start() that fails is a hardware or
     * configuration fault (an Ethernet chip that does not answer), and
     * retrying it is not free — on ESP32 it re-initialises the SPI bus and
     * the driver, and logs every time.
     */
    uint32_t _retryStartInterval() const {
        const uint8_t n = _loadStartFailures();
        if (n == 0) return 0;
        uint32_t interval = RETRY_INTERVAL;
        for (uint8_t i = 1; i < n && interval < START_RETRY_MAX; i++) interval *= 2;
        return interval < START_RETRY_MAX ? interval : START_RETRY_MAX;
    }

    /// True unless a failed start() is still within _retryStartInterval().
    bool _startRetryDue() const {
        if (_loadStartFailures() == 0) return true;
#ifndef ARDUINO_ARCH_AVR
        const uint32_t since = millis() - _startFailedMs.load();
#else
        const uint32_t since = millis() - _startFailedMs;
#endif
        return since >= _retryStartInterval();
    }

    // -------------------------------------------------------------------------
    // Deferred profile re-apply
    // -------------------------------------------------------------------------

private:
    /**
     * @brief start(), with its outcome recorded for _retryStartInterval().
     *
     * The manager starts adapters only through this. A failure stamps the time
     * and counts; a success clears the count. Loop task only (the counter is
     * read from the event task too, hence atomic, but written here alone).
     *
     * @return What start() returned.
     */
    bool _tryStart() {
        const bool started = start();
        if (started) {
            _storeStartFailures(0);
        } else {
#ifndef ARDUINO_ARCH_AVR
            _startFailedMs.store(millis());
#else
            _startFailedMs = millis();
#endif
            const uint8_t n = _loadStartFailures();
            if (n < 255) _storeStartFailures((uint8_t)(n + 1));
        }
        return started;
    }

#ifndef ARDUINO_ARCH_AVR
    uint8_t _loadStartFailures() const { return _startFailures.load(); }
    void    _storeStartFailures(uint8_t n) { _startFailures.store(n); }
#else
    uint8_t _loadStartFailures() const { return _startFailures; }
    void    _storeStartFailures(uint8_t n) { _startFailures = n; }
#endif

    /**
     * @brief Atomically reads and clears the pending apply flag.
     *
     * The deferral is one-way on purpose, and the symmetry is tempting enough
     * to say why. An apply is asked for from outside, through requestApply(),
     * on whatever task the application happens to be on — so the flag belongs
     * to the adapter being asked. A stop is not: the Core decides it and
     * reports it as Decision::stopIdx, so the request already exists on the
     * manager's side and is deferred there, in _pendingStopBits[]. A matching
     * flag here would be a second copy of that mechanism with nothing to set
     * it.
     *
     * @return true if an apply was requested since the last call.
     */
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

    friend class NetworkManagerClass; ///< Accesses _tryStart(), _consumePendingApply() and _onStateChange.

    // _state is written by _setState() on whichever task classified the change
    // (the SDK network-event task on ESP32) and read by getState() from the loop
    // task with no lock — the manager's mutex is a different mutex and buys
    // nothing here. Atomic for the same reason _pendingApply and _lastFailedMs
    // below are: a cross-task read of a plain member is not a defined load, and
    // the manager's snapshot logic is documented as relying on one that is.
#ifndef ARDUINO_ARCH_AVR
    std::atomic<State> _state;    ///< Current connection state.
#else
    State         _state;         ///< Current connection state (AVR: single task).
#endif
    StateChangeCb _onStateChange; ///< Callback registered by NetworkManager.

#ifndef ARDUINO_ARCH_AVR
    std::atomic<bool> _pendingApply { false };
    // Consecutive failed start() calls and the time of the last one. Plain
    // loads and stores only (no read-modify-write), so no libatomic is needed
    // on the xtensa-lx106.
    std::atomic<uint8_t>  _startFailures { 0 };
    std::atomic<uint32_t> _startFailedMs { 0 };
#else
    bool _pendingApply = false;
    uint8_t  _startFailures = 0;
    uint32_t _startFailedMs = 0;
#endif
};
