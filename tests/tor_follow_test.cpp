// Following Tor's SOCKS port with mocked tor_module events and a clock the test moves: when the
// node restarts on its own, what status().privateSends says meanwhile, and what an automatic
// restart carries over.
#include "../src/carry_over.h"
#include "../src/private_sends.h"
#include "../src/tor_follow.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace tor_follow;

static int failures = 0;
static int checks = 0;

static void expect(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
        ++failures;
    }
}

static std::string show(const Decision& d) {
    return d.privateSends + " | " + d.reason + " | " + d.restartTo + " @" + std::to_string(d.dueAt);
}

// tor_module's status_changed payload, as status.rs to_json() writes it.
static std::string payload(const char* socks) {
    return std::string(R"({"ok":true,"state":"ready","progress":100,"detail":"","socks":)")
           + (socks ? "\"" + std::string(socks) + "\"" : "null") + R"(,"since":1,"joinMs":2,"blockage":null})";
}

// What the module's callback does with an event.
static void event(Seen& s, const char* socks, int64_t now) {
    const private_sends::TorSocks t = private_sends::torSocksFrom(payload(socks));
    if (t.answered) observe(s, t.socks, t.why, now);
}

static Run onAt(const std::string& socks) {
    Run r;
    r.userStarted = true;
    r.canRestart = true;
    r.mode = "on";
    r.socksUsed = socks;
    return r;
}

// The module's poll loop in miniature: every 100 ms it decides, and a due restart moves the node
// to Tor's port. Returns how many automatic restarts ran.
struct Sim {
    Run run = onAt("127.0.0.1:9250");
    Seen seen;
    int64_t now = 1000000;
    int restarts = 0;
    void tick(int64_t ms) {
        for (int64_t end = now + ms; now < end; now += 100) {
            const Decision d = decide(run, seen, now);
            if (d.restartTo.empty() || now < d.dueAt) continue;
            ++restarts;
            run.mode = "on";
            run.socksUsed = d.restartTo;
            run.hadAuto = true;
            run.lastAutoAt = now;
        }
    }
};

static void decisions() {
    const int64_t t = 1000000;
    {
        Seen s;
        Run r = onAt("127.0.0.1:9250");
        expect(show(decide(r, s, t)) == "on |  |  @-1", "on, nothing seen yet");
        event(s, "socks5h://127.0.0.1:9250", t);
        expect(decide(r, s, t).privateSends == "on", "on, the same port");
        event(s, "socks5h://127.0.0.1:9251", t + 10);
        Decision d = decide(r, s, t + 10);
        expect(d.privateSends == "restart" && d.restartTo == "127.0.0.1:9251" && d.dueAt == t + 10 + kDebounceMs,
               "moved: a restart 3 s after the move: " + show(d));
        expect(d.reason == "Tor moved to 127.0.0.1:9251; the node restarts in a few seconds to use it", d.reason);
        // Progress events carry the same port and do not push the restart back.
        event(s, "socks5h://127.0.0.1:9251", t + 2000);
        expect(decide(r, s, t + 2000).dueAt == t + 10 + kDebounceMs, "progress keeps the debounce");
        // Another move restarts the debounce, towards the newest port.
        event(s, "socks5h://127.0.0.1:9252", t + 2500);
        d = decide(r, s, t + 2500);
        expect(d.restartTo == "127.0.0.1:9252" && d.dueAt == t + 2500 + kDebounceMs, "moved again: " + show(d));
        // Back where the node started before the restart ran: nothing to do.
        event(s, "socks5h://127.0.0.1:9250", t + 2600);
        expect(show(decide(r, s, t + 2600)) == "on |  |  @-1", "moved back: on");
    }
    {
        // Closed: never a restart, which would start without Tor; waiting until it is back.
        Seen s;
        Run r = onAt("127.0.0.1:9250");
        event(s, nullptr, t);
        Decision d = decide(r, s, t + 60000);
        expect(show(d) == "waiting | Tor's SOCKS port is off |  @-1", "closed: " + show(d));
        // set_socks closing and reopening elsewhere: one restart, 3 s after the reopen.
        event(s, "socks5h://127.0.0.1:9300", t + 500);
        d = decide(r, s, t + 500);
        expect(d.restartTo == "127.0.0.1:9300" && d.dueAt == t + 500 + kDebounceMs, "reopened elsewhere: " + show(d));
        // Reopened at the old address: Core's -onion works again as it is.
        event(s, nullptr, t + 600);
        event(s, "socks5h://127.0.0.1:9250", t + 700);
        expect(decide(r, s, t + 700).privateSends == "on", "reopened at the same address: on");
    }
    {
        // tor_module unloaded: its port closed with no event; the lost subscription says so.
        Seen s;
        Run r = onAt("127.0.0.1:9250");
        event(s, "socks5h://127.0.0.1:9250", t);
        observe(s, "", "tor_module stopped", t + 5);
        expect(show(decide(r, s, t + 5)) == "waiting | tor_module stopped |  @-1", "lost: " + show(decide(r, s, t + 5)));
        // Reloaded, it found 9250 taken by now and listens elsewhere: the arm's status() answers.
        const uint64_t at = s.sightings;
        expect(answer(s, at, "127.0.0.1:61000", "", t + 900), "the arm's answer lands");
        expect(decide(r, s, t + 900).restartTo == "127.0.0.1:61000", "reloaded elsewhere: a restart");
    }
    {
        // An answer to a status() asked before the latest event is older than it: dropped.
        Seen s;
        event(s, "socks5h://127.0.0.1:9250", t);
        const uint64_t at = s.sightings;
        event(s, "socks5h://127.0.0.1:9251", t + 1);
        expect(!answer(s, at, "127.0.0.1:9250", "", t + 2) && s.socks == "127.0.0.1:9251", "a stale answer is dropped");
    }
    {
        // Rate limit: one automatic restart per minute, the pending one when the window ends.
        Seen s;
        Run r = onAt("127.0.0.1:9251");
        r.hadAuto = true;
        r.lastAutoAt = t;
        event(s, "socks5h://127.0.0.1:9252", t + 10000);
        const Decision d = decide(r, s, t + 10000);
        expect(d.dueAt == t + kMinGapMs && d.reason.find("within a minute") != std::string::npos, "rate limited: " + show(d));
        r.lastAutoAt = t - kMinGapMs;
        expect(decide(r, s, t + 10000).dueAt == t + 10000 + kDebounceMs, "outside the window: the debounce alone");
    }
    {
        // Started unavailable: no tor_module, or no port. Tor later gets one: restart into on.
        Seen s;
        Run r = onAt("");
        r.mode = "unavailable";
        r.startWhy = "tor_module is not loaded";
        expect(show(decide(r, s, t)) == "unavailable | tor_module is not loaded |  @-1", "unavailable at start");
        event(s, nullptr, t + 1);
        expect(show(decide(r, s, t + 1)) == "unavailable | Tor's SOCKS port is off |  @-1", "still no port");
        event(s, "socks5h://127.0.0.1:9250", t + 2);
        const Decision d = decide(r, s, t + 2);
        expect(d.privateSends == "restart" && d.restartTo == "127.0.0.1:9250" && d.dueAt == t + 2 + kDebounceMs
                   && d.reason == "Tor opened its SOCKS port at 127.0.0.1:9250; the node restarts in a few seconds to send over it",
               "Tor got a port: " + show(d));
        expect(restartingReason("unavailable", "127.0.0.1:9250") == "Tor opened its SOCKS port at 127.0.0.1:9250; the node is restarting to use it",
               "restarting reason, from unavailable");
        expect(restartingReason("on", "127.0.0.1:9251") == "Tor moved to 127.0.0.1:9251; the node is restarting to use it",
               "restarting reason, from on");
    }
    {
        // No automatic restart on regtest, after the settings stopped asking for Tor, or once stopped.
        Seen s;
        event(s, "socks5h://127.0.0.1:9251", t);
        Run regtest = onAt("127.0.0.1:9250");
        regtest.canRestart = false;
        expect(show(decide(regtest, s, t + 9000)) == "waiting | Tor's SOCKS port moved to 127.0.0.1:9251; the node takes it at its next start |  @-1",
               "regtest: " + show(decide(regtest, s, t + 9000)));
        Run stopped = onAt("127.0.0.1:9250");
        stopped.userStarted = false;
        expect(decide(stopped, s, t + 9000).restartTo.empty(), "not user-started: no restart");
        Run unavailable = regtest;
        unavailable.mode = "unavailable";
        unavailable.startWhy = "tor_module is not loaded";
        expect(show(decide(unavailable, s, t + 9000)) == "unavailable | tor_module is not loaded |  @-1", "regtest unavailable stays");
        for (const char* m : {"proxy", "off"}) {
            Run r = onAt("");
            r.mode = m;
            expect(show(decide(r, s, t + 9000)) == std::string(m) + " |  |  @-1", std::string(m) + " ignores Tor's port");
        }
    }
}

static void sequences() {
    {
        // set_socks to a new address while running: one restart, ~3 s later.
        Sim sim;
        event(sim.seen, "socks5h://127.0.0.1:9250", sim.now);
        sim.tick(1000);
        event(sim.seen, "socks5h://127.0.0.1:9333", sim.now);
        const int64_t moved = sim.now;
        sim.tick(2900);
        expect(sim.restarts == 0, "not before the debounce");
        sim.tick(200);
        expect(sim.restarts == 1 && sim.run.socksUsed == "127.0.0.1:9333" && sim.run.lastAutoAt - moved == kDebounceMs,
               "one restart at 3 s, to the new port");
        sim.tick(120000);
        expect(sim.restarts == 1, "and no other");
    }
    {
        // A close and reopen at a new address within the debounce, as a reconnect may give: one.
        Sim sim;
        event(sim.seen, nullptr, sim.now);
        sim.tick(1500);
        event(sim.seen, "socks5h://127.0.0.1:9334", sim.now);
        sim.tick(60000);
        expect(sim.restarts == 1 && sim.run.socksUsed == "127.0.0.1:9334", "close and reopen: one restart");
    }
    {
        // A port flapping every 5 s for 3 minutes: at most one restart a minute, plus the first.
        Sim sim;
        for (int i = 0; i < 36; ++i) {
            event(sim.seen, i % 2 ? "socks5h://127.0.0.1:9250" : "socks5h://127.0.0.1:9400", sim.now);
            sim.tick(5000);
        }
        expect(sim.restarts >= 1 && sim.restarts <= 4, "flapping: " + std::to_string(sim.restarts) + " restarts in 3 minutes");
        // Flapping faster than the debounce: none until it settles.
        Sim fast;
        for (int i = 0; i < 60; ++i) {
            event(fast.seen, i % 2 ? "socks5h://127.0.0.1:9250" : "socks5h://127.0.0.1:9401", fast.now);
            fast.tick(1000);
        }
        expect(fast.restarts == 0, "flapping every second: " + std::to_string(fast.restarts) + " restarts");
    }
    {
        // Closed for good: the node waits and never restarts.
        Sim sim;
        event(sim.seen, nullptr, sim.now);
        sim.tick(300000);
        expect(sim.restarts == 0 && decide(sim.run, sim.seen, sim.now).privateSends == "waiting", "closed: waits, no restart");
    }
}

static void carry() {
    std::string why;
    const std::string info = R"({"result":{"transactions":[
        {"txid":"aa","wtxid":"ab","hex":"0200aa","time_added":1,"peers":[{"address":"x.onion:8333","sent":2,"received":3}]},
        {"txid":"bb","wtxid":"bc","hex":"0200bb","time_added":1,"peers":[]}]},"error":null})";
    std::vector<std::string> p = carry_over::pendingFrom(info, why);
    expect(p == std::vector<std::string>{"0200aa", "0200bb"} && why.empty(), "both pending transactions read");
    why.clear();
    expect(carry_over::pendingFrom(R"({"result":{"transactions":[]},"error":null})", why).empty() && why.empty(), "none pending");
    expect(carry_over::pendingFrom(R"({"result":null,"error":{"code":-9,"message":"Bitcoin is not connected"}})", why).empty()
               && why == "getprivatebroadcastinfo: Bitcoin is not connected", "a failed read says why: " + why);
    why.clear();
    expect(carry_over::pendingFrom("garbage", why).empty() && !why.empty(), "an unreadable reply says so");
    carry_over::merge(p, {"0200bb", "0200cc"});
    expect(p == std::vector<std::string>{"0200aa", "0200bb", "0200cc"}, "merge keeps one of each");
    expect(carry_over::resendParams("0200aa") == R"(["0200aa",0,21000000])", carry_over::resendParams("0200aa"));

    using carry_over::Outcome;
    struct Case { const char* reply; Outcome o; const char* why; };
    const Case cases[] = {
        {R"({"result":"aa","error":null})", Outcome::Resent, ""},
        {R"({"result":null,"error":{"code":-27,"message":"Transaction outputs already in utxo set"}})", Outcome::Ignored, "Transaction outputs already in utxo set"},
        {R"({"result":null,"error":{"code":-25,"message":"bad-txns-inputs-missingorspent"}})", Outcome::Ignored, "bad-txns-inputs-missingorspent"},
        {R"({"result":null,"error":{"code":-26,"message":"txn-mempool-conflict"}})", Outcome::Ignored, "txn-mempool-conflict"},
        {R"({"result":null,"error":{"code":-28,"message":"Loading block index..."}})", Outcome::Retry, "Loading block index..."},
        {R"({"result":null,"error":{"code":-37,"message":"Private broadcast queue is full"}})", Outcome::Failed, "Private broadcast queue is full"},
        {R"({"result":null,"error":{"code":-1,"message":"-privatebroadcast is enabled, but none of the Tor or I2P networks is reachable"}})", Outcome::Failed, "-privatebroadcast is enabled, but none of the Tor or I2P networks is reachable"},
        {"", Outcome::Failed, "unreadable reply from sendrawtransaction"},
    };
    for (const Case& c : cases) {
        std::string w;
        const Outcome o = carry_over::outcomeOf(c.reply, w);
        expect(o == c.o && w == c.why, std::string("outcome of ") + c.reply + " -> " + std::to_string(int(o)) + " '" + w + "'");
    }
}

int main() {
    decisions();
    sequences();
    carry();
    if (failures == 0) std::printf("tor follow: ok, %d checks\n", checks);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
