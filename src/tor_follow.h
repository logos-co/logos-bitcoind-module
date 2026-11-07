#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

// Following Logos' Tor SOCKS port while the node runs. Core reads -onion once, so a port that
// moves takes a restart; this decides when, and what status().privateSends says meanwhile.
// Header-only and clock-free, so tests/tor_follow_test.cpp drives it with mocked tor_module events.
namespace tor_follow {

// One restart for a port that settles: a reconnect or a close-and-reopen sends several events.
constexpr int64_t kDebounceMs = 3000;
// At most one automatic restart per window, so a flapping port cannot loop the node.
constexpr int64_t kMinGapMs = 60000;

// Tor's SOCKS port as last seen: a status_changed event, an answer to status(), or tor_module gone.
struct Seen {
    bool known = false;
    std::string socks;      // IP:PORT; "" when closed
    std::string why;        // why closed
    int64_t changedAt = 0;  // when `socks` last changed
    uint64_t sightings = 0; // so an answer older than the last sighting can be dropped
};

inline void observe(Seen& s, const std::string& socks, const std::string& why, int64_t now) {
    if (!s.known || s.socks != socks) s.changedAt = now;
    s.known = true;
    s.socks = socks;
    s.why = socks.empty() ? why : "";
    ++s.sightings;
}

// An answer to a status() asked when `sightingsAtAsk` held: dropped if anything was seen since.
inline bool answer(Seen& s, uint64_t sightingsAtAsk, const std::string& socks, const std::string& why, int64_t now) {
    if (s.sightings != sightingsAtAsk) return false;
    observe(s, socks, why, now);
    return true;
}

// The running node, as its last start left it.
struct Run {
    bool userStarted = false;  // a user's start() put it there and no stop() has come since
    bool canRestart = false;   // not regtest, and the settings still ask for Logos' Tor
    std::string mode;          // the start's privateSends: on, unavailable, proxy or off
    std::string socksUsed;     // its -onion, IP:PORT; "" unless on
    std::string startWhy;      // why unavailable at start
    bool hadAuto = false;      // an automatic restart has run ...
    int64_t lastAutoAt = 0;    // ... at this time
};

struct Decision {
    std::string privateSends;  // on, proxy, off, unavailable, waiting or restart
    std::string reason;
    std::string restartTo;     // Tor's port a pending automatic restart moves to; "" for none
    int64_t dueAt = -1;        // when it may run
};

inline Decision decide(const Run& r, const Seen& s, int64_t now) {
    if (r.mode != "on" && r.mode != "unavailable") return {r.mode, "", "", -1};
    const bool open = s.known && !s.socks.empty();
    if (r.mode == "on" && (!s.known || s.socks == r.socksUsed)) return {"on", "", "", -1};
    if (r.mode == "on" && !open) return {"waiting", s.why, "", -1};
    if (r.mode == "unavailable" && !open) return {"unavailable", s.known ? s.why : r.startWhy, "", -1};
    if (!r.userStarted || !r.canRestart) {
        if (r.mode == "unavailable") return {"unavailable", r.startWhy, "", -1};
        return {"waiting", "Tor's SOCKS port moved to " + s.socks + "; the node takes it at its next start", "", -1};
    }
    int64_t due = s.changedAt + kDebounceMs;
    if (r.hadAuto) due = std::max(due, r.lastAutoAt + kMinGapMs);
    const std::string when = due - now > kDebounceMs ? "within a minute" : "in a few seconds";
    const std::string what = r.mode == "on" ? "Tor moved to " + s.socks + "; the node restarts " + when + " to use it"
                                            : "Tor opened its SOCKS port at " + s.socks + "; the node restarts " + when
                                              + " to send over it";
    return {"restart", what, s.socks, due};
}

// The reason status() gives while an automatic restart stops and starts the node.
inline std::string restartingReason(const std::string& mode, const std::string& to) {
    return (mode == "on" ? "Tor moved to " : "Tor opened its SOCKS port at ") + to + "; the node is restarting to use it";
}

}  // namespace tor_follow
