#include "bitcoind_module_impl.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include <logos_caller.h>

#include "bitcoind_c.h"
#include "rpc_allowlist.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const char* const kDefaultCallers[] = {"bitcoin_wallet_core_module", "bitcoin_node_module"};

// During initial sync the cache is written this often, so a stop has little left to flush.
constexpr auto kSyncFlushInterval = std::chrono::minutes(3);

// Once a run has reached the tip, later starts use this cache (MiB) unless dbcacheMiB is lower.
constexpr int64_t kAfterSyncCacheMiB = 300;
// A tip older than this means the node is catching up again, so it gets the sync-time cache.
constexpr int64_t kStaleTipSecs = 7 * 24 * 3600;

std::string takeString(const char* s) {
    std::string out = s ? s : "";
    BITCOIND_free(s);
    return out;
}

StdLogosResult fail(const std::string& why) { return {false, {}, why}; }

std::string rpcRefusal(int code, const std::string& why) {
    return json{{"result", nullptr}, {"error", {{"code", code}, {"message", why}}}}.dump();
}

void appendLine(const std::string& path, const std::string& line) {
    std::ofstream f(path, std::ios::app);
    if (f) f << line << '\n';
}

const char* stateName(int state) {
    switch (state) {
        case BITCOIND_STOPPED:  return "stopped";
        case BITCOIND_STARTING: return "starting";
        case BITCOIND_RUNNING:  return "running";
        case BITCOIND_STOPPING: return "stopping";
        default:                return "failed";
    }
}

// Core keeps each network but mainnet in a subdirectory of its data directory.
std::string netSubdir(const std::string& network) {
    return network == "mainnet" ? "" : network;
}

// The result of a Core reply, or null when it carried an error.
json rpcResult(const char* method, const char* params) {
    const json r = json::parse(takeString(BITCOIND_rpc(method, params)), nullptr, false);
    if (!r.is_object() || !r.contains("error") || !r["error"].is_null() || !r.contains("result")) return nullptr;
    return r["result"];
}

json chainDefaults() {
    return json{{"blocks", -1}, {"headers", -1}, {"verificationProgress", -1}, {"initialSync", false},
                {"peersOut", -1}, {"pruneHeight", -1}, {"filterIndexHeight", -1}, {"tipAgeSecs", -1},
                {"sizeOnDisk", -1}};
}

}  // namespace

// Started in the body: the thread uses members declared after it.
BitcoindModuleImpl::BitcoindModuleImpl() { m_pollThread = std::thread([this] { pollLoop(); }); }

BitcoindModuleImpl::~BitcoindModuleImpl() {
    { std::lock_guard<std::mutex> lock(m_pollMutex); m_pollStop = true; }
    m_pollWake.notify_all();
    if (m_pollThread.joinable()) m_pollThread.join();
    if (m_stopThread.joinable()) m_stopThread.join();
    BITCOIND_stop();
}

void BitcoindModuleImpl::onContextReady() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_persistDir = instancePersistencePath();
    if (m_persistDir.empty()) return;
    std::error_code ec;
    fs::create_directories(m_persistDir, ec);
    std::ifstream f(fs::path(m_persistDir) / "bitcoind.json");
    if (f) {
        m_config = json::parse(f, nullptr, false);
        if (!m_config.is_object()) m_config = json::object();
    }
    std::ifstream sf(fs::path(m_persistDir) / "bitcoind-state.json");
    if (sf) {
        m_state = json::parse(sf, nullptr, false);
        if (!m_state.is_object()) m_state = json::object();
    }
    loadCallers();
}

// Absent file: the defaults. Unreadable file: nobody, so a typo never opens the door.
void BitcoindModuleImpl::loadCallers() {
    m_callerModules.assign(std::begin(kDefaultCallers), std::end(kDefaultCallers));
    m_allowHost = false;
    std::ifstream f(fs::path(m_persistDir) / "callers.json");
    if (!f) return;
    m_callerModules.clear();
    const json c = json::parse(f, nullptr, false);
    if (!c.is_object()) return;
    std::vector<std::string> modules;
    bool allowHost = false;
    for (auto& [k, v] : c.items()) {
        if (k == "modules" && v.is_array()) {
            for (const json& m : v) {
                if (!m.is_string()) return;
                modules.push_back(m.get<std::string>());
            }
        } else if (k == "allowHost" && v.is_boolean()) {
            allowHost = v.get<bool>();
        } else {
            return;
        }
    }
    m_callerModules = std::move(modules);
    m_allowHost = allowHost;
}

bool BitcoindModuleImpl::knownNetwork(const std::string& network) const {
    return network == "mainnet" || network == "testnet4" || network == "signet" || network == "regtest";
}

LogosMap BitcoindModuleImpl::defaultConfig(const std::string& network) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return defaultsFor(network);
}

json BitcoindModuleImpl::defaultsFor(const std::string& network) const {
    if (!knownNetwork(network)) return json::object();
    return json{
        {"dataDir", m_persistDir.empty() ? "" : (fs::path(m_persistDir) / network).string()},
        // MiB of recent blocks kept; 0 keeps them all. Core's minimum is 550.
        {"pruneMiB", network == "mainnet" ? 2048 : 550},
        {"dbcacheMiB", 0},  // 0: Core's default for this machine's memory
        {"maxMempoolMiB", 300},
        {"proxy", ""},
        {"logCategories", json::array()},
        {"exposeRpc", false},
        // Peers to keep a connection to, host:port (Core's -addnode), such as one's own node.
        {"addNodes", json::array()},
    };
}

namespace {
bool sameType(const json& d, const json& v) {
    if (d.is_boolean()) return v.is_boolean();
    // nlohmann types a parsed 550 as unsigned and a negative as signed.
    if (d.is_number()) return v.is_number_integer();
    if (d.is_array()) {
        if (!v.is_array()) return false;
        return std::all_of(v.begin(), v.end(), [](const json& e) { return e.is_string(); });
    }
    return v.is_string();
}
}  // namespace

// A stored value of the wrong type (a hand edit) falls back to the default.
json BitcoindModuleImpl::settingsFor(const std::string& network) {
    json s = defaultsFor(network);
    if (m_config.contains(network) && m_config[network].is_object())
        for (auto& [k, v] : m_config[network].items())
            if (s.contains(k) && sameType(s[k], v)) s[k] = v;
    return s;
}

LogosMap BitcoindModuleImpl::getConfig(const std::string& network) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return settingsFor(network);
}

StdLogosResult BitcoindModuleImpl::configure(const std::string& network, const LogosMap& config) {
    if (!config.is_object()) return fail("config must be an object");
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!knownNetwork(network)) return fail("unknown network: " + network);
    const json defaults = defaultsFor(network);
    for (auto& [k, v] : config.items()) {
        if (!defaults.contains(k)) return fail("unknown setting: " + k);
        if (!sameType(defaults[k], v)) return fail("wrong type for " + k);
        if (k == "dataDir" && v.get<std::string>().empty()) return fail("dataDir must not be empty");
        if (k == "pruneMiB") {
            const int64_t p = v.get<int64_t>();
            if (p != 0 && p < 550) return fail("pruneMiB must be 0 (keep every block) or at least 550");
        }
        if ((k == "dbcacheMiB" || k == "maxMempoolMiB") && v.get<int64_t>() < 0) return fail(k + " must not be negative");
        if (k == "maxMempoolMiB" && v.get<int64_t>() < 5) return fail("maxMempoolMiB must be at least 5");
        if (k == "logCategories")
            for (const json& c : v)
                if (c.get<std::string>().empty() || c.get<std::string>().find_first_of(",= ") != std::string::npos)
                    return fail("logCategories holds names like \"net\" or \"mempool\"");
        if (k == "proxy" && v.get<std::string>().find_first_of(" \"") != std::string::npos)
            return fail("proxy must be host:port");
        if (k == "addNodes") {
            if (v.size() > 8) return fail("addNodes holds at most 8 peers");
            for (const json& n : v)
                if (n.get<std::string>().empty() || n.get<std::string>().find_first_of(" \",=") != std::string::npos)
                    return fail("addNodes holds host:port entries");
        }
    }
    json& stored = m_config[network];
    if (!stored.is_object()) stored = json::object();
    for (auto& [k, v] : config.items()) stored[k] = v;
    persist();
    return {true, settingsFor(network)};
}

// The sync-time cache until a run has reached the tip, then kAfterSyncCacheMiB unless lower.
int64_t BitcoindModuleImpl::cacheFor(const std::string& network, int64_t configured) const {
    const json n = m_state.contains(network) && m_state[network].is_object() ? m_state[network] : json::object();
    const int64_t tipTime = n.value("tipTime", int64_t{0});
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (!n.value("reachedTip", false) || now - tipTime > kStaleTipSecs) return configured;
    return configured > 0 ? std::min(configured, kAfterSyncCacheMiB) : kAfterSyncCacheMiB;
}

// Called by the poller while the node is out of initial sync; writes only on news.
void BitcoindModuleImpl::noteTip(int64_t tipAgeSecs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_network.empty() || m_persistDir.empty()) return;
    const int64_t tipTime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - tipAgeSecs;
    json& n = m_state[m_network];
    if (!n.is_object()) n = json::object();
    if (n.value("reachedTip", false) && tipTime - n.value("tipTime", int64_t{0}) < 3600) return;
    n["reachedTip"] = true;
    n["tipTime"] = tipTime;
    const fs::path path = fs::path(m_persistDir) / "bitcoind-state.json";
    const fs::path tmp = path.string() + ".tmp";
    { std::ofstream f(tmp); f << m_state.dump(2); }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

void BitcoindModuleImpl::persist() {
    if (m_persistDir.empty()) return;
    const fs::path path = fs::path(m_persistDir) / "bitcoind.json";
    const fs::path tmp = path.string() + ".tmp";
    { std::ofstream f(tmp); f << m_config.dump(2); }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

// The plan's defaults: pruned with filters, no wallet, no listener, Tor only with a proxy.
json BitcoindModuleImpl::argvFor(const std::string& network, const json& s, int64_t dbcacheMiB) const {
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

StdLogosResult BitcoindModuleImpl::start(const std::string& network) {
    if (m_unloading) return fail("unloading");
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    json argv;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!knownNetwork(network)) return fail("unknown network: " + network);
        if (m_persistDir.empty()) return fail("no persistence path yet");
        const json s = settingsFor(network);
        const fs::path dataDir = s["dataDir"].get<std::string>();
        std::error_code ec;
        fs::create_directories(dataDir, ec);
        if (ec) return fail("dataDir: " + ec.message());
        // The chain is nobody else's business, and debug.log can name our peers.
        fs::permissions(dataDir, fs::perms::owner_all, fs::perm_options::replace, ec);
        const int64_t cache = cacheFor(network, s["dbcacheMiB"].get<int64_t>());
        argv = argvFor(network, s, cache);
        m_network = network;
        m_dbcacheMiB = cache;
        m_logFile = (dataDir / netSubdir(network) / "debug.log").string();
    }
    {
        std::lock_guard<std::mutex> lock(m_sampleMutex);
        ++m_runGen;
        m_chain = chainDefaults();
    }
    if (BITCOIND_start(argv.dump().c_str()) != 0) {
        const std::string why = takeString(BITCOIND_last_error());
        refresh();
        return fail(why);
    }
    m_lastFlushMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    refresh();
    return {true, status()};
}

// Writes the cache while the node still runs, so the flush shows as its own state.
StdLogosResult BitcoindModuleImpl::stop() {
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    if (BITCOIND_state() == BITCOIND_RUNNING) {
        m_flushing = true;
        refresh("flushing");
        BITCOIND_flush();
        m_flushing = false;
    }
    const int st = BITCOIND_state();
    if (st == BITCOIND_RUNNING || st == BITCOIND_STARTING) refresh("stopping");
    BITCOIND_stop();
    refresh();
    return {true, status()};
}

StdLogosResult BitcoindModuleImpl::flush() {
    if (BITCOIND_state() != BITCOIND_RUNNING) return fail("node is not running");
    if (BITCOIND_flush() != 0) return fail(takeString(BITCOIND_last_error()));
    return {true, status()};
}

// Async: at the tip Core's final flush takes well under a second, but during initial sync
// it can take longer than the host's 3 s grace. The thread is joined in the destructor.
LogosShutdown BitcoindModuleImpl::aboutToUnload() {
    m_unloading = true;
    if (m_stopThread.joinable()) return LogosShutdown::Asynchronous;
    const int st = BITCOIND_state();
    if (st == BITCOIND_STOPPED || st == BITCOIND_FAILED) {
        unloadLog("aboutToUnload: node not running");
        return LogosShutdown::Synchronous;
    }
    unloadLog("aboutToUnload: stopping node");
    m_stopThread = std::thread([this] {
        const auto t0 = std::chrono::steady_clock::now();
        BITCOIND_stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        unloadLog("aboutToUnload: node stopped in " + std::to_string(ms) + " ms; "
                  + takeString(BITCOIND_status_json()));
        unloadFinished();
    });
    return LogosShutdown::Asynchronous;
}

// A file, not stderr: the host closes our stdio before it unloads us.
void BitcoindModuleImpl::unloadLog(const std::string& line) {
    std::string dir;
    { std::lock_guard<std::mutex> lock(m_mutex); dir = m_persistDir; }
    if (!dir.empty()) appendLine((fs::path(dir) / "unload.log").string(), line);
}

void BitcoindModuleImpl::pollLoop() {
    std::unique_lock<std::mutex> lock(m_pollMutex);
    while (!m_pollStop) {
        lock.unlock();
        // Outside the emit lock: Core's RPC can wait on cs_main for seconds.
        const uint64_t gen = m_runGen;
        json chain = chainDefaults();
        if (BITCOIND_state() == BITCOIND_RUNNING) chain = sampleChain();
        bool current = false;
        {
            // A sample that straddled a restart belongs to the last run.
            std::lock_guard<std::mutex> l(m_sampleMutex);
            current = gen == m_runGen;
            if (current) m_chain = chain;
        }
        if (current && chain["initialSync"] == false && chain["blocks"].get<int64_t>() > 0
            && chain["tipAgeSecs"].get<int64_t>() >= 0)
            noteTip(chain["tipAgeSecs"].get<int64_t>());
        refresh();
        // Keep the cache small during initial sync, so a stop or unload has little to write.
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (chain["initialSync"] == true && !m_unloading && BITCOIND_state() == BITCOIND_RUNNING
            && nowMs - m_lastFlushMs >= std::chrono::duration_cast<std::chrono::milliseconds>(kSyncFlushInterval).count()) {
            m_lastFlushMs = nowMs;
            BITCOIND_flush();
        }
        lock.lock();
        m_pollWake.wait_for(lock, std::chrono::seconds(1), [this] { return m_pollStop.load(); });
    }
}

// Chain fields from Core's own RPC. Called only while the node runs; may wait on cs_main.
json BitcoindModuleImpl::sampleChain() {
    json c = chainDefaults();
    const json info = rpcResult("getblockchaininfo", nullptr);
    if (info.is_object()) {
        c["blocks"] = info.value("blocks", -1);
        c["headers"] = info.value("headers", -1);
        c["verificationProgress"] = info.value("verificationprogress", -1.0);
        c["initialSync"] = info.value("initialblockdownload", false);
        c["pruneHeight"] = info.value("pruned", false) ? info.value("pruneheight", -1) : 0;
        c["sizeOnDisk"] = info.value("size_on_disk", int64_t{-1});
        const int64_t tipTime = info.value("time", int64_t{0});
        if (tipTime > 0) {
            const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            // A block may carry a slightly future timestamp; never report a negative age.
            c["tipAgeSecs"] = now > tipTime ? now - tipTime : 0;
        }
    }
    const json net = rpcResult("getnetworkinfo", nullptr);
    if (net.is_object()) c["peersOut"] = net.value("connections_out", -1);
    const json idx = rpcResult("getindexinfo", "[\"basic block filter index\"]");
    if (idx.is_object() && idx.contains("basic block filter index") && idx["basic block filter index"].is_object())
        c["filterIndexHeight"] = idx["basic block filter index"].value("best_block_height", -1);
    return c;
}

// Merges the library's status with the last chain sample; emits when the state moved.
// Never waits on the node. `forceState` announces a stop before it blocks.
void BitcoindModuleImpl::refresh(const char* forceState) {
    std::lock_guard<std::mutex> emitLock(m_emitMutex);
    json st = json::parse(takeString(BITCOIND_status_json()), nullptr, false);
    if (!st.is_object() || !st.contains("state") || !st["state"].is_string())
        st = json{{"state", stateName(BITCOIND_state())}};
    json s;
    { std::lock_guard<std::mutex> lock(m_sampleMutex); s = m_chain; }
    if (st["state"] != "running" || forceState || !s.is_object()) s = chainDefaults();
    std::string network;
    { std::lock_guard<std::mutex> lock(m_mutex); network = m_network; }
    s["state"] = forceState ? std::string(forceState)
               : (m_flushing && st["state"] == "running") ? std::string("flushing") : st["state"].get<std::string>();
    s["network"] = network;
    s["version"] = st.value("version", "");
    s["uptimeSecs"] = st.value("uptimeSecs", 0);
    s["lastError"] = st.value("lastError", "");
    { std::lock_guard<std::mutex> lock(m_mutex); s["dbcacheMiB"] = m_dbcacheMiB; }
    if (!forceState) {
        std::lock_guard<std::mutex> lock(m_sampleMutex);
        m_sample = s;
    }
    const std::string state = s["state"].get<std::string>();
    if (state == m_emittedState || m_unloading) return;
    m_emittedState = state;
    bitcoindStateChanged(s.dump());
}

LogosMap BitcoindModuleImpl::status() {
    json st;
    { std::lock_guard<std::mutex> lock(m_sampleMutex); st = m_sample; }
    if (!st.is_object() || st.empty()) {
        st = chainDefaults();
        st["network"] = "";
        st["version"] = "";
        st["uptimeSecs"] = 0;
        st["lastError"] = "";
    }
    // An atomic read: the state is never a second stale, even when the rest is.
    const int state = BITCOIND_state();
    st["state"] = m_flushing && state == BITCOIND_RUNNING ? "flushing" : stateName(state);
    // Not a range-for over chainDefaults().items(): C++17 destroys that temporary before the
    // loop runs, and Windows' heap crashed the host on it while the node was stopped.
    if (state != BITCOIND_RUNNING) st.update(chainDefaults());
    return st;
}

bool BitcoindModuleImpl::admitted() {
    const logos::LogosCaller caller = logos::currentCaller();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (caller.isHost()) return m_allowHost;
    if (!caller.isModule() || caller.name.empty()) return false;
    return std::find(m_callerModules.begin(), m_callerModules.end(), caller.name) != m_callerModules.end();
}

std::string BitcoindModuleImpl::rpc(const std::string& method, const std::string& paramsJson) {
    // Codes from JSON-RPC 2.0: an invalid request, and a method that is not there for us.
    if (!admitted()) return rpcRefusal(-32600, "not authorized");
    if (m_unloading) return rpcRefusal(-9, "unloading");
    const bitcoind_rpc::Verdict verdict = bitcoind_rpc::classify(method);
    if (verdict != bitcoind_rpc::Verdict::Allowed) {
        if (verdict == bitcoind_rpc::Verdict::Refused)
            return rpcRefusal(-32601, "method not allowed: " + method);
        // The chain Core itself runs, not the network the module was last asked for.
        const json st = json::parse(takeString(BITCOIND_status_json()), nullptr, false);
        if (!st.is_object() || st.value("network", "") != "regtest")
            return rpcRefusal(-32601, method + " is allowed on regtest only");
    }
    return takeString(BITCOIND_rpc(method.c_str(), paramsJson.empty() ? nullptr : paramsJson.c_str()));
}

// Reads backwards from the end, so a long-running node's log costs only the tail.
std::string BitcoindModuleImpl::logTail(int64_t lines) {
    std::string path;
    { std::lock_guard<std::mutex> lock(m_mutex); path = m_logFile; }
    if (path.empty() || lines <= 0) return "";
    lines = std::min<int64_t>(lines, 10000);
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    std::string tail;
    std::streamoff pos = size;
    int64_t newlines = 0;
    while (pos > 0) {
        const std::streamoff chunk = std::min<std::streamoff>(pos, 64 * 1024);
        pos -= chunk;
        std::string buf(static_cast<size_t>(chunk), '\0');
        f.seekg(pos);
        f.read(buf.data(), chunk);
        tail.insert(0, buf);
        // The last line's own newline does not start another line.
        newlines = std::count(tail.begin(), tail.end(), '\n') - (!tail.empty() && tail.back() == '\n');
        if (newlines >= lines) break;
    }
    size_t start = 0;
    for (int64_t skip = newlines - lines; skip >= 0 && start < tail.size(); --skip)
        start = tail.find('\n', start) + 1;
    std::string out = tail.substr(start);
    if (!out.empty() && out.back() != '\n') out += '\n';
    return out;
}
