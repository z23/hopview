#!/usr/bin/env python3
"""Verify target assignment, disconnected recording, and pinned enrollment on loopback."""
import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import secrets
import socket
import sqlite3
import ssl
import subprocess
import tempfile
import time
import uuid


def verify(binary, work):
    cert, private = work / "receiver.pem", work / "receiver.key"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                    "-keyout", str(private), "-out", str(cert)], check=True, capture_output=True)
    server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server.load_cert_chain(cert, private)
    pin = hashlib.sha256(ssl.PEM_cert_to_DER_cert(cert.read_text())).hexdigest()
    secret, node = secrets.token_hex(32), str(uuid.uuid4())
    path, spool, log = work / "node.json", work / "outbox.sqlite", work / "node.log"
    targets = [{"target": address, "family": family, "intervalMs": 250, "protocol": "tcp", "port": 9}
               for address, family in (("127.0.0.1", 4), ("::1", 6))]
    seen = set()

    @contextlib.contextmanager
    def run(config):
        path.write_text(json.dumps(config))
        with log.open("ab") as output:
            proc = subprocess.Popen([binary, "collect", path], stdout=output, stderr=output)
        try:
            yield proc
        finally:
            proc.terminate()
            assert proc.wait(timeout=5) == 0, log.read_text()

    @contextlib.contextmanager
    def accept(listener):
        peer, _ = listener.accept()
        peer.settimeout(10)
        with server.wrap_socket(peer, server_side=True) as secure, secure.makefile("rwb", buffering=0) as stream:
            yield stream

    def exchange(stream, revision, desired):
        msg = json.loads(stream.readline())
        now = int(time.time() * 1000)
        ack = {"v": 1, "ack": msg["seq"], "receivedMs": now, "sentMs": now}
        if msg["type"] == "batch":
            seen.add(msg["body"]["trace"]["family"])
        else:
            if msg["type"] == "hello":
                assert msg["managed"] and msg["nodeId"] == node and msg["hostname"]
                assert msg["token"] == secret
            ack.update(site="branch", revision=revision)
            if revision != msg["appliedRevision"]:
                ack["targets"] = desired
        stream.write((json.dumps(ack) + "\n").encode())
        return msg

    def until_message(stream, revision, desired, condition):
        end = time.monotonic() + 15
        while time.monotonic() < end:
            if condition(exchange(stream, revision, desired)):
                return
        raise AssertionError("Collector state did not converge")

    def pending():
        with contextlib.closing(sqlite3.connect(spool)) as db:
            return db.execute("SELECT count(*) FROM outbox").fetchone()[0]

    def await_backlog(at_least):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            if pending() >= at_least:
                return
            time.sleep(.05)
        raise AssertionError("Collector did not retain disconnected observations")

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(10)
        config = {"managed": True, "receiver": "127.0.0.1", "port": listener.getsockname()[1],
                  "nodeId": node, "spool": str(spool), "enrollmentKey": "hv1." + secret + "." + pin}
        with run(config):
            with accept(listener) as stream:
                assert exchange(stream, 1, [])["appliedRevision"] == 0
                assert exchange(stream, 1, [])["appliedRevision"] == 1
                until_message(stream, 2, targets, lambda m: m.get("appliedRevision") == 2)
                until_message(stream, 2, targets, lambda m: seen == {4, 6})
                until_message(stream, 3, [{"target": ""}], lambda m: m.get("applyError") and
                              m["appliedRevision"] == 2)
                until_message(stream, 3, targets[:1], lambda m: m.get("appliedRevision") == 3)
            await_backlog(3)
        assert log.read_text().count("127.0.0.1: 127.0.0.1 via") == 1, "unchanged target restarted"
        # Simulate an older installation with unacknowledged rows in a rollback-journal spool.
        with contextlib.closing(sqlite3.connect(spool)) as previous:
            assert previous.execute("PRAGMA journal_mode=DELETE").fetchone()[0] == "delete"
            assert previous.execute("SELECT count(*) FROM outbox").fetchone()[0] >= 3
        # Drop queued connection attempts so the next accepted connection belongs to the new process.
        listener.setblocking(False)
        try:
            while True:
                listener.accept()[0].close()
        except BlockingIOError:
            pass
        listener.settimeout(10)
        with run(config):
            with accept(listener) as stream:
                with contextlib.closing(sqlite3.connect(spool)) as current:
                    assert current.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
                backlog = pending()
                await_backlog(backlog + 3)
                assert exchange(stream, 3, targets[:1])["appliedRevision"] == 3
                until_message(stream, 4, [], lambda m: m.get("appliedRevision") == 4)
                while pending():
                    exchange(stream, 4, [])
                time.sleep(.7)
                assert pending() == 0
        # A chain trusted by the system still has to match the pin before any key is sent.
        bad = dict(config, caCertificate=str(cert), enrollmentKey="hv1." + secret + "." + "00" * 32)
        with run(bad):
            try:
                with accept(listener) as stream:
                    assert stream.readline() == b"", "collector sent credentials to a different certificate"
            except (ssl.SSLEOFError, ssl.SSLZeroReturnError):
                pass
            end = time.monotonic() + 2
            while "certificate does not match" not in log.read_text() and time.monotonic() < end:
                time.sleep(.05)
            assert "certificate does not match" in log.read_text()
        assert secret not in log.read_text()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, default=Path("build-node/hopview-node"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        verify(args.binary.resolve(), Path(tmp))
    print("Managed enrollment, pinned TLS, live assignments, retention and offline restart passed.")
