#!/usr/bin/env python3
"""Check independent endpoint and TTL observations through the shipped executable."""
import json
from pathlib import Path
import subprocess
import sys


def report(binary, host, family, *options):
    return subprocess.run([binary, "--report", host, "--family", family, "--seconds", "1",
                           "--interval", "250", *options], capture_output=True, text=True, timeout=12)


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve()
    for host, family, method in [("127.0.0.1", "4", "auto"), ("::1", "6", "icmp"),
                                 ("localhost", "4", "icmp")]:
        result = report(binary, host, family, "--protocol", method, "--payload-bytes", "32")
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
    print("IPv4 and IPv6 endpoint and route observations passed.")
