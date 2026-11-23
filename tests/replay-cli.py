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


def verify(binary, work, grouped=False):
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
    if grouped:
        primary = dict(records[0], location="office", resolvedAddress="::1", clockOffsetMs=0,
                       clockErrorMs=-1, lastReceivedMs=0, manualCorrectionMs=0)
        peer = dict(primary, location="branch", clockOffsetMs=8, clockErrorMs=2)
        records = [{"kind": "hopline", "version": 2, "from": 1000, "through": 1250,
                    "traces": [primary, peer], "hops": [0]},
                   *(dict(row, trace=0) for row in records[1:]),
                   dict(records[1], trace=1, rtt=65), dict(records[3], trace=1, text="branch note")]
    source.write_text("".join(json.dumps(record) + "\n" for record in records))
    invalid = subprocess.run(command, env=env, cwd=work, capture_output=True, timeout=10)
    assert invalid.returncode != 0 and b"closing record" in invalid.stderr, invalid.stderr
    with source.open("a") as file:
        file.write(json.dumps({"kind": "end", "probes": 3 if grouped else 2,
                               "notes": 2 if grouped else 1, "diagnostics": 0}) + "\n")
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
            expected = [("offline.invalid", 6, 250)] * (2 if grouped else 1)
            assert db.execute("SELECT target,family,interval_ms FROM sessions").fetchall() == expected
            rows = [(1000, 12., 1), (1250, -1., 0)]
            notes = [(1250, "reply missing")]
            if grouped:
                rows.append((1000, 65., 1))
                notes.append((1250, "branch note"))
                assert db.execute("SELECT location,clock_offset_ms FROM session_origin ORDER BY session").fetchall() == [
                    ("office", 0), ("branch", 8)]
            assert db.execute("SELECT t,rtt,dest FROM samples WHERE hop=1 ORDER BY session,t").fetchall() == rows
            assert db.execute("SELECT t,body FROM notes ORDER BY session,t").fetchall() == notes
            assert db.execute("SELECT count(*) FROM receiver_streams").fetchone() == (0,)
        assert viewer.poll() is None and source.read_bytes() == original
        assert not list((work / "data").rglob("*.sqlite"))
    finally:
        if viewer.poll() is None:
            viewer.terminate()
        _, errors = viewer.communicate(timeout=5)
    assert b"qrc:/" not in errors, errors


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "build/hopline"
    for grouped in (False, True):
        with tempfile.TemporaryDirectory() as tmp:
            verify(binary, Path(tmp), grouped)
    print("Replay CLI checks passed.")
