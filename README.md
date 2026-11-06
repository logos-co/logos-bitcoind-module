# logos-bitcoind-module

`bitcoind_module` runs a Bitcoin full node, [Bitcoin Core](https://github.com/bitcoin/bitcoin)
31.2, **in-process**, through `libbitcoind_c` from `logos-bitcoin-core-nix`. No executable is
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

`tor_module` is an optional dependency: a start asks it for its SOCKS port when the node is to send
its transactions over Tor ([Private sends](#private-sends-through-logos-tor)).

`status()` carries `state`, `network`, `version`, `uptimeSecs`, `lastError`, and from Core's
RPC `blocks`, `headers`, `verificationProgress`, `initialSync`, `peersOut`, `pruneHeight`
(0 on an unpruned node), `filterIndexHeight`, `tipAgeSecs` (the tip block's timestamp against
the clock) and `sizeOnDisk`, and `dbcacheMiB`, the cache the node was started with (0 for
Core's default). Numbers are -1 until known and while the node is not running.
`privateSends` says what the node does with its own transactions now, and `privateSendsReason`
why when that is `unavailable` or `restart` ([below](#private-sends-through-logos-tor)).
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
`getblockfrompeer`, which asks one of the node's own peers: with a proxy every peer is an onion
service behind it (`-proxy`, `-onlynet=onion`), so nothing leaves outside it; without one the
peer may be a clearnet one, as for the node's sync. `getrawtransaction`
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
| `proxy` | `""` | Tor's SOCKS address. Set, it adds `-proxy`, `-onlynet=onion` and `-privatebroadcast`, whatever `privateSends` says |
| `privateSends` | `true`, `false` on regtest | With no `proxy`: the node's own transactions over Logos' Tor, `-onion=<its SOCKS port>` and `-privatebroadcast`; the node still syncs directly. See below |
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
reachable, so with neither a proxy nor Logos' Tor (`privateSends` off, as on regtest by default,
or Tor unavailable) the node broadcasts the ordinary way.

## Private broadcast

The node runs with `-privatebroadcast` in two cases:

- **Through Logos' Tor**, `privateSends` with no `proxy`, the default on mainnet, testnet4 and
  signet: `-onion=<tor_module's SOCKS port>`, without `-proxy` or `-onlynet`. Tor carries the
  private broadcast connections and any connection to an onion service; the node syncs over
  clearnet as before. [Below](#private-sends-through-logos-tor).
- **Through the user's proxy**, `proxy` set: `-proxy` and `-onlynet=onion`, so every connection
  goes through the proxy to onion services.

What Bitcoin Core 31.2 does with it, from its source at the v31.2 tag (241537a):

- `sendrawtransaction` leaves the transaction out of the node's mempool and hands it to private
  broadcast (`rpc/mempool.cpp:130`, `node/transaction.cpp:105`). Core's own wallet is not
  covered (`-privatebroadcast`'s help, `init.cpp:687`); the module runs with `-disablewallet`.
- Each transaction gets 3 new connections (`NUM_PRIVATE_BROADCAST_PER_TX`,
  `net_processing.cpp:201`, `:2275`), each to a random address of a reachable privacy network:
  onion services, IPv4 or IPv6 through Tor's proxy when those are reachable, or I2P
  (`net.cpp:3102`). With `-onlynet=onion` only onion services are reachable, so all three go to
  onion services through the proxy, each on its own circuit (`-proxyrandomize`, on by default;
  Core warns when it is off, `init.cpp:2280`).
- On such a connection the version message says nothing of the node: services 0, time 0, height
  0, no relay, empty addresses, and a fixed user agent, `/pynode:0.0.1/`, not Core's
  (`net_processing.cpp:1557`). The peer gets `inv`, then the transaction when it asks, and the
  connection closes; none lasts more than 3 minutes (`net_processing.cpp:203`, `:5758`).
- The node's existing connections are not told while the broadcast is private: the transaction
  isn't in the mempool, so nothing announces it. Once the node hears it back from the network it
  stops the private attempts (`net_processing.cpp:4504`) and relays it as any transaction.
- A transaction not heard back in time goes out privately again (`net_processing.cpp:1658`).
- At most 10,000 transactions wait to be heard back; past that, new ones are refused
  (`private_broadcast.h:44`, `net_processing.cpp:2282`, new in 31.2).
- Core refuses to start with `-privatebroadcast` when neither Tor nor I2P is reachable, or with
  `-connect` (`init.cpp:2264`). The 31.1 release notes fix a leak in 31.0, where some of these
  connections were made over clearnet instead of the privacy network (#35410, the proxy kept on a
  v2-to-v1 reconnect), and stop them from changing addrman (#35032).

So the three receiving onion peers learn the transaction, and none of the node's own peers
hears it from the node first; nothing in the feature hides that a transaction was broadcast from
some Tor user, or protects a transaction sent another way.

`tests/private_broadcast_test.py` checks it with the module's own flags (`tests/print_argv` prints
them), Core 31.2 in-process through libbitcoind_c, under Core's own test framework, as Core's
`p2p_private_broadcast.py` does: a SOCKS5 proxy routes the first private broadcast connection to a
second regtest node and every other connection to a Python peer that records what it hears, the
`-addnode` peer included (BIP324 or v1, as the node opens). The transaction stays out of the
mempool and reaches the second node; the other two private connections each get the bare version,
`inv` and the transaction, then close; none of the 11 to 13 existing connections (manual,
outbound full relay, block relay only, feeler) hears of it. A control run without
`-privatebroadcast` tells the existing peer. `nix build .#checks.<system>.private-broadcast` runs
it (about a minute); `private-broadcast-tor` runs the same with Logos' Tor's flags (below).

### Private sends through Logos' Tor

With `privateSends` and no `proxy`, a start asks `tor_module` for its SOCKS port and passes
`-onion=IP:PORT -privatebroadcast`. What Core 31.2 does with that, from the same source:

- `-onion` alone gives onion services a proxy and keeps them reachable (`init.cpp:1768`, `:1792`).
  IPv4 and IPv6 stay reachable with no proxy: only `-proxy` sets theirs (`init.cpp:1707`). So the
  check that Tor or I2P is reachable passes (`init.cpp:2259`), and the node's connections to
  clearnet peers, for blocks, headers and relay, are made directly, as without Tor.
- Private broadcast picks onion services, and IPv4 or IPv6 peers through the same Tor proxy once
  one outbound connection over Tor has completed its handshake (`net.cpp:3102`, `:3161`,
  `:4116`). It never picks a direct connection.
- `-proxyrandomize` (on by default, `init.cpp:152`) gives each connection through the onion proxy
  its own SOCKS credentials (`init.cpp:1786`), and `tor_module` never lets streams with different
  credentials share a circuit (its README, "The SOCKS port"): each private broadcast connection
  rides its own circuit.
- Onion services being reachable, addrman can also pick some for the node's ordinary outbound
  connections: those go through Tor, the rest over clearnet. The node's clearnet peers still see
  this computer's IP address running a Bitcoin node; what private broadcast hides is which node a
  transaction came from, as above.

`tests/private_broadcast_test.py --mode tor` checks it with these flags and an `-addnode` the
node reaches directly, as a clearnet peer: Core starts with them; `getnetworkinfo` shows onion
services through the proxy and IPv4 and IPv6 direct; the transaction stays out of the mempool and
reaches the second node through the proxy; the other two private connections get the bare
version, `inv` and the transaction; neither the direct peer nor the automatic connections to
onion services through the proxy (10 and 11 in two runs on 2026-11-06) hear of it; and every private
broadcast connection in the node's log went through the proxy. The control run without
`-privatebroadcast` tells the direct peer. No test reaches an IPv4 peer through Tor: a routable
IPv4 address in addrman would also be dialled directly, by the node's sync.

**Logos' Tor and its SOCKS port.**

- A start asks `tor_module.status()` only when it would use the answer, since asking starts an
  idle Tor. Its `socks`, `socks5h://IP:PORT`, becomes `-onion=IP:PORT`. The module waits up to
  2 s for it.
- When `tor_module` is not loaded, does not answer, answers an error, or has no SOCKS port (the
  user turned it off in the Tor app), the node starts without private sends and broadcasts the
  ordinary way: `status()` says `privateSends: "unavailable"` and why. A later start asks again.
- Core reads `-onion` once, at start. While the node runs with private sends the module asks
  `tor_module` again every 30 s; when the port has moved (another chosen in the Tor app, or a
  restarted Tor that found 9250 taken) or closed, `status()` says `privateSends: "restart"` and
  why. Until the node restarts, its private broadcast connections fail at the old port and Core
  keeps trying others there; none goes out directly.
- Transactions waiting for private broadcast are kept in memory only, outside the mempool
  (`private_broadcast.h:216`), so a stop or restart forgets those not yet sent.

`status().privateSends`, for the node that runs now:

| Value | |
|---|---|
| `on` | Private broadcast through Logos' Tor |
| `proxy` | Private broadcast through the user's proxy |
| `off` | `privateSends` off and no proxy: the ordinary way |
| `unavailable` | `privateSends` on, but Logos' Tor had no SOCKS port to give at start: the ordinary way. `privateSendsReason` says why |
| `restart` | Started `on`, but Tor's SOCKS port has since moved or closed. `privateSendsReason` says what changed |
| `""` | No node running (`stopped` or `failed`) |

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
- **Windows, 2026-10-11** (logosctl's portable bundle, the module cross-built from this tree):
  in one `logos_host`, regtest running in 0.27 s and stopped in 94 ms; signet through a local Tor
  running in 0.24 s, at an onion peer after 27 s, stopped in 234 ms; regtest again with its 101
  blocks. 20 of 20 unloads at the tip within 2 s (median 151 ms, max 954 ms, node stop 33–133 ms),
  half of them with eight blackholed `addNodes` connecting. The wallet core's local-node route
  passed there too. `status()` used to iterate a temporary that C++17 had already destroyed:
  while the node was stopped or starting, the Windows host died (0xc0000005) in 3 of about 90
  loads that called `configure` and `status` in their first second, Linux in none of 40. Fixed,
  0 of 60 since.

## Licence

Bitcoin Core is MIT; its licence ships beside the plugin as `COPYING.bitcoin`, and the notices
of LevelDB, secp256k1, crc32c, minisketch and libevent as `NOTICES.bitcoind_c`.

## Development

```bash
nix build .#install    # result/modules/bitcoind_module: the plugin and libbitcoind_c
nix build .#checks.aarch64-darwin.rpc-allowlist   # rpc()'s allowlist, on its own
nix build .#checks.aarch64-darwin.private-broadcast   # private broadcast with the module's flags
nix build .#checks.aarch64-darwin.private-broadcast-tor   # the same through Logos' Tor (privateSends)
nix build .#checks.aarch64-darwin.private-sends   # tor_module's reply, the modes and their flags
```
