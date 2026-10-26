#!/usr/bin/env python3
"""Evaluates a JavaScript expression in the first page of a Chromium-based browser started with
--remote-debugging-port, using only the standard library (a minimal WebSocket client for the
DevTools protocol). Used to read YouTube's "stats for nerds" during device tests.

Usage: cdp-eval.py PORT EXPRESSION
"""
import base64
import json
import os
import socket
import struct
import sys
import urllib.request


def ws_connect(url):
    rest = url[len("ws://"):]
    hostport, path = rest.split("/", 1)
    host, port = hostport.split(":")
    sock = socket.create_connection((host, int(port)), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(
        f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode()
    )
    response = b""
    while b"\r\n\r\n" not in response:
        response += sock.recv(4096)
    if b" 101 " not in response.split(b"\r\n")[0]:
        raise RuntimeError(response.split(b"\r\n")[0].decode())
    return sock


def ws_send(sock, text):
    data = text.encode()
    header = bytearray([0x81])
    if len(data) < 126:
        header.append(0x80 | len(data))
    elif len(data) < 65536:
        header.append(0x80 | 126)
        header += struct.pack(">H", len(data))
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", len(data))
    mask = os.urandom(4)
    sock.sendall(bytes(header) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def ws_recv(sock):
    first, second = recv_exact(sock, 2)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    return recv_exact(sock, length).decode(errors="replace")


def main():
    port, expression = sys.argv[1], sys.argv[2]
    targets = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
    page = next(t for t in targets if t.get("type") == "page")
    sock = ws_connect(page["webSocketDebuggerUrl"])
    ws_send(sock, json.dumps({"id": 1, "method": "Runtime.evaluate",
                              "params": {"expression": expression, "returnByValue": True}}))
    while True:
        message = json.loads(ws_recv(sock))
        if message.get("id") == 1:
            result = message.get("result", {}).get("result", {})
            print(result.get("value", result))
            return


if __name__ == "__main__":
    main()
