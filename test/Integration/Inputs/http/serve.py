#!/usr/bin/env python3
"""A local HTTP server for the tests of the http device (devices/http.ent,
examples/http/http-SPEC.md, §7.2 and §8.1), and a command run against it.

  serve.py DIR -- command [arguments ...]

Serves on 127.0.0.1, plain and with TLS (cert.pem here, self-signed for
127.0.0.1), writes into DIR what the program reads (`server.url`,
`server_tls.url`, `closed.url`: a port nothing listens on, `cert.pem`,
`binary.bin`: bytes with zeros among them, `big.bin`: 10 MB), runs the
command in DIR and gives its exit status. Endpoints, any method:

  /anything        the method and path, the request's header lines as
                   received, an empty line, its body
  /sum             "length L fnv H" of the request's body (FNV-1a, 64)
  /bytes/N         N bytes (byte i is i * 7 + 3), with X-Fnv and X-Length
  /status/N        status N
  /headers         X-Twice twice (one, two), then X-Once
  /redirect/N      a redirect to /redirect/N-1 (?code=C: of status C,
                   302 else); /redirect/0 says "done METHOD"
  /delay/MS        answers after MS milliseconds
  /cookies/set?k=v sets the cookie k=v; /cookies gives the Cookie header
  /gzip, /brotli   bodies compressed so
  /n/I             "n I"
"""

import gzip
import http.server
import os
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))


def fnv(data):
    """FNV-1a, 64 bits."""
    h = 0xcbf29ce484222325
    prime = 0x100000001b3
    mask = 0xffffffffffffffff
    for b in data:
        h = ((h ^ b) * prime) & mask
    return h


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def __getattr__(self, name):
        if name.startswith("do_"):
            return self.handle_any
        raise AttributeError(name)

    def send(self, status, body=b"", headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        if status not in (204, 304) and not (100 <= status < 200):
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD" and status not in (204, 304):
            self.wfile.write(body)

    def handle_any(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        url = urllib.parse.urlsplit(self.path)
        path, query = url.path, urllib.parse.parse_qs(url.query)
        parts = path.strip("/").split("/")
        if path == "/anything":
            lines = "".join(f"{name}: {value}\r\n"
                            for name, value in self.headers.raw_items())
            self.send(200, f"{self.command} {path}\r\n{lines}\r\n".encode()
                      + body)
        elif path == "/sum":
            self.send(200, f"length {len(body)} fnv {fnv(body)}".encode())
        elif parts[0] == "bytes":
            n = int(parts[1])
            data = bytes((i * 7 + 3) & 255 for i in range(n))
            self.send(200, data, [("X-Fnv", f"{fnv(data)}"),
                                  ("X-Length", str(n))])
        elif parts[0] == "status":
            n = int(parts[1])
            self.send(n, f"status {n}".encode())
        elif path == "/headers":
            self.send(200, b"twice", [("X-Twice", "one"), ("X-Twice", "two"),
                                      ("X-Once", "  spaced  ")])
        elif parts[0] == "redirect":
            n = int(parts[1])
            if n == 0:
                self.send(200, f"done {self.command}".encode())
            else:
                code = int(query.get("code", ["302"])[0])
                more = f"?code={code}" if "code" in query else ""
                self.send(code, b"", [("Location", f"/redirect/{n - 1}{more}")])
        elif parts[0] == "delay":
            time.sleep(int(parts[1]) / 1000)
            self.send(200, b"late")
        elif path == "/cookies/set":
            cookies = [("Set-Cookie", f"{k}={v[0]}; Path=/")
                       for k, v in query.items()]
            self.send(200, b"set", cookies)
        elif path == "/cookies":
            self.send(200, (self.headers.get("Cookie") or "").encode())
        elif path == "/gzip":
            self.send(200, gzip.compress(b"gzipped body, decoded"),
                      [("Content-Encoding", "gzip")])
        elif path == "/brotli":
            with open(os.path.join(HERE, "body.br"), "rb") as f:
                self.send(200, f.read(), [("Content-Encoding", "br")])
        elif parts[0] == "n":
            self.send(200, f"n {parts[1]}".encode())
        else:
            self.send(404, b"not here")


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 1024

    def handle_error(self, request, address):
        # (Connections the client cuts on purpose: a body past its limit,
        # a timeout, a certificate it does not trust.)
        pass


def started(tls):
    server = Server(("127.0.0.1", 0), Handler)
    if tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(os.path.join(HERE, "cert.pem"),
                                os.path.join(HERE, "key.pem"))
        server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main():
    where = sys.argv[1]
    command = sys.argv[sys.argv.index("--") + 1:]
    os.makedirs(where, exist_ok=True)
    plain, tls = started(False), started(True)
    closed = socket.socket()
    closed.bind(("127.0.0.1", 0))
    closed_port = closed.getsockname()[1]
    closed.close()
    files = {
        "server.url": f"http://127.0.0.1:{plain.server_address[1]}",
        "server_tls.url": f"https://127.0.0.1:{tls.server_address[1]}",
        "closed.url": f"http://127.0.0.1:{closed_port}",
    }
    for name, text in files.items():
        with open(os.path.join(where, name), "w") as f:
            f.write(text)
    with open(os.path.join(HERE, "cert.pem"), "rb") as f:
        cert = f.read()
    with open(os.path.join(where, "cert.pem"), "wb") as f:
        f.write(cert)
    with open(os.path.join(where, "binary.bin"), "wb") as f:
        f.write(bytes([0, 1, 2, 0, 255, 0, 10, 13, 0]) * 7)
    with open(os.path.join(where, "big.bin"), "wb") as f:
        f.write(bytes((i * 13 + 1) & 255 for i in range(10 * 1024 * 1024)))
    done = subprocess.run(command, cwd=where)
    plain.shutdown()
    tls.shutdown()
    sys.exit(done.returncode)


if __name__ == "__main__":
    main()
