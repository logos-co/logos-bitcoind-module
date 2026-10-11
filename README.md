# logos-bitcoind-module

`bitcoind_module` runs a Bitcoin full node, [Bitcoin Core](https://github.com/bitcoin/bitcoin)
31.1, **in-process**, through `libbitcoind_c` from `logos-bitcoin-core-nix`. No executable is
bundled and no subprocess is spawned: the node lives and dies with this module. The Bitcoin
wallet reaches it over Logos IPC, through `rpc()`, rather than through a port.

```bash
logosctl call bitcoind_module configure signet 'json:{"proxy":"127.0.0.1:9050"}'
logosctl call bitcoind_module start signet
logosctl call bitcoind_module status        # state, blocks, headers, peersOut, tipAgeSecs, ...
logosctl call bitcoind_module logTail 50
logosctl call bitcoind_module stop
```

## API

| Method | Returns |
|---|---|
| `configure(network, config)` | Merges `config` into the stored settings; refuses unknown keys, wrong types and unknown networks |
| `getConfig(network)`, `defaultConfig(network)` | The stored settings with defaults filled in; the defaults alone. Empty for an unknown network |
| `start(network)` | Starts the node and returns once its thread runs |
| `stop()` | Writes the UTXO cache (state `flushing`), then stops the node and waits for it |
| `flush()` | Writes the UTXO cache to disk and keeps it in memory |
| `status()` | Lifecycle and chain state; never waits on the node |
| `rpc(method, paramsJson)` | One call into Core's RPC table, as a JSON string |
| `logTail(lines)` | The last lines of the node's `debug.log` |

Event: `bitcoindStateChanged(payloadJson)` on every lifecycle transition (`stopped`,
`starting`, `running`, `flushing`, `stopping`, `failed`), with `status()`'s shape.

Networks are `mainnet`, `testnet4`, `signet` and `regtest`.

`status()` carries `state`, `network`, `version`, `uptimeSecs`, `lastError`, and from Core's
RPC `blocks`, `headers`, `verificationProgress`, `initialSync`, `peersOut`, `pruneHeight`
(0 on an unpruned node), `filterIndexHeight`, `tipAgeSecs` (the tip block's timestamp against
the clock) and `sizeOnDisk`, and `dbcacheMiB`, the cache the node was started with (0 for
Core's default). Numbers are -1 until known and while the node is not running.
A thread samples the node every second, so `status()` never waits on Core's locks; `state`
itself is read live.

During initial sync that thread also writes the UTXO cache every three minutes
(`ForceFlushStateToDisk(false)`, which keeps the cache), so a stop or an unload has little
left to write.

## rpc

`rpc(method, paramsJson)` runs one method of Core's RPC table in-process, as bitcoin-qt's
console does: there is no HTTP server, no port and no credential. `paramsJson` is a JSON array
or object, or `""` for none. The answer is Core's reply, `{"result":…,"error":null}` or
`{"result":null,"error":{"code":…,"message":…}}`; while the node starts Core answers
`-28` (in warmup) with its init message, and `-9` when the node is not running.

Only `bitcoin_wallet_core_module` and `bitcoin_node_module` may call it (`-32600`, not
authorized, otherwise), and only for these methods (`-32601` otherwise; `src/rpc_allowlist.h`):
`getblockchaininfo`, `getblockcount`, `getblockhash`, `getblockheader`, `getblock`,
`getblockfilter`, `getblockfrompeer`, `getrawmempool`, `getmempoolentry`, `getrawtransaction`,
`sendrawtransaction`, `estimatesmartfee`, `getnetworkinfo`, `getpeerinfo`. The wallet core's
filter restore reads `getblockfilter` and fetches a pruned block it needs with
`getblockfrompeer`, which asks one of the node's own peers: those connections follow the node's
proxy setting (`-proxy`, `-onlynet=onion`), so nothing leaves outside it. `getrawtransaction`
serves the mempool's transactions, and a block's with its hash.
On regtest, for the wallet's end-to-end tests, the same callers may also mine with
`generatetoaddress` and `generateblock`; they are refused on every other network, judged by the
chain Core itself runs.

The check reads the caller identity the host attaches to each call (`logos::currentCaller()`),
not anything the caller sends. For tests, a `callers.json` in the instance directory replaces
the caller list: `{"modules":["bitcoin_wallet_core_module"],"allowHost":true}`, where
`allowHost` admits calls from the host itself, such as `logosctl call`. A file that does not
parse, or has another key, admits nobody. It is read when the module loads.

## Settings

Per network, persisted in the instance directory (`bitcoind.json`).

| Key | Default | |
|---|---|---|
| `dataDir` | `<instance dir>/<network>` | Core's data directory; created owner-only |
| `pruneMiB` | `2048` mainnet, `550` elsewhere | MiB of recent blocks kept; `0` keeps every block. Core's minimum is 550 |
| `dbcacheMiB` | `0` | UTXO cache during initial sync; `0` leaves Core's default for the machine's memory. See below |
| `maxMempoolMiB` | `300` | |
| `proxy` | `""` | Tor's SOCKS address. Set, it adds `-proxy`, `-onlynet=onion` and `-privatebroadcast` |
| `logCategories` | `[]` | Core `-debug` categories, such as `["net","mempool"]` |
| `addNodes` | `[]` | Up to 8 peers to keep a connection to, `host:port` (Core's `-addnode`), such as one's own node |
| `exposeRpc` | `false` | Opens Core's RPC server on 127.0.0.1 for other programs; see below |

A large cache makes initial sync fast but a stop slow and a desktop heavy, so once a run of a
network has reached the tip (Core reports it out of initial sync), the module records it in
`bitcoind-state.json` in the instance directory, and later starts of that network use 300 MiB,
or `dbcacheMiB` when that is lower. A recorded tip more than a week old counts as catching up
again, and gets the sync-time cache.

Every start also passes `-blockfilterindex=1`, `-disablewallet`, `-persistmempool=0`,
`-listen=0` and `-noconf`. Core refuses `-privatebroadcast` when neither Tor nor I2P is
reachable, so without a proxy (clearnet, or regtest) the node broadcasts the ordinary way.

## Exposing the node to other programs

`exposeRpc` is an opt-in for other consumers on the machine, such as `bitcoin-cli` or a block
explorer, and is off by default. When set, the node also runs Core's HTTP RPC server:
`-server=1 -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1` on Core's port for the network (8332
mainnet, 48332 testnet4, 38332 signet, 18443 regtest), with cookie authentication only and the
cookie user limited by `-rpcwhitelist` to the same methods as `rpc()`. Core writes a fresh
cookie at every start, owner-only (`-rpccookieperms=owner`), to `.cookie` in the network's data
directory, and deletes it when the node stops:

```bash
bitcoin-cli -signet -datadir=<dataDir> getblockchaininfo
```

Restarting that server in one process needs two Core patches in logos-bitcoin-core-nix: the RPC
stop path's static `std::once_flag`s, and credentials and whitelists that only accumulated.

## Unloading

On unload the node is stopped on a thread and the outcome is written to `unload.log` in the
instance directory. The host waits up to its grace period (3 s); a node killed after that
replays its unwritten blocks on the next start, since Core's chainstate is crash-safe.

At the tip the final flush takes milliseconds. What used to hold a stop up was Core's
connection threads: a `connect()` in flight waited out `-timeout` (5 s) before `Shutdown` could
join them. logos-bitcoin-core-nix's patch 0005 makes that wait interruptible, so a stop cuts it
short; over Tor the wait is the SOCKS5 handshake, which Core already interrupts.

## Verified, 2026-10-10

In a real `logos_host` under logosctl, with HOME isolated:

- **macOS** (aarch64, logosctl 0.3.2): the config refusals; regtest running 0.5–0.7 s after
  `start`; every allowlisted method answers, named parameters included; the host is refused
  without `callers.json`, `generatetodescriptor` and `stop` always; `stop` in 0.26–0.56 s,
  restart running in 0.38–0.47 s, unload while running in 0.29–0.37 s (node stop 73–130 ms).
  `exposeRpc`: an owner-only cookie, 200 for a whitelisted method, 403 for another, 401 for a
  wrong or the previous start's cookie; the cookie is gone and the port closed after a stop.
- **Windows** 10.0.26200 (the portable logosctl): regtest running in 0.27–0.40 s, `stop` in
  0.09–0.19 s, restart in 0.22–0.41 s, unload while running in 0.19–0.24 s (node stop 72–127 ms).
- **Signet**, synced from scratch over clearnet in about 40 minutes (325,817 blocks; pruned to
  550 MiB, 481 MB on disk): unload during initial sync 665 ms. At the tip, before patch 0005:
  over Tor five unloads in 456–1357 ms, over clearnet four in 403–589 ms and one in 4.2 s, a
  connect in flight. With it: ten clearnet unloads with eight blackholed `addNodes`, one or two
  of their connects interrupted at each shutdown, in 488–642 ms (node stop 284–433 ms), and five
  over Tor in 570–1354 ms.
- **Regtest mining**: `generatetoaddress` (101 blocks) and `generateblock` through `rpc()`,
  refused on signet; `stop()` emits `flushing`, `stopping`, `stopped`; after the run that
  reached the tip, the next starts passed `-dbcache=300`.
- **testnet4** initial sync with `-debug=coindb`: the sync-time flush ran every three minutes
  (440k coins written in 566 ms).

## Licence

Bitcoin Core is MIT; its licence ships beside the plugin as `COPYING.bitcoin`, and the notices
of LevelDB, secp256k1, crc32c, minisketch and libevent as `NOTICES.bitcoind_c`.

## Development

```bash
nix build .#install    # result/modules/bitcoind_module: the plugin and libbitcoind_c
nix build .#checks.aarch64-darwin.rpc-allowlist   # rpc()'s allowlist, on its own
```
