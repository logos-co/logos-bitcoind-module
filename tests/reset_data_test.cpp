// resetData()'s choice of directory and aside name, and which init errors read as corruption.
#include "../src/reset_data.h"

#include <cstdio>
#include <cstdlib>
#include <set>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

int main() {
    namespace fs = std::filesystem;
    using reset_data::chainDir;
    const fs::path own = "/u/module_data/bitcoind_module/abc/signet";
    // The module's own dataDir holds one network: it moves whole, trailing slash or not.
    check(chainDir(own, own, "signet") == own, "own dataDir");
    check(chainDir("/u/module_data/bitcoind_module/abc/signet/", own, "signet") == own, "own dataDir, trailing slash");
    check(chainDir("/u/module_data/bitcoind_module/abc/mainnet", "/u/module_data/bitcoind_module/abc/mainnet", "mainnet")
          == "/u/module_data/bitcoind_module/abc/mainnet", "own mainnet dataDir");
    // A dataDir set by hand may hold several networks: only Core's subdirectory for this one.
    check(chainDir("/data/btc", own, "signet") == "/data/btc/signet", "own-chosen dataDir, signet");
    check(chainDir("/data/btc", own, "testnet4") == "/data/btc/testnet4", "own-chosen dataDir, testnet4");
    check(chainDir("/data/btc", "/u/x/mainnet", "mainnet").empty(), "own-chosen dataDir, mainnet refused");

    std::set<std::string> taken;
    auto exists = [&](const fs::path& p) { return taken.count(p.string()) > 0; };
    const fs::path a = reset_data::asideFor(own, "20261124T101500Z", exists);
    check(a.string() == own.string() + ".reset-20261124T101500Z", "aside name");
    taken.insert(a.string());
    check(reset_data::asideFor(own, "20261124T101500Z", exists).string() == a.string() + "-2", "aside name taken");

    using reset_data::namesCorruption;
    for (const char* e : {"previous block header belongs to unexpected block 00ab; expected 00cd; "
                          "Cannot read last block filter header; index may be corrupted",
                          "Corrupted block database detected",
                          "Error loading block database",
                          "Please restart with -reindex or -reindex-chainstate to recover.",
                          "Please rebuild the index",
                          "Error initializing block database"})
        check(namesCorruption(e), e);
    for (const char* e : {"", "Cannot obtain a lock on directory /x. Bitcoin Core is probably already running.",
                          "Disk space is too low!", "Invalid -onion address or hostname: 'x'",
                          "a node is already running in this process"})
        check(!namesCorruption(e), e);

    if (failures == 0) std::printf("reset data: ok\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
