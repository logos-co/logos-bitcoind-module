// Private sends: tor_module's status read into a SOCKS address, the mode each setting gives, and
// the flags each mode adds. tests/private_broadcast_test.py --mode tor runs the "on" flags in Core.
#include "../src/bitcoind_argv.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using json = nlohmann::json;

static int failures = 0;

static void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
        ++failures;
    }
}

static void socks(const std::string& reply, bool answered, const std::string& want, const std::string& why) {
    const private_sends::TorSocks t = private_sends::torSocksFrom(reply);
    expect(t.answered == answered && t.socks == want && t.why == why,
           reply + " -> " + std::to_string(t.answered) + " '" + t.socks + "' '" + t.why + "'");
}

static bool has(const json& argv, const std::string& prefix) {
    return std::any_of(argv.begin(), argv.end(), [&](const json& a) { return a.get<std::string>().rfind(prefix, 0) == 0; });
}

int main() {
    socks(R"({"ok":true,"state":"ready","socks":"socks5h://127.0.0.1:9250"})", true, "127.0.0.1:9250", "");
    socks(R"({"ok":true,"state":"connecting","socks":"socks5h://127.0.0.1:61234"})", true, "127.0.0.1:61234", "");
    socks(R"({"ok":true,"state":"ready","socks":"socks5h://[::1]:9250"})", true, "[::1]:9250", "");
    socks(R"({"ok":true,"state":"ready","socks":null})", true, "", "Tor's SOCKS port is off");
    socks(R"({"ok":true,"state":"ready"})", true, "", "Tor's SOCKS port is off");
    socks(R"({"ok":true,"socks":"socks5://127.0.0.1:9250"})", true, "",
          "Tor's SOCKS port is not one Core can use: socks5://127.0.0.1:9250");
    socks(R"({"ok":true,"socks":"socks5h://127.0.0.1:0"})", true, "",
          "Tor's SOCKS port is not one Core can use: socks5h://127.0.0.1:0");
    socks(R"({"ok":true,"socks":"socks5h://127.0.0.1:9250 -proxy=x"})", true, "",
          "Tor's SOCKS port is not one Core can use: socks5h://127.0.0.1:9250 -proxy=x");
    socks(R"({"ok":false,"error":"starting"})", false, "", "tor_module: starting");
    socks("not json", false, "", "tor_module answered something other than its status");

    const json base = {{"dataDir", "/d"}, {"pruneMiB", 550}, {"dbcacheMiB", 0}, {"maxMempoolMiB", 300},
                       {"proxy", ""}, {"logCategories", json::array()}, {"exposeRpc", false},
                       {"addNodes", json::array()}, {"privateSends", true}};
    json off = base, proxied = base, old = base;
    off["privateSends"] = false;
    proxied["proxy"] = "127.0.0.1:9050";
    old.erase("privateSends");  // a caller from before the setting: off
    expect(private_sends::modeFor(base, "127.0.0.1:9250") == "on", "on");
    expect(private_sends::modeFor(base, "") == "unavailable", "unavailable");
    expect(private_sends::modeFor(off, "127.0.0.1:9250") == "off", "off");
    expect(private_sends::modeFor(old, "127.0.0.1:9250") == "off", "absent is off");
    expect(private_sends::modeFor(proxied, "127.0.0.1:9250") == "proxy", "proxy");

    const json on = bitcoind_argv::argvFor("signet", base, 0, "127.0.0.1:9250");
    expect(has(on, "-onion=127.0.0.1:9250") && has(on, "-privatebroadcast") && !has(on, "-proxy") && !has(on, "-onlynet"),
           "on: " + on.dump());
    for (const json& a : {bitcoind_argv::argvFor("signet", base, 0, ""), bitcoind_argv::argvFor("signet", off, 0, "127.0.0.1:9250")})
        expect(!has(a, "-onion") && !has(a, "-privatebroadcast") && !has(a, "-proxy") && !has(a, "-onlynet"), "plain: " + a.dump());
    const json p = bitcoind_argv::argvFor("signet", proxied, 0, "127.0.0.1:9250");
    expect(has(p, "-proxy=127.0.0.1:9050") && has(p, "-onlynet=onion") && has(p, "-privatebroadcast") && !has(p, "-onion"),
           "proxy: " + p.dump());

    if (failures == 0) std::printf("private sends: ok\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
