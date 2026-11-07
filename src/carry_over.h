#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Carrying the node's pending private broadcasts across its own automatic restart: Core keeps them
// in memory only, so they are read before the stop and sent again after the start. Header-only,
// so tests/tor_follow_test.cpp checks it without a node.
namespace carry_over {

// The raw transactions in a getprivatebroadcastinfo reply; empty with `why` set when unreadable.
inline std::vector<std::string> pendingFrom(const std::string& reply, std::string& why) {
    std::vector<std::string> out;
    const nlohmann::json r = nlohmann::json::parse(reply, nullptr, false);
    if (!r.is_object() || !r.contains("error") || !r.contains("result")) {
        why = "unreadable reply from getprivatebroadcastinfo";
        return out;
    }
    if (!r["error"].is_null()) {
        why = "getprivatebroadcastinfo: " + r["error"].value("message", r["error"].dump());
        return out;
    }
    const nlohmann::json& txs = r["result"].is_object() ? r["result"].value("transactions", nlohmann::json()) : nlohmann::json();
    if (!txs.is_array()) {
        why = "getprivatebroadcastinfo has no transactions";
        return out;
    }
    for (const nlohmann::json& t : txs)
        if (t.is_object() && t.contains("hex") && t["hex"].is_string() && !t["hex"].get<std::string>().empty())
            out.push_back(t["hex"].get<std::string>());
    return out;
}

// Adds what is not there yet, keeping the order.
inline void merge(std::vector<std::string>& into, const std::vector<std::string>& more) {
    for (const std::string& h : more)
        if (std::find(into.begin(), into.end(), h) == into.end()) into.push_back(h);
}

// sendrawtransaction's parameters for a carried transaction: no fee or burn cap, since Core
// already took it once with whatever caps its caller gave.
inline std::string resendParams(const std::string& hex) {
    return nlohmann::json::array({hex, 0, 21000000}).dump();
}

enum class Outcome {
    Resent,   // queued for private broadcast again
    Ignored,  // mined already, or its inputs are spent or conflict: nothing left to send
    Retry,    // the node is still warming up
    Failed,   // could not be queued: queue full, no Tor, node gone
};

// What a sendrawtransaction reply means for one carried transaction; `why` for the non-Resent ones.
inline Outcome outcomeOf(const std::string& reply, std::string& why) {
    const nlohmann::json r = nlohmann::json::parse(reply, nullptr, false);
    if (!r.is_object() || !r.contains("error")) {
        why = "unreadable reply from sendrawtransaction";
        return Outcome::Failed;
    }
    if (r["error"].is_null()) return Outcome::Resent;
    const int code = r["error"].value("code", 0);
    why = r["error"].value("message", std::string("error ") + std::to_string(code));
    switch (code) {
        case -28: return Outcome::Retry;    // RPC_IN_WARMUP
        case -27:                           // RPC_VERIFY_ALREADY_IN_UTXO_SET: mined
        case -26:                           // RPC_VERIFY_REJECTED: conflicts, or the mempool refuses it
        case -25: return Outcome::Ignored;  // RPC_VERIFY_ERROR: inputs missing or spent
        default:  return Outcome::Failed;   // -37 queue full, -1 no Tor, -9 node gone, ...
    }
}

}  // namespace carry_over
