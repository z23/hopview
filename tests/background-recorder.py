#!/usr/bin/env python3
"""Exercise close, reopen and restart with a real loopback trace and a private history."""
import argparse
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time


def serve_notifications(log):
    import dbus
    import dbus.service
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
    DBusGMainLoop(set_as_default=True)
    interface = "org.freedesktop.Notifications"

    class Notifications(dbus.service.Object):
        @dbus.service.method(interface, in_signature="susssasa{sv}i", out_signature="u")
        def Notify(self, app, replaces, icon, summary, body, actions, hints, timeout):
            with open(log, "a") as out:
                out.write(json.dumps([str(app), str(summary), list(actions)]) + "\n")
            return 1

    bus = dbus.SessionBus()
    service = Notifications(dbus.service.BusName(interface, bus), "/org/freedesktop/Notifications")
    print("ready", flush=True)
    GLib.MainLoop().run()


def verify(binary, work, private_bus=None):
    db = work / "capture.db"
    env = dict(os.environ, QT_QUICK_BACKEND="software")
    if sys.platform.startswith("linux"):
        env.update(QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="generic",
                   XDG_RUNTIME_DIR=str(work), XDG_CONFIG_HOME=str(work / "config"),
                   XDG_CACHE_HOME=str(work / "cache"),
                   DBUS_SESSION_BUS_ADDRESS=private_bus or f"unix:path={work}/unused")
    command = [binary, "--database", str(db)]
    target = ["--family", "4", "--interval", "250", "127.0.0.1"]

    def run(*args):
        subprocess.run(command + list(args), env=env, check=True, timeout=8)

    def count(sql):
        with closing(sqlite3.connect(db)) as con:
            return con.execute(sql).fetchone()[0]

    def wait_for_rounds(minimum):
        for _ in range(100):
            if owner.poll() is not None:
                raise AssertionError(f"collector exited with status {owner.returncode}")
            try:
                if count("SELECT count(*) FROM samples WHERE hop=0 AND rtt>=0") >= minimum:
                    return
            except sqlite3.OperationalError:
                pass  # The initial schema may still be opening.
            time.sleep(.1)
        raise AssertionError("collector failed to append loopback rounds")

    def wait_for_exit():
        # --quit waits for the database lock to be released. The owner might have
        # exited before we start waiting, so query its actual exit status directly.
        deadline = time.monotonic() + 5
        while owner.poll() is None and time.monotonic() < deadline:
            time.sleep(.05)
        if owner.returncode is None:
            if sys.platform == "darwin":
                # Keep a thread trace in CI when Cocoa or a worker stalls at teardown.
                trace = work / "shutdown-sample.txt"
                subprocess.run(["/usr/bin/sample", str(owner.pid), "1", "10", "-file", str(trace)],
                               timeout=10, check=False)
                if trace.is_file():
                    print(trace.read_text(errors="replace")[:24000], file=sys.stderr, flush=True)
            raise AssertionError(f"recorder {owner.pid} was still running 5 seconds after shutdown")
        assert owner.returncode == 0, f"recorder exited with status {owner.returncode}"

    owner = subprocess.Popen(command + target, env=env)
    try:
        wait_for_rounds(3)
        run("--background")  # Enable background recording and close the window.
        hidden = count("SELECT count(*) FROM samples WHERE hop=0")
        wait_for_rounds(hidden + 5)
        if private_bus:
            # The GUI controls are exercised in Integration::alertControls. Set a threshold
            # below loopback RTT, then above it, so the real recorder produces both transitions.
            with closing(sqlite3.connect(db)) as con, con:
                con.execute("INSERT INTO alert_rules(session,latency,loss,seconds) VALUES(1,0.0000001,0,1)")
            def wait_notification(expected):
                log = work / "notices.jsonl"
                for _ in range(100):
                    if log.exists():
                        notices = [json.loads(line) for line in log.read_text().splitlines()]
                        if any(name == "Hopview" and expected in summary and "default" in actions
                               for name, summary, actions in notices):
                            return
                    time.sleep(.1)
                raise AssertionError(f"notification service did not receive {expected}")
            wait_notification("High latency")
            with closing(sqlite3.connect(db)) as con, con:
                con.execute("UPDATE alert_rules SET latency=60000 WHERE session=1")
            wait_notification("Recovered")
            assert count("SELECT count(*) FROM alert_events") == 2
        run(*target)  # The launcher forwards and exits while the same owner collects.
        visible = count("SELECT count(*) FROM samples WHERE hop=0")
        wait_for_rounds(visible + 4)
        assert count("SELECT count(*) FROM sessions") == 1
        run("--quit")
        wait_for_exit()
        stopped = count("SELECT count(*) FROM samples WHERE hop=0")
        owner = subprocess.Popen(command + ["--background"], env=env)
        wait_for_rounds(stopped + 3)
        run()
        assert count("SELECT count(*) FROM sessions") == 1
        if sys.platform == "win32":
            run("--quit")  # Popen.terminate() forcibly ends a Windows process.
        else:
            owner.terminate()
        wait_for_exit()
        assert count("PRAGMA integrity_check") == "ok"
    finally:
        if owner.poll() is None:
            owner.kill()
            owner.wait()


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--serve":
        serve_notifications(sys.argv[2])
        sys.exit(0)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default="build/hopview")
    parser.add_argument("--notifications", action="store_true",
                        help="Also exercise a private D-Bus daemon and notification service (dbus-python, PyGObject).")
    args = parser.parse_args()
    if args.notifications and not sys.platform.startswith("linux"):
        parser.error("--notifications runs the Linux desktop notification fixture")
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix="hopview-recorder-") as tmp:
        work = Path(tmp)
        if args.notifications:
            address = f"unix:path={work}/bus"
            with subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--nopidfile",
                                   "--print-address=1", "--address=" + address],
                                  stdout=subprocess.PIPE, text=True) as bus:
                try:
                    assert bus.stdout.readline().strip().startswith(address)
                    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address)
                    with subprocess.Popen([sys.executable, __file__, "--serve", str(work / "notices.jsonl")],
                                          stdout=subprocess.PIPE, env=env, text=True) as service:
                        try:
                            assert service.stdout.readline().strip() == "ready"
                            verify(binary, work, address)
                        finally:
                            service.terminate()
                finally:
                    bus.terminate()
        else:
            verify(binary, work)
    print("Recorder checks passed" + (" including alert and recovery notification delivery." if args.notifications else "."))
