/*
 * NetworkManagerCore.h
 *
 * PURE DECISION LOGIC for priority-based network fallback. This header contains
 * NO Arduino, NO hardware calls, NO threading primitives (no std::atomic, no
 * std::mutex) and NO platform headers — only <cstdint>. That is the whole point:
 *
 *   - It compiles and runs on a host PC, so the fallback/restore/reconnect logic
 *     can be unit-tested in milliseconds instead of by flashing an ESP32 and
 *     physically unplugging cables. See nm_harness.cpp.
 *   - It has no idea what an "adapter" is as an object. It never starts, stops,
 *     or talks to anything. It is given a SNAPSHOT of the world (which adapters
 *     are in which state, which are worth probing) plus the current time, and it
 *     returns an INTENT ("start index 1", "stop index 0", "emit RESTORED"). The
 *     glue layer (NetworkManager.h) is what actually executes those intents
 *     against real hardware.
 *
 * Why split it out at all? The orchestration logic — the FALLBACK / RESTORED /
 * DISCONNECTED classification — is the subtle part, and it has nothing to do
 * with hardware. Keeping it behind a host-testable boundary means it can be
 * exercised exhaustively on a PC (see nm_harness.cpp) before any of it runs on
 * a device, so its behaviour is proven rather than assumed.
 *
 * THREADING CONTRACT (important — read before touching the glue):
 *   The Core is NOT thread-safe and deliberately holds no locks. Instead, the
 *   glue guarantees that every Core call (onConnected / onFailed / tick) happens
 *   under the single NetworkManager mutex. Because all access is serialised by
 *   that one external lock, the Core's own state (_lastServingIdx,
 *   _disconnectGrace, _disconnectReported) needs no atomics: concentrating all
 *   policy state here lets concurrency be handled in exactly one place (the
 *   glue's lock discipline) instead of being reasoned about field by field.
 */

#pragma once

// <cstdint> requires a conforming C++11 stdlib. The AVR toolchain (avr-gcc
// 7.x, -std=gnu++11) ships without it but does provide the C header instead.
#if defined(__AVR__)
#   include <stdint.h>
#else
#   include <cstdint>
#endif

#ifndef NETWORK_MANAGER_MAX_ADAPTERS
#   define NETWORK_MANAGER_MAX_ADAPTERS 4
#endif

#ifndef NETWORK_MANAGER_RECONNECT_TIMEOUT
#   define NETWORK_MANAGER_RECONNECT_TIMEOUT 60000
#endif

/**
 * @brief Platform-independent fallback/restore decision engine.
 *
 * Drives a set of adapters that are RANKED BY PRIORITY, where a lower index in
 * the snapshot means higher priority (index 0 is the most-preferred interface).
 * The glue is responsible for sorting adapters by priority before taking the
 * snapshot, so the Core only ever reasons about indices, never about raw
 * priority numbers.
 */
class NetworkManagerCore {
public:
    /** @brief Connection state of a single adapter, mirrored from the glue. */
    enum class State : uint8_t { IDLE, CONNECTING, CONNECTED, FAILED };

    /**
     * @brief High-level events the Core can ask the glue to emit.
     *
     * NONE is the "no event this call" value so a Decision can always be
     * returned by value. The public NetworkManagerClass::Event enum is a
     * separate, API-stable type; the glue maps Core::Event onto it. (We keep
     * the two separate so the Core stays independent of the library's public
     * API, and so reordering one never silently changes the other.)
     *
     * Note: there is intentionally no RECONNECTING here. That public event is
     * currently reserved/unemitted — see the note in NetworkManager.h for why a
     * naive wiring produces noise during ordinary fallback gaps.
     */
    enum class Event : uint8_t { NONE, CONNECTED, DISCONNECTED, FALLBACK, RESTORED };

    /**
     * @brief Immutable snapshot of the adapter set at one instant.
     *
     * The glue fills this in before every Core call. It is a plain value type on
     * purpose: the Core works on a frozen copy, never on live adapter objects,
     * which is what makes it pure and testable.
     *
     * @var state    Per-adapter connection state (index 0 = highest priority).
     * @var canProbe Per-adapter "is it worth trying to (re)start now?" — already
     *               includes any hardware gating (e.g. Ethernet link-up,
     *               retry-interval back-off). The Core treats it as an opaque
     *               bool so all hardware knowledge stays in the adapter/glue.
     * @var count    Number of valid entries in the arrays.
     */
    struct StateView {
        State   state[NETWORK_MANAGER_MAX_ADAPTERS];
        bool    canProbe[NETWORK_MANAGER_MAX_ADAPTERS];
        uint8_t count = 0;
    };

    /**
     * @brief What the glue should do as a result of a Core call.
     *
     * Every field is independent and optional, so a single call can, for
     * example, both emit DISCONNECTED and start a probe in the same tick. The
     * glue executes whichever fields are set:
     *
     * @var emit     Event to deliver to the application (NONE = none).
     * @var emitIdx  Adapter index the event refers to (the connected adapter for
     *               CONNECTED/FALLBACK/RESTORED; 0 for DISCONNECTED).
     * @var startIdx Adapter to start(), or -1. Only ever set by tick() (the loop
     *               task), so the glue may start it directly — never deferred.
     * @var stopIdx  Adapter to stop(), or -1. Only ever set by onConnected()
     *               (RESTORED supersede) or onFailed(). Because those run in
     *               event-handler context, the glue MUST defer the stop to the
     *               next tick (calling WiFi.disconnect()/ETH teardown from an
     *               event handler risks re-entrancy / stack overflow).
     */
    struct Decision {
        Event  emit     = Event::NONE;
        int8_t emitIdx  = -1;
        int8_t startIdx = -1;
        int8_t stopIdx  = -1;
    };

    /**
     * @brief Constructs the core.
     * @param reconnectTimeout Milliseconds with nothing connected before
     *        DISCONNECTED is emitted. 0 disables the DISCONNECTED escalation.
     */
    explicit NetworkManagerCore(
            uint32_t reconnectTimeout = NETWORK_MANAGER_RECONNECT_TIMEOUT)
        : _reconnectTimeout(reconnectTimeout)
    {}

    /**
     * @brief Resets all policy state. Called by the glue's end().
     *
     * After this the next connection is classified as a cold CONNECTED.
     */
    void reset() {
        _lastServingIdx     = -1;
        _disconnectGrace    = 0;
        _disconnectReported = false;
    }

    /**
     * @brief Classify an adapter reaching CONNECTED.
     *
     * The serving adapter is RE-DERIVED from the snapshot every time
     * (lowestConnected()) rather than tracked incrementally. Deriving it fresh
     * on each call means there is no separate "currently serving" variable to
     * keep in sync across the several branches below — the snapshot is the
     * single source of truth, so it cannot drift out of step with reality.
     *
     * Why deriving from live state is safe despite the cross-adapter handoff
     * window (where the OUTGOING adapter may still read CONNECTED for a few
     * hundred ms because the core's disconnect event is asynchronous):
     *   - RESTORED: the higher-priority adapter that just came up is the LOWEST
     *     connected index, so it wins regardless of whether the lower one still
     *     reads CONNECTED.
     *   - FALLBACK: the failed primary is already IDLE/FAILED by the time the
     *     lower adapter gets an IP (it failed FIRST — that is *why* we fell
     *     back), so it is not in the connected set at all.
     * Taking the lowest connected index is therefore correct in both directions
     * even from a slightly stale snapshot. The glue guarantees the one fact we
     * rely on: the adapter that triggered this call reads CONNECTED in the view.
     *
     * @param idx  Index of the adapter that just connected (for reference; the
     *             decision is driven by the snapshot, not this alone).
     * @param view Snapshot taken by the glue.
     * @param now  Current millis().
     */
    Decision onConnected(uint8_t /*idx*/, const StateView& view, uint32_t /*now*/) {
        Decision d;

        int8_t serving = _lowestConnected(view);
        if (serving < 0) {
            // Defensive: a CONNECTED callback with nobody connected in the view
            // should not happen (the glue snapshots AFTER _setState(CONNECTED)).
            // If it ever does, do nothing rather than misclassify.
            return d;
        }

        const int8_t last = _lastServingIdx;

        if (last < 0) {
            // Nobody was serving before: cold boot, or the first connection
            // after a DISCONNECTED reset. Always a plain CONNECTED — there is no
            // previous interface to be a fallback from or a restore of.
            d.emit = Event::CONNECTED;
            d.emitIdx = serving;
        } else if (serving < last) {
            // A higher-priority interface took over from a lower-priority one.
            // Report RESTORED and tear down the now-superseded adapter. The stop
            // is returned as stopIdx (deferred by the glue), never done here.
            d.emit = Event::RESTORED;
            d.emitIdx = serving;
            d.stopIdx = last;
        } else if (serving > last) {
            // A lower-priority interface took over. This only happens after the
            // higher-priority one failed, so it is a fallback. Report FALLBACK.
            // Deliberately NO timer is started here: the DISCONNECTED grace clock
            // is owned solely by tick(), and a working fallback must NOT count
            // toward DISCONNECTED — while any interface is serving, the link is
            // up, just on a lower-priority path.
            d.emit = Event::FALLBACK;
            d.emitIdx = serving;
        } else {
            // serving == last: the same adapter re-confirmed CONNECTED (e.g. a
            // duplicate GOT_IP). No priority change, so no event and no action.
        }

        // Whoever is serving now becomes the reference for future events, and
        // any pending disconnect grace is cancelled because we have a connection.
        _lastServingIdx     = serving;
        _disconnectGrace    = 0;
        _disconnectReported = false;
        return d;
    }

    /**
     * @brief Classify an adapter reaching FAILED.
     *
     * The Core's only response to a failure is to ask the glue to stop that
     * adapter (logical teardown + transition to IDLE), which the glue defers to
     * the next tick. The CONSEQUENCE of the failure — fallback to another
     * interface, or escalation to DISCONNECTED — is decided later by tick()
     * once the snapshot reflects the new reality. Splitting it this way keeps
     * each Core call a pure function of (snapshot, time).
     *
     * Note _lastServingIdx is intentionally NOT changed here: a failure does not
     * erase "who served last". That memory is what lets the eventual FALLBACK vs
     * RESTORED classification stay correct, and it is only cleared when
     * DISCONNECTED is finally emitted.
     */
    Decision onFailed(uint8_t idx, const StateView& /*view*/, uint32_t /*now*/) {
        Decision d;
        d.stopIdx = (int8_t)idx;
        return d;
    }

    /**
     * @brief Periodic decision: probing and the disconnect timeout.
     *
     * Called once per glue update() (loop task), AFTER the glue has applied any
     * deferred stops, so the snapshot reflects settled state.
     *
     * Does three things, in order:
     *   1. Start the disconnect grace clock if the serving set just emptied.
     *   2. Probe: pick at most one adapter to (re)start, frontier-gated.
     *   3. Escalate to DISCONNECTED if the grace period has fully elapsed.
     */
    Decision tick(const StateView& view, uint32_t now) {
        Decision d;

        const int8_t connected = _lowestConnected(view);

        // (1) Grace clock. The clock runs whenever nothing is connected and
        // this outage has not yet been reported — including from boot, where
        // nothing has ever served. A working fallback keeps `connected >= 0`,
        // so the clock stays at 0 and DISCONNECTED never fires while any
        // interface is serving.
        if (connected < 0 && !_disconnectReported && _disconnectGrace == 0) {
            _disconnectGrace = now;
            if (_disconnectGrace == 0) _disconnectGrace = 1;  // 0 is the "off" sentinel
        }

        // (2) Probe loop, gated by the "serving frontier".
        //
        // The frontier is the lowest index that is already CONNECTED or
        // CONNECTING. The rule is: only ever (re)start an adapter STRICTLY ABOVE
        // the frontier (i.e. higher priority / lower index). Consequences:
        //   - Nobody serving (frontier < 0): probe the best ready adapter
        //     -> initial connect / reconnect after total loss.
        //   - A lower-priority adapter serving (frontier == k): probe only
        //     indices < k -> higher-priority restore attempt, never disturbing
        //     the working fallback.
        //   - The best adapter already serving (frontier == 0): the loop breaks
        //     immediately -> nothing is probed, so a happy primary is never
        //     disturbed and a lower-priority adapter is never started underneath
        //     it (which would otherwise cause a spurious fallback).
        // Only one adapter may be CONNECTING at a time, so if anything is
        // already CONNECTING we skip probing entirely this tick.
        if (!_anyConnecting(view)) {
            const int8_t frontier = _servingFrontier(view);
            for (uint8_t i = 0; i < view.count; i++) {
                if (frontier >= 0 && (int8_t)i >= frontier) break;  // don't touch frontier or below
                if (view.canProbe[i]) {
                    d.startIdx = (int8_t)i;
                    break;
                }
            }
        }

        // (3) Disconnect escalation. Only after the grace period has fully
        // elapsed with nothing connected do we tell the app we are DISCONNECTED
        // and reset _lastServingIdx so the next connection is a cold CONNECTED.
        if (_reconnectTimeout > 0 &&
            _disconnectGrace != 0 &&
            now - _disconnectGrace > _reconnectTimeout) {
            d.emit    = Event::DISCONNECTED;
            // Report the interface we lost rather than a fixed index. At boot
            // nothing ever served, so fall back to the primary adapter.
            d.emitIdx = (_lastServingIdx >= 0) ? _lastServingIdx : 0;
            _disconnectGrace    = 0;
            _lastServingIdx     = -1;
            _disconnectReported = true;   // do not repeat while the outage lasts
        }

        return d;
    }

    // --- introspection helpers (handy for tests / status) -------------------
    int8_t   lastServingIdx()  const { return _lastServingIdx; }
    uint32_t disconnectGrace() const { return _disconnectGrace; }

private:
    // ---- policy state (serialised by the glue's mutex; no atomics needed) ----

    // Index of the lowest-priority-number adapter that was last reported as
    // serving (CONNECTED) to the application, or -1 if none. "Sticky": only
    // updated while an adapter is serving; a stop/fail does NOT change it. This
    // is the single reference point for FALLBACK vs RESTORED. Reset to -1 only
    // when DISCONNECTED is emitted (or via reset()).
    int8_t _lastServingIdx = -1;

    // millis() timestamp of when the serving set became empty, or 0 when
    // something is serving. SOLE purpose: the DISCONNECTED grace period — it has
    // exactly one meaning and is never overloaded to also signal "we are in
    // fallback", which keeps the grace logic easy to reason about. (millis() can
    // legitimately be 0 at boot; tick() bumps a 0 timestamp to 1 so 0 stays an
    // unambiguous "off".)
    uint32_t _disconnectGrace = 0;

    // DISCONNECTED has already been emitted for the current outage. This splits a
    // meaning that _lastServingIdx used to carry twice: "never served" AND
    // "outage already reported". Keeping them apart lets the grace clock run from
    // boot (a boot with no network is a genuine outage and is reported once),
    // while still preventing a repeat every reconnect timeout while it persists.
    bool    _disconnectReported = false;

    const uint32_t _reconnectTimeout;

    // ---- pure snapshot queries ----

    /** @brief Lowest index that is CONNECTED, or -1. = the current server. */
    static int8_t _lowestConnected(const StateView& v) {
        for (uint8_t i = 0; i < v.count; i++)
            if (v.state[i] == State::CONNECTED) return (int8_t)i;
        return -1;
    }

    /** @brief Lowest index that is CONNECTED or CONNECTING, or -1. = frontier. */
    static int8_t _servingFrontier(const StateView& v) {
        for (uint8_t i = 0; i < v.count; i++)
            if (v.state[i] == State::CONNECTED || v.state[i] == State::CONNECTING)
                return (int8_t)i;
        return -1;
    }

    /** @brief True if any adapter is mid-connect (gates the probe loop). */
    static bool _anyConnecting(const StateView& v) {
        for (uint8_t i = 0; i < v.count; i++)
            if (v.state[i] == State::CONNECTING) return true;
        return false;
    }
};