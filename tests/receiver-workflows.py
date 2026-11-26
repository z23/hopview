#!/usr/bin/env python3
"""Exercise a managed PoP, delayed uploads, remote diagnostics, health and read-only HTTPS."""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, closing, contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import select
import socket
from socketserver import BaseRequestHandler, ThreadingTCPServer
import sqlite3
import ssl
import subprocess
import sys
import tempfile
from threading import Thread
import time
import urllib.error
import urllib.request
import uuid

sys.dont_write_bytecode = True
from telemetry import scalar, until


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@contextmanager
def serving(server):
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def verify(binary, work):
    cert, private = work / "cert.pem", work / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1",
                    "-keyout", private, "-out", cert], capture_output=True, check=True)
    ctx = ssl.create_default_context(cafile=str(cert))
    reader = secrets.token_hex(32)
    (work / "viewer-token").write_text(reader)
    secret = secrets.token_hex(32)
    pin = hashlib.sha256(ssl.PEM_cert_to_DER_cert(cert.read_text())).hexdigest()
    database, spool = work / "history.sqlite", work / "queue.sqlite"
    port, viewer_port = free_port(), free_port()
    receiver = work / "receiver.json"
    receiver.write_text(json.dumps(dict(listen="127.0.0.1", port=port, database=str(database),
                                        certificate=str(cert), privateKey=str(private),
                                        viewer=dict(port=viewer_port, tokenFile="viewer-token"))))

    def http(path, token=reader, method="GET", with_headers=False):
        headers = {}
        if token is not None:
            headers["Authorization"] = "Basic " + base64.b64encode(("reader:" + token).encode()).decode()
        request = urllib.request.Request(f"https://127.0.0.1:{viewer_port}" + path, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, context=ctx, timeout=6) as response:
                return (response.status, response.read(), response.headers) if with_headers else (response.status, response.read())
        except urllib.error.HTTPError as error:
            return (error.code, error.read(), error.headers) if with_headers else (error.code, error.read())

    class Delay(BaseRequestHandler):
        def handle(self):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=1) as upstream:
                    upstream.settimeout(5)
                    peers = {self.request: upstream, upstream: self.request}
                    while True:
                        ready, _, _ = select.select(list(peers), [], [], 3)
                        for source in ready:
                            chunk = source.recv(65536)
                            if not chunk:
                                return
                            time.sleep(.05)
                            peers[source].sendall(chunk)
            except OSError:
                return

    class Service(BaseHTTPRequestHandler):
        def do_HEAD(self):
            self.send_response(204)
            self.end_headers()

        def log_message(self, *args):
            pass

    with ExitStack() as stack:
        def start(mode, config):
            output = stack.enter_context((work / (mode + ".log")).open("ab"))
            proc = subprocess.Popen([binary, mode, config], stdout=output, stderr=output)
            def stop():
                if proc.poll() is None:
                    proc.terminate()
                status = proc.wait(timeout=10)
                if status != 0:
                    raise AssertionError(f"hopview-node {mode} (pid {proc.pid}) exited with status {status}")
            stack.callback(stop)
            return stop

        proxy = ThreadingTCPServer(("127.0.0.1", 0), Delay)
        proxy.daemon_threads = True
        proxy_port = stack.enter_context(serving(proxy))
        service_port = stack.enter_context(serving(ThreadingHTTPServer(("127.0.0.1", 0), Service)))
        stop_receiver = start("receive", receiver)
        until(lambda: scalar(database, "SELECT count(*) FROM managed_collectors") == 0, "receiver started")
        assignments = [dict(target=f"127.0.0.{i}", family=4, protocol="tcp", port=9, intervalMs=1000)
                       for i in range(1, 21)]
        assignments[0]["diagnostics"] = dict(interface="lo", seconds=5, url=f"http://127.0.0.1:{service_port}/")
        assignments.append(dict(target="127.0.0.21", family=4, interface="no-such-device", protocol="icmp"))
        with closing(sqlite3.connect(database)) as db:
            db.execute("INSERT INTO managed_collectors(key_digest,targets) VALUES(?,?)",
                       (hashlib.sha256(secret.encode()).hexdigest(), json.dumps(assignments)))
            private_session = db.execute("INSERT INTO sessions(target,family,interval_ms,created_ms) "
                                         "VALUES('desktop-only',4,1000,0)").lastrowid
            db.commit()
        collector = work / "collector.json"
        collector.write_text(json.dumps(dict(managed=True, receiver="127.0.0.1", port=proxy_port,
            nodeId=str(uuid.uuid4()), spool=str(spool), enrollmentKey="hv1." + secret + "." + pin)))
        start("collect", collector)
        until(lambda: scalar(database, "SELECT count(*) FROM receiver_streams") == 20, "20 live targets")
        until(lambda: scalar(database, "SELECT count(DISTINCT kind) FROM diagnostic_records") == 2, "both diagnostic tracks")
        until(lambda: scalar(database, "SELECT count(*) FROM diagnostic_records WHERE json_extract(data,'$.status')=204"), "service reached")
        until(lambda: scalar(database, "SELECT count(*) FROM collector_health WHERE report LIKE '%Unknown network interface%'") == 1,
              "health includes failed target setup")
        stop_receiver()
        with closing(sqlite3.connect(f"file:{spool}?mode=ro", uri=True)) as inspector:
            inspector.execute("BEGIN")
            queued = inspector.execute("SELECT count(*) FROM outbox").fetchone()[0]
            until(lambda: scalar(spool, "SELECT count(*) FROM outbox") >= queued + 20,
                  "collector continued writing while a reader inspected the outbox")
        until(lambda: scalar(spool, "SELECT count(*) FROM outbox") >= 70, "disconnected rounds spooled")
        through_id = scalar(spool, "SELECT max(id) FROM outbox")
        cutoff = int(time.time() * 1000)
        start("receive", receiver)
        until(lambda: scalar(spool, "SELECT count(*) FROM outbox WHERE id<=?", (through_id,)) == 0,
              "earlier observations uploaded over a 100 ms link")
        until(lambda: scalar(database, "SELECT count(DISTINCT session) FROM samples WHERE hop=0 AND t>?", (cutoff,)) == 20,
              "collection continued through backfill")
        until(lambda: scalar(spool, "SELECT count(*) FROM outbox") < 15, "backlog drained at 20 rounds per second")
        assert http("/", token="bad")[0] == 401
        assert http("/", method="POST")[0] == 405
        assert http("/api/view?session=1&from=0&through=9999999999999")[0] == 400
        status_root, _, headers_root = http("/", with_headers=True)
        assert status_root == 200 and "max-age=" in headers_root.get("Strict-Transport-Security", "")
        assert http("/app.js")[0] == 200
        sessions = json.loads(http("/api/sessions")[1])
        assert len(sessions) == 20 and all(s["id"] != private_session for s in sessions)
        assert http(f"/api/view?session={private_session}&from=0&through=1")[0] == 404
        session = sessions[0]
        assert session["effectiveProtocol"] == "tcp" and session["effectiveFamily"] == 4
        path = f"/api/view?session={session['id']}&from={session['startMs']}&through={cutoff}"
        with ThreadPoolExecutor(max_workers=2) as desks:
            first, second = list(desks.map(lambda _: json.loads(http(path)[1]), range(2)))
        assert first == second and first["rows"] and first["charts"][0]["buckets"]
        assert first["measurement"]["effectiveProtocol"] == "tcp" and first["endpoint"]["sent"] > 0
        health = json.loads(scalar(database, "SELECT report FROM collector_health"))
        assert health["lastUploadMs"] and health["lastProbeMs"] and health["droppedBatches"] == 0
        # Verify per-IP authentication failure rate limiting returns 429 with Retry-After.
        for _ in range(10):
            assert http("/", token="wrong")[0] == 401
        limited_code, _, limited_headers = http("/", token="wrong", with_headers=True)
        assert limited_code == 429 and int(limited_headers.get("Retry-After", "0")) >= 1
        # The right password is let through while the limit is active, and clears it.
        assert http("/")[0] == 200
        assert http("/", token="wrong")[0] == 401
        # A request without credentials is every browser's first step. It is refused but not counted.
        for _ in range(12):
            assert http("/", token=None)[0] == 401
        assert http("/", token="wrong")[0] == 401
        assert http("/")[0] == 200


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("node", nargs="?", type=Path, default=Path("build-node/hopview-node"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        try:
            verify(args.node.resolve(), work)
        except Exception:
            for log in work.glob("*.log"):
                print(log.name, log.read_text()[-1500:])
            raise
    print("20-target WAN backfill, diagnostic tracks, health and independent HTTPS readers passed.")
