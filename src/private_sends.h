#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Private sends through Logos' Tor (tor_module): which mode a start runs in, and the SOCKS
// address tor_module's status() names. Header-only, so tests/private_sends_test.cpp checks it
// without a host.
namespace private_sends {

// tor_module's `socks5h://IP:PORT` as Core's -onion takes it, IP:PORT; "" for anything else.
inline std::string onionProxyFrom(const std::string& socksUrl) {
    const std::string scheme = "socks5h://";
    if (socksUrl.compare(0, scheme.size(), scheme) != 0) return "";
    std::string hp = socksUrl.substr(scheme.size());
    if (!hp.empty() && hp.back() == '/') hp.pop_back();
    const size_t colon = hp.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == hp.size() || hp.size() - colon > 6) return "";
    if (hp.find_first_not_of("0123456789abcdefABCDEF.:[]") != std::string::npos) return "";
    if (hp.find_first_not_of("0123456789", colon + 1) != std::string::npos) return "";
    const long port = std::stol(hp.substr(colon + 1));
    return port > 0 && port <= 65535 ? hp : "";
}

// What a start with these settings does with the node's own transactions: "proxy" (private
// broadcast through the user's proxy, onion peers only), "on" (private broadcast through Logos'
// Tor at `torSocks`), "off", or "unavailable" (asked for, but there is no Tor SOCKS port to use).
inline std::string modeFor(const nlohmann::json& s, const std::string& torSocks) {
    if (!s.value("proxy", std::string()).empty()) return "proxy";
    if (!s.value("privateSends", false)) return "off";
    return torSocks.empty() ? "unavailable" : "on";
}

// Logos' Tor as one status() reply describes it.
struct TorSocks {
    bool answered = false;  // tor_module answered ok; false when it is starting or failed
    std::string socks;      // IP:PORT; "" when the port is off or unusable
    std::string why;        // why there is no socks
};

inline TorSocks torSocksFrom(const std::string& statusReply) {
    TorSocks t;
    const nlohmann::json v = nlohmann::json::parse(statusReply, nullptr, false);
    if (!v.is_object()) {
        t.why = "tor_module answered something other than its status";
        return t;
    }
    if (v.value("ok", false) != true) {
        const nlohmann::json e = v.contains("error") ? v["error"] : nlohmann::json();
        t.why = "tor_module: " + (e.is_string() ? e.get<std::string>() : std::string("no status"));
        return t;
    }
    t.answered = true;
    if (!v.contains("socks") || !v["socks"].is_string()) {
        t.why = "Tor's SOCKS port is off";
        return t;
    }
    t.socks = onionProxyFrom(v["socks"].get<std::string>());
    if (t.socks.empty()) t.why = "Tor's SOCKS port is not one Core can use: " + v["socks"].get<std::string>();
    return t;
}

}  // namespace private_sends
