#!/usr/bin/env python3
"""UDP log listener for Casquinha's network log mirror (b34+).

macOS `nc -kul` silently "connects" to the first datagram's peer and drops
everything after it — the "exactly one line ever arrives" trap (b38 proved
the OS 9 side sent 28/28 OK). This one just binds and prints, forever.

    python3 tools/loglisten.py [port]        # default 5514
"""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5514
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
print(f"listening for Casquinha logs on UDP :{port} (^C to stop)", flush=True)
while True:
    data, addr = s.recvfrom(4096)
    line = data.decode("mac_roman", errors="replace")
    if not line.endswith("\n"):
        line += "\n"
    print(line, end="", flush=True)
