#!/usr/bin/env python3
"""Emits two Set-Cookie headers.

Set-Cookie is the header that may not be merged into a single comma-separated
value the way most repeated headers can, so a server that stores response
headers in a map keyed by name silently drops all but the last one. This script
exists to prove that does not happen.
"""

import sys

sys.stdout.write("Content-Type: text/plain; charset=utf-8\r\n")
sys.stdout.write("Set-Cookie: first=one; Path=/\r\n")
sys.stdout.write("Set-Cookie: second=two; Path=/\r\n")
sys.stdout.write("\r\n")
sys.stdout.write("two cookies were set\n")
