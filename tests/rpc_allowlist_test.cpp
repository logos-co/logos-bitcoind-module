// rpc()'s allowlist: what the wallet core needs is served, mining on regtest only, the rest refused.
#include "../src/rpc_allowlist.h"

#include <cstdio>
#include <cstdlib>

using bitcoind_rpc::Verdict;

static int failures = 0;

static void expect(const char* method, Verdict want) {
    if (bitcoind_rpc::classify(method) != want) {
        std::fprintf(stderr, "FAIL %s\n", method);
        ++failures;
    }
}

int main() {
    // The wallet core's filter restore, pruned blocks, pending payments and the Emitter.
    for (const char* m : {"getrawtransaction", "getblockcount", "getblockfilter", "getblockfrompeer",
                          "getrawmempool", "getblockheader", "getblock", "getblockhash",
                          "getblockchaininfo", "sendrawtransaction", "estimatesmartfee",
                          "getmempoolentry", "getnetworkinfo", "getpeerinfo"})
        expect(m, Verdict::Allowed);
    expect("generatetoaddress", Verdict::RegtestOnly);
    expect("generateblock", Verdict::RegtestOnly);
    // Nothing that changes the node, its settings or its peers, nor reads its disk or wallet.
    for (const char* m : {"stop", "setnetworkactive", "addnode", "disconnectnode", "setban",
                          "pruneblockchain", "invalidateblock", "reconsiderblock", "dumptxoutset",
                          "loadwallet", "importdescriptors", "generatetodescriptor", "submitblock",
                          "logging", "help", "", "GETBLOCK"})
        expect(m, Verdict::Refused);
    if (failures == 0) std::printf("rpc allowlist: ok\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
