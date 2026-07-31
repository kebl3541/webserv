#!/usr/bin/env python3
"""Reads the request body from stdin and echoes it back.

Used to prove that the server streams a POST body into the script correctly,
including a body that arrived chunked and was reassembled before being handed
over.
"""

import os
import sys

length = os.environ.get("CONTENT_LENGTH", "")
try:
    expected = int(length)
except ValueError:
    expected = 0

body = sys.stdin.buffer.read(expected) if expected > 0 else b""

sys.stdout.write("Content-Type: text/plain; charset=utf-8\r\n")
sys.stdout.write("\r\n")
sys.stdout.flush()
sys.stdout.buffer.write(b"received " + str(len(body)).encode() + b" bytes\n")
sys.stdout.buffer.write(body)
