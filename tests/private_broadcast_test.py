#!/usr/bin/env python3
"""Private broadcast with bitcoind_module's own flags for a proxy (tests/print_argv): Core 31.2
in-process through libbitcoind_c, under Core's test framework as p2p_private_broadcast.py does. A
SOCKS5 proxy sends the first private broadcast connection to a second regtest node and every other
connection to a recording peer, the -addnode included. README, "Private broadcast".

  private_broadcast_test.py --lib <libbitcoind_c> --framework <core src>/test/functional \\
      --print-argv <print_argv> --work <empty dir>
"""

import argparse
import base64
import ctypes
import decimal
import hashlib
import json
import os
import re
import secrets
import subprocess
import sys
import threading
import time

NUM_PRIVATE_BROADCAST_PER_TX = 3  # Core's NUM_PRIVATE_BROADCAST_PER_TX (src/net_processing.cpp)


def run_node(lib_path, argv_json):
    """Child process: one Core node through libbitcoind_c, with RPC over stdin and stdout."""
    lib = ctypes.CDLL(lib_path)
    lib.BITCOIND_start.argtypes = [ctypes.c_char_p]
    lib.BITCOIND_start.restype = ctypes.c_int
    lib.BITCOIND_rpc.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lib.BITCOIND_rpc.restype = ctypes.c_void_p
    lib.BITCOIND_last_error.restype = ctypes.c_void_p
    lib.BITCOIND_free.argtypes = [ctypes.c_void_p]

    def take(p):
        s = ctypes.string_at(p).decode()
        lib.BITCOIND_free(p)
        return s

    def answer(v):
        sys.stdout.write((v if isinstance(v, str) else json.dumps(v)) + "\n")
        sys.stdout.flush()

    if lib.BITCOIND_start(argv_json.encode()) != 0:
        answer({"error": take(lib.BITCOIND_last_error())})
        return 1
    answer({"started": True})
    for line in sys.stdin:
        req = json.loads(line)
        if req["method"] == "__stop":
            break
        answer(take(lib.BITCOIND_rpc(req["method"].encode(), json.dumps(req["params"]).encode())))
    lib.BITCOIND_stop()
    answer({"stopped": True})
    return 0


class RPCError(Exception):
    pass


class Node:
    """A node in its own process; calls like node.getblockcount() go to Core's RPC table."""

    def __init__(self, name, lib, argv):
        self.name = name
        self.argv = argv
        self.p = subprocess.Popen([sys.executable, __file__, "--node", lib, json.dumps(argv)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        first = json.loads(self.p.stdout.readline())
        assert first.get("started"), f"{name}: {first}"
        self.lock = threading.Lock()
        deadline = time.time() + 120
        while True:
            try:
                self.call("getblockchaininfo")
                break
            except RPCError as e:
                assert time.time() < deadline, f"{name} never left warmup: {e}"
                time.sleep(0.2)

    def call(self, method, *args, **kwargs):
        with self.lock:
            self.p.stdin.write(json.dumps({"method": method, "params": kwargs or list(args)}) + "\n")
            self.p.stdin.flush()
            # Amounts as Decimal, as the framework's own RPC client reads them.
            reply = json.loads(self.p.stdout.readline(), parse_float=decimal.Decimal)
        if reply.get("error"):
            raise RPCError(f"{self.name}.{method}: {reply['error']}")
        return reply["result"]

    def __getattr__(self, method):
        if method.startswith("_"):
            raise AttributeError(method)
        return lambda *args, **kwargs: self.call(method, *args, **kwargs)

    def stop(self):
        with self.lock:
            self.p.stdin.write(json.dumps({"method": "__stop", "params": []}) + "\n")
            self.p.stdin.flush()
            self.p.stdout.readline()
        self.p.wait(timeout=60)


def onion_v3():
    """A random but well-formed v3 onion name (Core checks the checksum and version)."""
    pubkey = secrets.token_bytes(32)
    checksum = hashlib.sha3_256(b".onion checksum" + pubkey + b"\x03").digest()[:2]
    return base64.b32encode(pubkey + checksum + b"\x03").decode().lower() + ".onion"


def wait_until(what, f, timeout=60):
    deadline = time.time() + timeout
    while not f():
        assert time.time() < deadline, f"timed out: {what}"
        time.sleep(0.1)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--node":
        return run_node(sys.argv[2], sys.argv[3])
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True)
    ap.add_argument("--framework", required=True)
    ap.add_argument("--print-argv", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args()
    sys.path.insert(0, args.framework)
    from test_framework.p2p import NetworkThread, P2PInterface, P2P_SERVICES, start_p2p_listener
    from test_framework.socks5 import start_socks5_server
    from test_framework.v2_p2p import EncryptedP2PState
    from test_framework.wallet import MiniWallet

    network = NetworkThread()
    network.start()
    wait_until("the network thread", lambda: NetworkThread.network_event_loop is not None, 10)

    class Recorder(P2PInterface):
        """A peer that records every inventory announced to it, then asks for it as any peer would.
        It answers BIP324 or v1, whichever the node opens with: the module leaves -v2transport on."""

        def __init__(self):
            super().__init__()
            self.announced = set()
            self.transport_chosen = False

        def data_received(self, t):
            # The framework's responder drops the v1 prefix it read before falling back to v1, so
            # the choice is made here, on the first bytes: v1 opens with the network's magic.
            if not self.transport_chosen and len(self.recvbuf) + len(t) >= 4:
                self.transport_chosen = True
                if not (self.recvbuf + t).startswith(self.magic_bytes):
                    self.v2_state = EncryptedP2PState(initiating=False, net="regtest")
            super().data_received(t)

        def on_inv(self, message):
            for i in message.inv:
                self.announced.add(i.hash)
            super().on_inv(message)

    def listener():
        p = Recorder()
        p.peer_connect_helper(dstaddr="0.0.0.0", dstport=0, net="regtest", timeout_factor=1)
        p.peer_connect_send_version(services=P2P_SERVICES)
        return p, start_p2p_listener(network, p)

    for d in ("originator", "receiver"):
        os.makedirs(f"{args.work}/{d}", exist_ok=True)
    receiver_port = 20000 + secrets.randbelow(20000)
    receiver = Node("receiver", args.lib, ["-regtest", f"-datadir={args.work}/receiver", "-noconf", "-listen=1",
                                          f"-bind=127.0.0.1:{receiver_port}=onion", "-connect=0", "-disablewallet"])

    # The node's existing peer: its -addnode, as the module's addNodes setting writes it.
    peer_onion = onion_v3()
    state = {"watcher": None, "pb_to_receiver": False}
    dests = []
    lock = threading.Lock()
    log_path = f"{args.work}/originator/regtest/debug.log"

    def conn_type(addr, port):
        """The type of the node's newest attempt at addr:port, from its debug log, as Core's test reads it."""
        pattern = re.compile(rf".*trying v. connection \((.+)\) to \[?{re.escape(addr)}\]?:{port},")
        found = []

        def look():
            try:
                with open(log_path, encoding="utf-8") as f:
                    found.extend(m.group(1) for m in map(pattern.match, f) if m)
            except FileNotFoundError:
                pass
            return bool(found)

        wait_until(f"the log line for {addr}:{port}", look, 15)
        return found[-1]

    def destinations(addr, port):
        kind = conn_type(addr, port)
        with lock:
            if kind == "private-broadcast" and not state["pb_to_receiver"]:
                state["pb_to_receiver"] = True
                dests.append({"to": addr, "type": kind, "peer": None})
                return {"actual_to_addr": "127.0.0.1", "actual_to_port": receiver_port}
            peer, (host, p) = listener()
            if addr == peer_onion:
                state["watcher"] = peer
            dests.append({"to": addr, "type": kind, "peer": peer})
            return {"actual_to_addr": host, "actual_to_port": p}

    proxy = start_socks5_server(destinations)
    settings = {"dataDir": f"{args.work}/originator", "pruneMiB": 550, "dbcacheMiB": 0, "maxMempoolMiB": 300,
                "proxy": f"{proxy.conf.addr[0]}:{proxy.conf.addr[1]}", "logCategories": ["net", "privatebroadcast"],
                "exposeRpc": False, "addNodes": [f"{peer_onion}:18444"]}
    argv = json.loads(subprocess.check_output([args.print_argv, "regtest", json.dumps(settings)], text=True))
    for flag in [f"-proxy={settings['proxy']}", "-onlynet=onion", "-privatebroadcast", "-listen=0", f"-addnode={peer_onion}:18444"]:
        assert flag in argv, f"the module's flags lack {flag}: {argv}"
    print("module flags:", " ".join(argv))
    node = Node("originator", args.lib, argv)

    # A chain both nodes hold: mined here, handed over block by block.
    wallet = MiniWallet(node)
    wallet.generate(110)
    for h in range(1, node.getblockcount() + 1):
        receiver.submitblock(node.getblock(node.getblockhash(h), 0))
    assert receiver.getblockcount() == node.getblockcount()
    # Onion addresses for private broadcast to draw from (and for automatic connections).
    for _ in range(20):
        node.addpeeraddress(onion_v3(), 18444)
    wait_until("the -addnode peer", lambda: state["watcher"] is not None and state["watcher"].message_count["verack"] >= 1, 60)
    print(f"existing peer: {peer_onion} (manual), handshake done")

    tx = wallet.create_self_transfer()
    txid, wtxid = int(tx["txid"], 16), int(tx["wtxid"], 16)
    assert node.sendrawtransaction(tx["hex"]) == tx["txid"]
    assert tx["txid"] not in node.getrawmempool(), "private broadcast keeps it out of the mempool"
    wait_until("the receiver's mempool", lambda: tx["txid"] in receiver.getrawmempool(), 90)
    print(f"the receiver holds {tx['txid']}, from a private broadcast connection")

    # The other private broadcast connections: each a fresh peer, told the transaction, then closed.
    def private():
        with lock:
            return [d for d in dests if d["type"] == "private-broadcast" and d["peer"] is not None]
    wait_until("the other private broadcast connections", lambda: len(private()) >= NUM_PRIVATE_BROADCAST_PER_TX - 1, 90)
    for d in private()[:NUM_PRIVATE_BROADCAST_PER_TX - 1]:
        p = d["peer"]
        p.wait_for_disconnect(timeout=90)
        v = p.last_message["version"]
        assert (v.nServices, v.relay, v.nStartingHeight) == (0, 0, 0), "a private broadcast version tells nothing"
        assert p.message_count["tx"] == 1 and p.last_message["tx"].tx.txid_hex == tx["txid"], dict(p.message_count)
        assert txid in p.announced or wtxid in p.announced
        print(f"private broadcast connection to {d['to']}: version, inv, tx, closed")

    # Nothing over an existing connection: the -addnode peer, and every automatic one.
    time.sleep(10)
    with lock:
        others = [d for d in dests if d["type"] != "private-broadcast" and d["peer"] is not None]
    for d in others:
        p = d["peer"]
        assert txid not in p.announced and wtxid not in p.announced and p.message_count["tx"] == 0, \
            f"{d['type']} peer {d['to']} heard the transaction"
    types = sorted({d["type"] for d in others})
    print(f"no existing connection carried it: {len(others)} peers ({', '.join(types)})")
    live = node.getpeerinfo()
    assert any(p["connection_type"] == "manual" for p in live), "the -addnode peer stays connected"
    wait_until("the private broadcast connections closed",
               lambda: not any(p["connection_type"] == "private-broadcast" for p in node.getpeerinfo()), 60)
    pending = [t for t in node.getprivatebroadcastinfo()["transactions"] if t["txid"] == tx["txid"]]
    assert len(pending) == 1 and len(pending[0]["peers"]) >= NUM_PRIVATE_BROADCAST_PER_TX, pending
    node.stop()

    # Control: the same flags without -privatebroadcast. The existing peer hears of the next one.
    state["watcher"] = None
    plain = [a for a in argv if a != "-privatebroadcast"]
    node = Node("originator", args.lib, plain)
    wallet = MiniWallet(node)
    wallet.rescan_utxos()
    wait_until("the -addnode peer again", lambda: state["watcher"] is not None and state["watcher"].message_count["verack"] >= 1, 60)
    tx2 = wallet.create_self_transfer()
    node.sendrawtransaction(tx2["hex"])
    assert tx2["txid"] in node.getrawmempool()
    watcher = state["watcher"]
    wait_until("the existing peer told of it", lambda: int(tx2["wtxid"], 16) in watcher.announced or int(tx2["txid"], 16) in watcher.announced, 60)
    print("control: without -privatebroadcast the existing peer is told of the transaction")
    node.stop()
    receiver.stop()
    proxy.stop()
    network.close(timeout=10)
    print("private broadcast: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
