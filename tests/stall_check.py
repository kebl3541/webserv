#!/usr/bin/env python3
"""Checks that a client which stops reading is eventually reaped.

A client that requests a large response and then never reads it leaves the
server holding bytes it cannot deliver. If the idle sweep refreshes that
connection's deadline every time it looks at it, the connection is never closed
and the slot is held for as long as the peer cares to keep the socket open.
That is a connection leak an attacker can open at will, and it costs them
nothing but an idle socket.

Prints "opened=N leftover=M". A leftover of zero means the descriptors came
back, which is the property under test.

Usage: stall_check.py SERVER_PID PORT PATH [--timeout SECONDS]
"""

import socket
import subprocess
import sys
import time


def descriptors(pid):
    return int(subprocess.check_output(
        "lsof -p %s 2>/dev/null | wc -l" % pid, shell=True).strip())


def main():
    if len(sys.argv) < 4:
        sys.stderr.write("usage: stall_check.py SERVER_PID PORT PATH\n")
        return 2

    pid = sys.argv[1]
    port = int(sys.argv[2])
    path = sys.argv[3]
    limit = 90.0
    if "--timeout" in sys.argv:
        limit = float(sys.argv[sys.argv.index("--timeout") + 1])

    baseline = descriptors(pid)
    held = []
    request = ("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode()

    for _ in range(5):
        connection = socket.create_connection(("127.0.0.1", port), timeout=5)
        connection.sendall(request)
        held.append(connection)          # deliberately never read

    time.sleep(2)
    opened = descriptors(pid) - baseline

    deadline = time.time() + limit
    while time.time() < deadline and descriptors(pid) > baseline:
        time.sleep(2)

    leftover = descriptors(pid) - baseline
    for connection in held:
        connection.close()

    print("opened=%d leftover=%d" % (opened, leftover))
    return 0


if __name__ == "__main__":
    sys.exit(main())
