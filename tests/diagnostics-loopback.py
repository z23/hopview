#!/usr/bin/env python3
"""Run GUI/worker and dual-stack HTTPS timing checks against private loopback servers."""
import argparse
import contextlib
import http.server
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass  # The untrusted-certificate check closes its test connection early.

    def do_HEAD(self):
        time.sleep(0.035)
        self.send_response(503 if self.path == "/failure" else 200)
        self.end_headers()

    def log_message(self, *args):
        pass


class IPv6Server(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6


@contextlib.contextmanager
def server(ip, tls=None):
    cls = IPv6Server if ":" in ip else http.server.ThreadingHTTPServer
    with cls((ip, 0), Handler) as listener:
        if tls:
            listener.socket = tls.wrap_socket(listener.socket, server_side=True)
        thread = threading.Thread(target=listener.serve_forever)
        thread.start()
        try:
            yield listener.server_port
        finally:
            listener.shutdown()
            thread.join()


def run(executable):
    with tempfile.TemporaryDirectory() as work:
        ca, key = (Path(work) / name for name in ("ca.pem", "server.key"))
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-keyout", str(key), "-out", str(ca), "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost"], check=True, capture_output=True)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(ca, key)
        with server("127.0.0.1", tls) as v4, server("::1", tls) as v6, server("127.0.0.1") as plain:
            env = dict(os.environ, HOPVIEW_DIAG_CA=str(ca), HOPVIEW_DIAG_TLS=f"https://localhost:{v4}/",
                       HOPVIEW_DIAG_TLS6=f"https://localhost:{v6}/", HOPVIEW_DIAG_HTTP=f"http://127.0.0.1:{plain}/",
                       QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="generic", QT_QUICK_BACKEND="software")
            subprocess.run([executable, "diagnosticLoopback"], env=env, check=True, timeout=30)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", default="build-tests/integration/integration_test")
    run(parser.parse_args().executable)
