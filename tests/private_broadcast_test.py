#!/usr/bin/env python3
"""Private broadcast with bitcoind_module's own flags (tests/print_argv): Core 31.2 in-process
through libbitcoind_c, under Core's test framework as p2p_private_broadcast.py does. A SOCKS5 proxy
sends the first private broadcast connection to a second regtest node and every other connection to
a recording peer. README, "Private broadcast".

--mode proxy: the user's proxy (-proxy, -onlynet=onion); the -addnode is an onion peer behind it.
--mode tor: privateSends with Logos' Tor (-onion only); the -addnode is a clearnet peer the node
dials directly, as its sync does.

  private_broadcast_test.py --lib <libbitcoind_c> --framework <core src>/test/functional \\
      --print-argv <print_argv> --work <empty dir> [--mode proxy|tor]
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
    ap.add_argument("--mode", choices=["proxy", "tor"], default="proxy")
    args = ap.parse_args()
    tor = args.mode == "tor"
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

    def recorder():
        p = Recorder()
        p.peer_connect_helper(dstaddr="0.0.0.0", dstport=0, net="regtest", timeout_factor=1)
        p.peer_connect_send_version(services=P2P_SERVICES)
        return p

    def listener():
        p = recorder()
        return p, start_p2p_listener(network, p)

    def clearnet_watcher(port=0):
        """A recording peer the node reaches directly, not through the proxy; port 0 picks one."""
        p = recorder()
        got = []
        network.listen(p, lambda a, pt: got.append(pt), port=port, addr="127.0.0.1")
        wait_until("the clearnet listener", lambda: got, 10)
        return p, got[0]

    def print_argv(settings, tor_socks=None):
        extra = [tor_socks] if tor_socks is not None else []
        return json.loads(subprocess.check_output([args.print_argv, "regtest", json.dumps(settings)] + extra, text=True))

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
    socks = f"{proxy.conf.addr[0]}:{proxy.conf.addr[1]}"
    settings = {"dataDir": f"{args.work}/originator", "pruneMiB": 550, "dbcacheMiB": 0, "maxMempoolMiB": 300,
                "proxy": socks, "logCategories": ["net", "privatebroadcast"],
                "exposeRpc": False, "addNodes": [f"{peer_onion}:18444"], "privateSends": False}
    if tor:
        # privateSends with Logos' Tor at `socks`: the module's other cases first, flags only.
        settings["proxy"] = ""
        settings["privateSends"] = True
        for case, s, t in [("unavailable", settings, ""), ("off", {**settings, "privateSends": False}, socks)]:
            a = print_argv(s, t)
            assert not [f for f in a if f.startswith(("-onion", "-proxy", "-onlynet", "-privatebroadcast"))], f"{case}: {a}"
        a = print_argv({**settings, "proxy": "127.0.0.1:9050"}, socks)
        assert "-proxy=127.0.0.1:9050" in a and "-onlynet=onion" in a and "-privatebroadcast" in a, a
        assert not [f for f in a if f.startswith("-onion")], f"a proxy keeps its own flags: {a}"
        watcher, watcher_port = clearnet_watcher()
        state["watcher"] = watcher
        settings["addNodes"] = [f"127.0.0.1:{watcher_port}"]
        argv = print_argv(settings, socks)
        expected = [f"-onion={socks}", "-privatebroadcast", "-listen=0", f"-addnode=127.0.0.1:{watcher_port}"]
        assert not [f for f in argv if f.startswith(("-proxy", "-onlynet"))], f"privateSends adds no -proxy or -onlynet: {argv}"
    else:
        argv = print_argv(settings)
        expected = [f"-proxy={socks}", "-onlynet=onion", "-privatebroadcast", "-listen=0", f"-addnode={peer_onion}:18444"]
    for flag in expected:
        assert flag in argv, f"the module's flags lack {flag}: {argv}"
    print("module flags:", " ".join(argv))
    node = Node("originator", args.lib, argv)
    if tor:
        # Onion peers through the proxy; IPv4 and IPv6 reachable and direct, so sync stays clearnet.
        nets = {n["name"]: n for n in node.getnetworkinfo()["networks"]}
        assert nets["onion"]["reachable"] and nets["onion"]["proxy"] == socks, nets["onion"]
        for n in ("ipv4", "ipv6"):
            assert nets[n]["reachable"] and nets[n]["proxy"] == "", nets[n]
        print(f"networks: onion through {socks}; ipv4 and ipv6 reachable, direct")

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
    print(f"existing peer: {settings['addNodes'][0]} (manual, {'direct' if tor else 'through the proxy'}), handshake done")

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
    if tor:
        w = state["watcher"]
        assert txid not in w.announced and wtxid not in w.announced and w.message_count["tx"] == 0, \
            "the clearnet -addnode peer heard the transaction"
        assert not any(d["to"] == "127.0.0.1" for d in dests), "the clearnet peer was reached through the proxy"
        print(f"nor did the clearnet -addnode peer, connected directly ({w.message_count['verack']} verack)")
        # Every private broadcast connection the node tried went through the proxy: none direct.
        def tried():
            with open(log_path, encoding="utf-8") as f:
                return {m.group(1) for m in map(re.compile(r".*trying v. connection \(private-broadcast\) to \[?([^\]\s]+?)\]?:\d+,").match, f) if m}
        def direct():
            with lock:
                return tried() - {d["to"] for d in dests if d["type"] == "private-broadcast"}
        assert tried(), "no private broadcast attempt in the log"
        wait_until("every private broadcast attempt at the proxy", lambda: not direct(), 15)
        print(f"all {len(tried())} private broadcast connections went through the proxy")
    wait_until("the private broadcast connections closed",
               lambda: not any(p["connection_type"] == "private-broadcast" for p in node.getpeerinfo()), 60)
    pending = [t for t in node.getprivatebroadcastinfo()["transactions"] if t["txid"] == tx["txid"]]
    assert len(pending) == 1 and len(pending[0]["peers"]) >= NUM_PRIVATE_BROADCAST_PER_TX, pending
    node.stop()

    if tor:
        # What the module's automatic restart relies on (src/carry_over.h): a restart forgets the
        # pending private broadcasts, the same hex is taken again, and a mined or spent one is refused.
        state["watcher"], _ = clearnet_watcher(watcher_port)
        node = Node("originator", args.lib, argv)
        wallet = MiniWallet(node)
        assert node.getprivatebroadcastinfo()["transactions"] == [], "a restart forgets pending private broadcasts"
        held = pending[0]["hex"]
        assert node.sendrawtransaction(held, 0, 21000000) == tx["txid"]
        assert [t["txid"] for t in node.getprivatebroadcastinfo()["transactions"]] == [tx["txid"]]
        assert tx["txid"] not in node.getrawmempool(), "sent again privately, still out of the mempool"
        node.generateblock(wallet.get_address(), [held])
        try:
            node.sendrawtransaction(held, 0, 21000000)
            raise AssertionError("a mined transaction was taken again")
        except RPCError as e:
            assert "'code': -27" in str(e), e
        wallet.rescan_utxos()
        utxo = wallet.get_utxo()
        spent = wallet.create_self_transfer(utxo_to_spend=utxo)
        rival = wallet.create_self_transfer(utxo_to_spend=utxo, fee_rate=decimal.Decimal("0.01"))
        node.generateblock(wallet.get_address(), [rival["hex"]])
        try:
            node.sendrawtransaction(spent["hex"], 0, 21000000)
            raise AssertionError("a double spend was taken again")
        except RPCError as e:
            assert "'code': -25" in str(e) and "missingorspent" in str(e), e
        print("restart: the queue was empty; the held transaction was taken again; mined -27, spent -25")
        node.stop()

    # Control: the same flags without -privatebroadcast. The existing peer hears of the next one.
    state["watcher"] = None
    if tor:
        state["watcher"], _ = clearnet_watcher(watcher_port)
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
    print(f"private broadcast ({args.mode}): ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
