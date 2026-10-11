#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "rpc_allowlist.h"

// The flags the module starts Core with, from one network's settings. Header-only, so a test
// starts Core with exactly these (tests/print_argv.cpp).
namespace bitcoind_argv {

// The plan's defaults: pruned with filters, no wallet, no listener, Tor only with a proxy.
inline nlohmann::json argvFor(const std::string& network, const nlohmann::json& s, int64_t dbcacheMiB) {
    using json = nlohmann::json;
    json argv = json::array();
    if (network != "mainnet") argv.push_back("-" + network);
    argv.push_back("-datadir=" + s["dataDir"].get<std::string>());
    // The module's settings are the whole configuration; a bitcoin.conf in dataDir is ignored.
    argv.push_back("-noconf");
    if (s["pruneMiB"].get<int64_t>() > 0) argv.push_back("-prune=" + std::to_string(s["pruneMiB"].get<int64_t>()));
    argv.push_back("-blockfilterindex=1");
    argv.push_back("-disablewallet");
    argv.push_back("-persistmempool=0");
    argv.push_back("-listen=0");
    if (dbcacheMiB > 0) argv.push_back("-dbcache=" + std::to_string(dbcacheMiB));
    for (const json& n : s["addNodes"]) argv.push_back("-addnode=" + n.get<std::string>());
    argv.push_back("-maxmempool=" + std::to_string(s["maxMempoolMiB"].get<int64_t>()));
    for (const json& c : s["logCategories"]) argv.push_back("-debug=" + c.get<std::string>());
    const std::string proxy = s["proxy"].get<std::string>();
    if (!proxy.empty()) {
        argv.push_back("-proxy=" + proxy);
        argv.push_back("-onlynet=onion");
        // Core refuses private broadcast without Tor or I2P, so it rides on the proxy.
        argv.push_back("-privatebroadcast");
    }
    if (s["exposeRpc"].get<bool>()) {
        // Loopback only, a cookie Core writes afresh at each start, and the same methods as rpc().
        std::string methods;
        for (const char* m : bitcoind_rpc::kAllowed) methods += (methods.empty() ? "" : ",") + std::string(m);
        argv.push_back("-server=1");
        argv.push_back("-rpcbind=127.0.0.1");
        argv.push_back("-rpcallowip=127.0.0.1");
        argv.push_back("-rpccookieperms=owner");
        argv.push_back("-rpcwhitelist=__cookie__:" + methods);
        argv.push_back("-rpcwhitelistdefault=1");
    }
    return argv;
}

}  // namespace bitcoind_argv
