#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <logos_json.h>
#include <logos_module_context.h>
#include <logos_result.h>

#include "carry_over.h"
#include "private_sends.h"
#include "tor_follow.h"

/// A Bitcoin node (Bitcoin Core) running in-process through libbitcoind_c. One node at a
/// time; its lifetime is this module's lifetime. The wallet reaches it through rpc(), not a port.
class BitcoindModuleImpl : public LogosModuleContext {
public:
    BitcoindModuleImpl();
    ~BitcoindModuleImpl();

    /// Merge `config` into the stored settings for `network` (mainnet|testnet4|signet|regtest).
    /// Unknown keys and wrong types are refused. `privateSends` (default true but on regtest)
    /// sends the node's own transactions over Logos' Tor when `proxy` is empty.
    StdLogosResult configure(const std::string& network, const LogosMap& config);

    /// Stored settings for `network`, with defaults filled in; empty for an unknown network.
    LogosMap getConfig(const std::string& network);

    /// Default settings for `network`; empty for an unknown network.
    LogosMap defaultConfig(const std::string& network);

    /// Start the node for `network`. Returns once its thread is running. While it sends through
    /// Logos' Tor it follows Tor's SOCKS port, restarting on its own when the port moves.
    StdLogosResult start(const std::string& network);

    /// Write the cache (state "flushing"), then stop the node and wait for it to shut down.
    StdLogosResult stop();

    /// Write the node's UTXO cache to disk, keeping it in memory.
    StdLogosResult flush();

    /// @code{.json}
    /// { "state": "stopped|starting|running|flushing|stopping|failed", "network": string,
    ///   "version": string, "uptimeSecs": number, "lastError": string, "blocks": number,
    ///   "headers": number, "verificationProgress": number, "initialSync": bool,
    ///   "peersOut": number, "pruneHeight": number, "filterIndexHeight": number,
    ///   "tipAgeSecs": number, "sizeOnDisk": number, "dbcacheMiB": number,
    ///   "privateSends": "on|proxy|off|unavailable|waiting|restart|", "privateSendsReason": string,
    ///   "privateSendsCarried": number, "privateSendsCarryIgnored": number,
    ///   "privateSendsCarryError": string }
    /// @endcode
    /// Never waits on the node: a thread samples it every second and this serves the last
    /// sample. Numbers are -1 until known. `privateSends` is what the running node does with its
    /// own transactions ("" while none runs): `waiting` while Tor's port is closed, `restart`
    /// while an automatic restart to Tor's new port is pending or under way. The reason says why
    /// for unavailable, waiting and restart. The carry fields count the pending private
    /// broadcasts the last automatic restart sent again, or found mined or spent, and say why
    /// any could not be.
    LogosMap status();

    /// One call into Core's RPC table, in-process. `paramsJson` is a JSON array or object
    /// ("" for none). Returns Core's reply, {"result":...,"error":null} or
    /// {"result":null,"error":{"code":n,"message":s}}. Only bitcoin_wallet_core_module and
    /// bitcoin_node_module may call it, and only for the allowlisted methods; on regtest also
    /// generatetoaddress and generateblock.
    std::string rpc(const std::string& method, const std::string& paramsJson);

    /// The last `lines` lines of the node's debug.log.
    std::string logTail(int64_t lines);

logos_events:
    /// Emitted on every lifecycle transition and privateSends change, with the shape of status().
    void bitcoindStateChanged(const std::string& payloadJson);

protected:
    void onContextReady() override;
    LogosShutdown aboutToUnload() override;

private:
    bool knownNetwork(const std::string& network) const;
    nlohmann::json defaultsFor(const std::string& network) const;
    nlohmann::json settingsFor(const std::string& network);
    int64_t cacheFor(const std::string& network, int64_t configured) const;
    void noteTip(int64_t tipAgeSecs);
    void persist();
    bool admitted();
    void loadCallers();
    void pollLoop();
    void refresh(const char* forceState = nullptr);
    nlohmann::json sampleChain();
    void unloadLog(const std::string& line);
    StdLogosResult startLocked(const std::string& network);
    void stopLocked();
    std::string askTor(std::string& why);
    void askTorAsync();
    void watchTor();
    void followTor();
    void autoRestart();
    void carryOver();
    void followLog(const std::string& line);
    tor_follow::Decision decideNow();
    std::pair<std::string, std::string> privateSendsNow(const std::string& state);
    void addCarry(nlohmann::json& st);

    std::mutex m_mutex;
    std::string m_persistDir;                            // guarded by m_mutex
    nlohmann::json m_config = nlohmann::json::object();  // guarded by m_mutex
    std::vector<std::string> m_callerModules;            // guarded by m_mutex
    bool m_allowHost = false;                            // guarded by m_mutex
    std::string m_network;                               // guarded by m_mutex; the last start's
    std::string m_logFile;                               // guarded by m_mutex; the last start's
    int64_t m_dbcacheMiB = 0;                            // guarded by m_mutex; the last start's, 0 = Core's
    nlohmann::json m_state = nlohmann::json::object();   // guarded by m_mutex; bitcoind-state.json
    std::string m_privateSends;                          // guarded by m_mutex; the last start's mode
    std::string m_privateSendsReason;                    // guarded by m_mutex; why unavailable
    std::string m_torSocksUsed;                          // guarded by m_mutex; the last start's -onion
    bool m_userStarted = false;                          // guarded by m_mutex; a start() not yet stopped
    bool m_hadAuto = false;                              // guarded by m_mutex; an automatic restart ran...
    int64_t m_lastAutoMs = 0;                            // guarded by m_mutex; ...at this steady time
    std::string m_autoReason;                            // guarded by m_mutex; while m_autoRestarting
    // Pending private broadcasts an automatic restart carries over; in memory only.
    std::vector<std::string> m_carryHex;                 // guarded by m_mutex
    int64_t m_carried = 0;                               // guarded by m_mutex
    int64_t m_carryIgnored = 0;                          // guarded by m_mutex
    std::string m_carryError;                            // guarded by m_mutex
    // Tor's port as seen, shared with tor_module's callbacks, which can land after the module is gone.
    struct TorWatch {
        std::mutex mutex;
        tor_follow::Seen seen;
        bool armed = false;  // the subscription armed: events before it are lost, so ask once
    };
    std::shared_ptr<TorWatch> m_tor = std::make_shared<TorWatch>();
    std::atomic<int64_t> m_lastTorAskMs{0};              // steady clock; the last ask of tor_module
    std::atomic<bool> m_autoRestarting{false};           // an automatic restart stops and starts the node
    std::atomic<int> m_sendsInFlight{0};                 // rpc() sendrawtransaction calls under way
    std::atomic<bool> m_flushing{false};                 // stop() is writing the cache

    std::mutex m_lifecycleMutex;  // one start or stop at a time, an automatic restart's included

    // Core's RPC can wait on cs_main for seconds while blocks connect, so a thread samples the
    // node and status() serves the last sample.
    std::thread m_pollThread;
    std::atomic<bool> m_pollStop{false};
    std::mutex m_pollMutex;
    std::condition_variable m_pollWake;
    std::mutex m_sampleMutex;
    nlohmann::json m_chain = nlohmann::json::object();   // guarded by m_sampleMutex; last chain sample
    std::atomic<uint64_t> m_runGen{0};                   // bumped under m_sampleMutex at each start
    nlohmann::json m_sample = nlohmann::json::object();  // guarded by m_sampleMutex; what status() serves
    std::mutex m_emitMutex;                  // one merge-and-emit at a time, so events keep order
    std::string m_emittedState = "stopped";  // guarded by m_emitMutex
    std::string m_emittedSends;              // guarded by m_emitMutex
    std::atomic<int64_t> m_lastFlushMs{0};   // steady clock; the last sync-time flush

    // Emitting after the host's grace is a use-after-free in the host, so unload silences us.
    std::atomic<bool> m_unloading{false};
    std::thread m_stopThread;
};
