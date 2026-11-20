#!/usr/bin/env python3
"""A local WebSocket server for the tests of the websocket device
(devices/websocket.ent), and a command run against it.

  serve.py DIR -- command [arguments ...]

Serves on 127.0.0.1, plain and with TLS (Inputs/http/cert.pem,
self-signed for 127.0.0.1), writes into DIR what the program reads
(`server.url`, `server_tls.url`: ws:// and wss://, `closed.url`: a port
nothing listens on, `cert.pem`), runs the command in DIR and gives its
exit status. Python's standard library only.

  /echo     upgrades (answering a Set-Cookie: sid=1), says "hello", then
            echoes each message, but these texts:
              frag     "frag-a" "frag-b" as one message in two frames
              headers  "x-test=<X-Test> cookie=<Cookie>" of the upgrade
              ping     a ping "p1"; once its pong comes, "pong p1"
              big      70000 "x"
              close    a close, 4000 "bye"
  /refuse   answers 401
"""

import base64
import hashlib
import os
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
CERTS = os.path.join(HERE, "..", "http")
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def read_exact(f, n):
    out = b""
    while len(out) < n:
        got = f.read(n - len(out))
        if not got:
            raise EOFError
        out += got
    return out


def read_frame(f):
    b0, b1 = read_exact(f, 2)
    fin, opcode, masked, length = b0 & 0x80, b0 & 0x0F, b1 & 0x80, b1 & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(f, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(f, 8))[0]
    mask = read_exact(f, 4) if masked else b"\0\0\0\0"
    data = bytes(c ^ mask[i % 4] for i, c in enumerate(read_exact(f, length)))
    return fin, opcode, masked, data


def frame(opcode, data, fin=True):
    head = bytes([(0x80 if fin else 0) | opcode])
    n = len(data)
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    return head + data


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        lines = []
        while True:
            line = self.rfile.readline()
            if not line or line in (b"\r\n", b"\n"):
                break
            lines.append(line.decode().rstrip("\r\n"))
        if not lines:
            return
        path = lines[0].split(" ")[1].split("?")[0]
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(":")
            headers[name.strip().lower()] = value.strip()
        if path != "/echo" or headers.get("upgrade", "").lower() != "websocket":
            self.wfile.write(b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 2\r\n"
                             b"Connection: close\r\n\r\nno")
            return
        accept = base64.b64encode(hashlib.sha1(
            (headers["sec-websocket-key"] + GUID).encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                          "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept +
                          "\r\nSet-Cookie: sid=1; Path=/\r\n\r\n").encode())
        send = self.wfile.write
        send(frame(1, b"hello"))
        while True:
            try:
                fin, opcode, masked, data = read_frame(self.rfile)
            except (EOFError, OSError, ValueError):
                return
            if not masked:
                send(frame(8, struct.pack(">H", 1002)))
                return
            if opcode == 8:
                send(frame(8, data))
                return
            if opcode == 10:
                send(frame(1, b"pong " + data))
            elif opcode == 1 and data == b"frag":
                send(frame(1, b"frag-a", fin=False) + frame(0, b"frag-b"))
            elif opcode == 1 and data == b"headers":
                send(frame(1, ("x-test=" + headers.get("x-test", "") + " cookie=" +
                               headers.get("cookie", "")).encode()))
            elif opcode == 1 and data == b"ping":
                send(frame(9, b"p1"))
            elif opcode == 1 and data == b"big":
                send(frame(1, b"x" * 70000))
            elif opcode == 1 and data == b"close":
                send(frame(8, struct.pack(">H", 4000) + b"bye"))
                try:
                    read_frame(self.rfile)
                except (EOFError, OSError, ValueError):
                    pass
                return
            elif opcode in (1, 2):
                send(frame(opcode, data))


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, address):
        pass


def started(tls):
    server = Server(("127.0.0.1", 0), Handler)
    if tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(os.path.join(CERTS, "cert.pem"), os.path.join(CERTS, "key.pem"))
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
        "server.url": f"ws://127.0.0.1:{plain.server_address[1]}",
        "server_tls.url": f"wss://127.0.0.1:{tls.server_address[1]}",
        "closed.url": f"ws://127.0.0.1:{closed_port}",
    }
    for name, text in files.items():
        with open(os.path.join(where, name), "w") as f:
            f.write(text)
    with open(os.path.join(CERTS, "cert.pem"), "rb") as f:
        cert = f.read()
    with open(os.path.join(where, "cert.pem"), "wb") as f:
        f.write(cert)
    done = subprocess.run(command, cwd=where)
    plain.shutdown()
    tls.shutdown()
    sys.exit(done.returncode)


if __name__ == "__main__":
    main()
