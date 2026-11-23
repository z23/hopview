#!/usr/bin/env python3
"""Open a valid and a truncated incident with the packaged executable, offline."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time


def verify(binary, work):
    source = work / "interval.hopline"
    env = dict(os.environ, TMPDIR=str(work), XDG_DATA_HOME=str(work / "data"),
               XDG_CACHE_HOME=str(work / "cache"), XDG_CONFIG_HOME=str(work / "config"),
               QT_QPA_PLATFORM="minimal", QT_QPA_PLATFORMTHEME="generic",
               QT_DEBUG_BACKINGSTORE="1", QT_QUICK_BACKEND="software")
    command = [str(binary), "--replay", str(source)]
    records = [
        {"kind": "hopline", "version": 1, "from": 1000, "through": 1250,
         "target": "offline.invalid", "family": 6, "intervalMs": 250, "protocol": "tcp",
         "port": 443, "interfaceName": ""},
        {"kind": "probe", "t": 1000, "hop": 1, "address": "::1", "rtt": 12, "destination": True},
        {"kind": "probe", "t": 1250, "hop": 1, "address": "", "rtt": None, "destination": False},
        {"kind": "note", "t": 1250, "text": "reply missing"},
    ]
    source.write_text("".join(json.dumps(record) + "\n" for record in records))
    invalid = subprocess.run(command, env=env, cwd=work, capture_output=True, timeout=10)
    assert invalid.returncode != 0 and b"closing record" in invalid.stderr, invalid.stderr
    with source.open("a") as file:
        file.write(json.dumps({"kind": "end", "probes": 2, "notes": 1}) + "\n")
    original = source.read_bytes()
    viewer = subprocess.Popen(command, env=env, cwd=work, stderr=subprocess.PIPE)
    try:
        # The minimal Qt platform dumps the actual window backing store on flush.
        deadline = time.monotonic() + 10
        while not any(work.glob("output*.png")):
            assert viewer.poll() is None, viewer.stderr.read()
            if time.monotonic() > deadline:
                raise AssertionError("The replay window did not render a frame")
            time.sleep(.05)
        [recording] = work.glob("*/replay.sqlite")
        time.sleep(.8)  # More than three intervals; replay must never start a collector.
        with sqlite3.connect(f"file:{recording}?mode=ro", uri=True) as db:
            assert db.execute("SELECT target,family,interval_ms FROM sessions").fetchall() == [
                ("offline.invalid", 6, 250)]
            assert db.execute("SELECT t,rtt,dest FROM samples WHERE hop=1 ORDER BY t").fetchall() == [
                (1000, 12., 1), (1250, -1., 0)]
            assert db.execute("SELECT t,body FROM notes").fetchall() == [(1250, "reply missing")]
        assert viewer.poll() is None and source.read_bytes() == original
        assert not list((work / "data").rglob("*.sqlite"))
    finally:
        if viewer.poll() is None:
            viewer.terminate()
        _, errors = viewer.communicate(timeout=5)
    assert b"qrc:/" not in errors, errors


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "build/hopline"
    with tempfile.TemporaryDirectory() as tmp:
        verify(binary, Path(tmp))
    print("Replay CLI checks passed.")
