#!/usr/bin/env python3
"""Exercise the installed join workflow in a disposable mount namespace."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import secrets
import select
import shutil
import stat
import subprocess
import tempfile
import termios
import time
import uuid


def inside():
    endpoint, key = "[::1]:9347", "hv1." + secrets.token_hex(32)
    command = ["/hopview-node", "join", endpoint]
    config = Path("/etc/hopview/collector.json")

    def run(secret):
        return subprocess.run(command, input=secret + "\n", text=True, capture_output=True, timeout=6)

    default = config.read_bytes()
    assert run("truncated").returncode != 0 and config.read_bytes() == default
    local = dict(json.loads(default), site="owned-config")
    config.write_text(json.dumps(local))
    assert run(key).returncode != 0 and json.loads(config.read_text()) == local
    config.write_bytes(default)

    master, slave = pty.openpty()
    def controlling_tty():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    proc = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave, preexec_fn=controlling_tty)
    transcript = b""
    try:
        end = time.monotonic() + 6
        while b"enrollment key:" not in transcript:
            if time.monotonic() > end:
                raise AssertionError(f"Missing key prompt: {transcript!r}")
            if select.select([master], [], [], .2)[0]:
                transcript += os.read(master, 4096)
        os.write(master, key.encode() + b"\n")
        assert proc.wait(timeout=6) == 0
        while select.select([master], [], [], .1)[0]:
            transcript += os.read(master, 4096)
        assert key.encode() not in transcript, "Enrollment key appeared in terminal output"
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(slave)
        os.close(master)
    settings = json.loads(config.read_text())
    assert settings["managed"] and settings["receiver"] == "::1" and settings["port"] == 9347
    assert settings["enrollmentKey"] == key and uuid.UUID(settings["nodeId"]).version == 4
    assert stat.S_IMODE(config.stat().st_mode) == 0o640 and config.stat().st_gid == 0, config.stat()
    assert run(key).returncode == 0 and json.loads(config.read_text()) == settings
    assert run("hv1." + secrets.token_hex(32)).returncode != 0
    assert json.loads(config.read_text()) == settings
    calls = Path("/tmp/service-calls").read_text().splitlines()
    assert calls == ["enable --now hopview-collector.service"] * 2
    print("CLI enrollment, protected prompt and files, service activation and idempotent retry passed.")


def verify(binary, work):
    (work / "group").write_text("root:x:0:\nhopview:x:0:\n")
    service = work / "systemctl"
    service.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\" >> /tmp/service-calls\n")
    service.chmod(0o755)
    config = work / "config"
    config.mkdir()
    shutil.copyfile(Path(__file__).resolve().parents[1] / "deploy/collector.example.json", config / "collector.json")
    command = ["bwrap", "--unshare-user", "--uid", "0", "--gid", "0", "--cap-add", "ALL",
               "--unshare-pid", "--unshare-net", "--die-with-parent", "--ro-bind", "/usr", "/usr"]
    for dirname in ("bin", "lib", "lib64"):
        path = Path("/") / dirname
        if path.is_symlink():
            command.extend(["--symlink", os.readlink(path), "/" + dirname])
        elif path.exists():
            command.extend(["--ro-bind", str(path), str(path)])
    subprocess.run(command + ["--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
                              "--dir", "/etc", "--ro-bind", str(work / "group"), "/etc/group",
                              "--bind", str(config), "/etc/hopview",
                              "--ro-bind", str(service), "/usr/bin/systemctl",
                              "--ro-bind", str(binary), "/hopview-node",
                              "--ro-bind", str(Path(__file__).resolve()), "/test.py",
                              "/usr/bin/python3", "/test.py", "--inside"], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("binary", nargs="?", type=Path, default=Path("build-node/hopview-node"))
    args = parser.parse_args()
    if args.inside:
        if not Path("/hopview-node").is_file() or not Path("/test.py").is_file():
            raise SystemExit("Run the fixture from the project checkout")
        inside()
    else:
        with tempfile.TemporaryDirectory() as tmp:
            verify(args.binary.resolve(), Path(tmp))
