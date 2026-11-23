#!/usr/bin/env python3
"""Exercise real headless collectors, TLS transport, offline spooling, and replay-safe ingestion."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import secrets
import socket
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import time


def until(check, message, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            if check():
                return
        except (sqlite3.OperationalError, ConnectionRefusedError, FileNotFoundError):
            pass
        time.sleep(0.05)
    raise AssertionError(message)


def scalar(path, sql, args=()):
    with contextlib.closing(sqlite3.connect(f"file:{path}?mode=ro", uri=True)) as db:
        return db.execute(sql, args).fetchone()[0]


def verify(binary, work, office_node=None, branch_node=None, viewer=None):
    def write(name, obj):
        path = work / name
        path.write_text(json.dumps(obj))
        return path

    cert, key = work / "cert.pem", work / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                    "-keyout", str(key), "-out", str(cert)], stdout=subprocess.DEVNULL,
                   stderr=subprocess.PIPE, check=True)
    context = ssl.create_default_context(cafile=str(cert))
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
    sites = {"office": "office.token", "branch": "branch.token", "replay-test": "test.token"}
    for name in sites.values():
        (work / name).write_text(secrets.token_hex(32))
    database = work / "history.sqlite"
    server = write("receiver.json", {"listen": "127.0.0.1", "port": port, "database": str(database),
                                      "certificate": str(cert), "privateKey": str(key), "sites": sites})
    collectors, spools = {}, {}
    for site in ("office", "branch"):
        spools[site] = work / (site + ".sqlite")
        collectors[site] = write(site + ".json", {
            "site": site, "receiver": "127.0.0.1", "port": port, "tokenFile": sites[site],
            "caCertificate": str(cert), "spool": str(spools[site]), "spoolMiB": 1,
            "targets": [{"target": ip, "family": family, "protocol": "tcp",
                         "port": port, "intervalMs": 250} for ip, family in (("127.0.0.1", 4), ("::1", 6))]})
    children = []
    def launch(mode, path):
        log = open(work / (path.stem + ".log"), "ab", buffering=0)
        try:
            executable = binary
            if mode == "collect":
                site = json.loads(path.read_text())["site"]
                executable = (branch_node if site == "branch" else office_node) or binary
            child = subprocess.Popen([executable, mode, path], stdout=log, stderr=log)
        finally:
            log.close()
        children.append(child)
        return child

    def stop(child):
        child.terminate()
        assert child.wait(timeout=10) == 0

    def talk(sock, obj):
        sock.sendall((json.dumps(obj) + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            chunk = sock.recv(2048)
            if not chunk:
                raise AssertionError("Missing receiver acknowledgement")
            data += chunk
        return json.loads(data)

    def tls():
        return context.wrap_socket(socket.create_connection(("127.0.0.1", port), 3), server_hostname="localhost")

    def listening():
        with socket.create_connection(("127.0.0.1", port), 1):
            return True

    try:
        desktop = None
        if viewer:
            environment = dict(os.environ, HOPLINE_REMOTE_DB=str(database), QT_QPA_PLATFORM="offscreen",
                               QT_QUICK_BACKEND="software", QT_QPA_PLATFORMTHEME="generic")
            desktop = subprocess.Popen([viewer, "twoSiteWindow"], env=environment)
            children.append(desktop)
            until(lambda: (work / "ui.ready").exists(), "desktop failed to open the receiver history")
        # Unavailable receiver: spool complete rounds across a process restart.
        offline = launch("collect", collectors["office"])
        until(lambda: scalar(spools["office"], "SELECT count(*) FROM outbox") >= 6, "no offline samples")
        stop(offline)
        first = scalar(spools["office"], "SELECT min(json_extract(body,'$.samples[0][0]')) FROM outbox")
        # Simulate an existing outbox larger than a newly lowered 1 MiB limit.
        overflow_config = json.loads(collectors["office"].read_text())
        overflow_path = work / "overflow.sqlite"
        overflow_config["spool"] = str(overflow_path)
        with contextlib.closing(sqlite3.connect(spools["office"])) as src, contextlib.closing(sqlite3.connect(overflow_path)) as dest:
            src.backup(dest)
        with sqlite3.connect(overflow_path) as queue:
            row = queue.execute("SELECT body FROM outbox ORDER BY id LIMIT 1").fetchone()[0]
            count = 1100000 // len(row.encode()) + 1
            queue.executemany("INSERT INTO outbox(body) VALUES(?)", ((row,) for _ in range(count)))
        overflow = launch("collect", write("overflow.json", overflow_config))
        until(lambda: "at capacity" in (work / "overflow.log").read_text(), "no bounded-spool eviction")
        stop(overflow)
        assert scalar(overflow_path, "SELECT sum(length(cast(body AS blob))) FROM outbox") <= 1048576
        assert scalar(overflow_path, "SELECT min(id) FROM outbox") > 1
        receiver = launch("receive", server)
        until(listening, "receiver not listening")
        with tls() as sock:
            denied = talk(sock, {"v": 1, "type": "hello", "site": "office", "seq": 0,
                                 "token": (work / sites["branch"]).read_text()})
            assert denied.get("error") == "Authentication failed"
        for site in collectors:
            launch("collect", collectors[site])
        until(lambda: scalar(database, "SELECT count(*) FROM session_origin WHERE clock_error_ms>=0") == 4,
              "four live IPv4/IPv6 remote sessions with clock estimates")
        if desktop:
            assert desktop.wait(timeout=20) == 0, "desktop did not render the two live sites"
        assert scalar(database, "SELECT min(t) FROM samples") == first
        assert scalar(database, "SELECT count(*) FROM receiver_streams") == 4
        assert scalar(database, "SELECT count(DISTINCT location) FROM receiver_streams") == 2
        assert scalar(database, "SELECT count(*) FROM samples WHERE hop=1 AND dest=1") > 4
        stop(receiver)
        until(lambda: scalar(spools["branch"], "SELECT count(*) FROM outbox") >= 4, "no retry spool")
        backlog = scalar(spools["branch"], "SELECT max(id) FROM outbox")
        receiver = launch("receive", server)
        until(lambda: scalar(spools["branch"],"SELECT count(*) FROM outbox WHERE id<=?", (backlog,)) == 0,
              "no backfill after receiver restart")
        assert scalar(database, "SELECT count(*) FROM receiver_streams") == 4

        # Replay an acknowledged batch exactly as after a lost ACK.
        token = (work / sites["replay-test"]).read_text()
        t = int(time.time() * 1000) - 3600000
        body = {"trace": {"target": "localhost", "family": 4, "intervalMs": 1000, "protocol": "icmp"},
                "resolvedAddress": "127.0.0.1", "clockOffsetMs": 17, "clockErrorMs": 3,
                "samples": [[t, 1, "127.0.0.1", 4.5, True], [t + 1000, 1, "", -1, False]]}
        def deliver(payload):
            with tls() as sock:
                assert talk(sock, {"v":1,"type":"hello","seq":0,"site":"replay-test","token":token})["ack"] == 0
                assert talk(sock, {"v":1,"type":"batch","seq":7,"body":payload})["ack"] == 7

        deliver(body)
        session = scalar(database, "SELECT session FROM receiver_streams WHERE location='replay-test'")
        with sqlite3.connect(database) as db:
            db.execute("INSERT INTO alert_rules(session,latency,loss,seconds) VALUES(?,?,0,1)", (session, 75))
        newer = dict(body, clockOffsetMs=91)
        newer["samples"] = [[t + i * 1000, 1, "127.0.0.1", 200 if i < 6 else 5, True] for i in range(2, 10)]
        deliver(newer)
        stop(receiver)
        receiver = launch("receive", server)
        def resend():
            deliver(body)
            deliver(newer)
            deliver(body)
            return True
        until(resend, "receiver restart dropped retry")
        assert scalar(database, "SELECT count(*) FROM samples WHERE session=? AND hop=1", (session,)) == 10
        assert scalar(database, "SELECT sum(sent) FROM bins WHERE session=? AND hop=0 AND span=10000", (session,)) == 10
        assert scalar(database, "SELECT clock_offset_ms FROM session_origin WHERE session=?", (session,)) == 91
        assert scalar(database, "SELECT count(*) FROM alert_events WHERE session=? AND reason=1", (session,)) == 1
        assert scalar(database, "SELECT count(*) FROM alert_events WHERE session=? AND reason=4", (session,)) == 1

        # An untrusted server certificate must leave authenticated data in the spool.
        wrong = json.loads(collectors["office"].read_text())
        del wrong["caCertificate"]
        wrong["spool"] = str(work / "untrusted.sqlite")
        bad = launch("collect", write("untrusted.json", wrong))
        until(lambda: scalar(wrong["spool"], "SELECT count(*) FROM outbox") >= 4 and
              "Unable to upload" in (work / "untrusted.log").read_text(), "invalid TLS certificate was accepted")
        stop(bad)
        wrong.update(receiver="192.0.2.1", insecureLoopback=True)
        fail = subprocess.run([binary, "collect", write("unsafe.json", wrong)], capture_output=True, timeout=3)
        assert fail.returncode and b"literal loopback" in fail.stderr
        assert scalar(database, "PRAGMA integrity_check") == "ok"
    except BaseException:
        for path in work.glob("*.log"):
            print(path.name, path.read_text()[-2000:], file=sys.stderr)
        raise
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
        for child in children:
            child.wait(timeout=10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("node", type=Path, nargs="?", default=Path("build-node/hopline-node"))
    parser.add_argument("--office-node", type=Path, help="Collector executable for the office site")
    parser.add_argument("--branch-node", type=Path, help="Collector executable for the branch site")
    parser.add_argument("--viewer", type=Path, help="Qt integration binary for an open desktop comparison")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        verify(args.node.resolve(), Path(tmp), args.office_node, args.branch_node, args.viewer)
    print("Two-site dual-stack telemetry, verified TLS, clock estimates, backfill, authentication and duplicate delivery passed.")
