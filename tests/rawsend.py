#!/usr/bin/env python3
"""Sends a raw byte sequence to the server and prints whatever comes back.

The test suite needs to send deliberately malformed requests that curl would
refuse to construct. netcat cannot be used for this: on a keep-alive response
the server does not close the connection, and nc then blocks indefinitely
because its -w flag governs the connect timeout rather than the idle read.
This helper applies a real socket timeout and always returns.

Usage: rawsend.py HOST PORT [--timeout SECONDS]
       The raw request is read from stdin, with \\r and \\n escapes expanded.
"""

import socket
import sys


def main() -> int:
    if len(sys.argv) < 3:
        sys.stderr.write("usage: rawsend.py HOST PORT [--timeout SECONDS]\n")
        return 2

    host = sys.argv[1]
    port = int(sys.argv[2])
    timeout = 3.0
    if "--timeout" in sys.argv:
        timeout = float(sys.argv[sys.argv.index("--timeout") + 1])

    payload = sys.stdin.buffer.read()

    try:
        connection = socket.create_connection((host, port), timeout=timeout)
    except OSError as error:
        sys.stderr.write("connect failed: {}\n".format(error))
        return 1

    received = bytearray()
    try:
        connection.sendall(payload)
        connection.settimeout(timeout)
        while True:
            chunk = connection.recv(65536)
            if not chunk:
                break
            received.extend(chunk)
    except socket.timeout:
        # Expected whenever the server keeps the connection open for reuse:
        # everything it intended to send has already arrived.
        pass
    except OSError:
        pass
    finally:
        connection.close()

    sys.stdout.buffer.write(bytes(received))
    return 0


if __name__ == "__main__":
    sys.exit(main())
