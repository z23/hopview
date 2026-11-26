#!/usr/bin/env python3
"""Check independent endpoint and TTL observations through the shipped executable."""
import json
import os
from pathlib import Path
import secrets
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
from contextlib import ExitStack, closing


def report(binary, host, family, *options):
    return subprocess.run([binary, "--report", host, "--family", family, "--seconds", "1",
                           "--interval", "250", *options], capture_output=True, text=True, timeout=12)


def collector_capture(binary):
    with tempfile.TemporaryDirectory() as tmp, ExitStack() as stack:
        work = Path(tmp)
        (work / "token").write_text(secrets.token_hex(32))
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            port = reserve.getsockname()[1]

        def start(mode, settings):
            config = work / (mode + ".json")
            config.write_text(json.dumps(dict(settings, port=port, insecureLoopback=True)))
            log = stack.enter_context((work / (mode + ".log")).open("w+"))
            proc = subprocess.Popen([binary, mode, config], stdin=subprocess.PIPE, stdout=log, stderr=log,
                                    env=dict(os.environ, HOPVIEW_PARENT_PIPE="1"))

            def stop():
                proc.stdin.close()
                if sys.platform != "win32" and proc.poll() is None:
                    proc.terminate()
                try:
                    code = proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    raise
                log.seek(0)
                assert code == 0, log.read()
            stack.callback(stop)

        database = work / "history.sqlite"
        start("receive", dict(listen="127.0.0.1", database=str(database), sites={"desktop": "token"}))
        targets = [dict(target=host, family=family, protocol="icmp", intervalMs=250)
                   for host, family in [("127.0.0.1", 4), ("::1", 6)]]
        start("collect", dict(site="desktop", receiver="127.0.0.1", tokenFile="token",
                              spool="queue.sqlite", targets=targets))
        for _ in range(100):
            try:
                with closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as db:
                    rows = db.execute("SELECT s.target,p.hop,count(*) FROM samples p "
                                      "JOIN sessions s ON s.id=p.session WHERE p.rtt>=0 AND "
                                      "((p.hop=0 AND p.dest=2) OR (p.hop=1 AND p.dest=1)) "
                                      "GROUP BY s.target,p.hop").fetchall()
                if len(rows) == 4 and all(count >= 2 for _, _, count in rows):
                    assert {(host, hop) for host, hop, _ in rows} == {
                        (t["target"], hop) for t in targets for hop in (0, 1)}
                    return
            except sqlite3.OperationalError:
                pass  # Receiver is still opening its database.
            time.sleep(.1)
        raise AssertionError("Missing receiver evidence for both native collector address families")


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve()
    # The 12000-byte probes exceed the 8 KB queues that macOS gives a new ICMP datagram socket.
    for host, family, method, payload in [("127.0.0.1", "4", "auto", "32"), ("::1", "6", "icmp", "32"),
                                          ("localhost", "4", "icmp", "32"),
                                          ("127.0.0.1", "4", "icmp", "12000"), ("::1", "6", "icmp", "12000")]:
        result = report(binary, host, family, "--protocol", method, "--payload-bytes", payload)
        assert result.returncode == 0, (result.stderr, result.stdout)
        data = json.loads(result.stdout)
        assert data["effectiveProtocol"] == "icmp" and data["effectivePort"] == 0, data
        assert data["measurement"]["effectiveFamily"] == int(family), data
        assert data["endpoint"]["received"] == data["endpoint"]["sent"] >= 2, data
        assert len(data["rows"]) == 1 and data["rows"][0]["hop"] == 1, data
        assert data["rows"][0]["destination"] and data["rows"][0]["loss"] == 0, data
    if sys.platform in ("darwin", "win32"):
        for host, family, options, reason in [
            ("127.0.0.1", "4", ["--protocol", "tcp"], "ICMP"),
            ("::1", "6", ["--dont-fragment"], "IPv4"),
        ]:
            result = report(binary, host, family, *options)
            assert result.returncode != 0 and reason in (result.stderr + result.stdout), result
    if len(sys.argv) > 2:
        collector_capture(Path(sys.argv[2]).resolve())
    print("IPv4 and IPv6 endpoint and route observations passed.")
