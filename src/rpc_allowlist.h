#pragma once

#include <algorithm>
#include <iterator>
#include <string>

// Which of Core's RPC methods rpc() serves: what the wallet core and node module need, and nothing
// that changes the node's settings or reads beyond the chain and its mempool.
namespace bitcoind_rpc {

inline const char* const kAllowed[] = {
    "getblockchaininfo", "getblockcount", "getblockhash", "getblockheader", "getblock",
    "getblockfilter", "getblockfrompeer", "getrawmempool", "getmempoolentry",
    "getrawtransaction", "sendrawtransaction", "estimatesmartfee", "getnetworkinfo",
    "getpeerinfo",
};

// Mining for the wallet's end-to-end tests, judged by the chain Core runs.
inline const char* const kRegtestOnly[] = {"generatetoaddress", "generateblock"};

enum class Verdict { Allowed, RegtestOnly, Refused };

inline Verdict classify(const std::string& method) {
    auto in = [&](const char* const* first, const char* const* last) {
        return std::find_if(first, last, [&](const char* m) { return method == m; }) != last;
    };
    if (in(std::begin(kAllowed), std::end(kAllowed))) return Verdict::Allowed;
    if (in(std::begin(kRegtestOnly), std::end(kRegtestOnly))) return Verdict::RegtestOnly;
    return Verdict::Refused;
}

}  // namespace bitcoind_rpc
