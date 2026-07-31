#!/usr/bin/env python3
"""Returns a status code chosen by the query string, e.g. /cgi-bin/status.py?code=418.

Demonstrates that the server honours a script's "Status:" header rather than
always answering 200.
"""

import os
import sys

query = os.environ.get("QUERY_STRING", "")
code = 200
for pair in query.split("&"):
    if pair.startswith("code="):
        try:
            candidate = int(pair[5:])
            if 100 <= candidate <= 599:
                code = candidate
        except ValueError:
            pass

sys.stdout.write("Status: {} Chosen By Script\r\n".format(code))
sys.stdout.write("Content-Type: text/plain; charset=utf-8\r\n")
sys.stdout.write("\r\n")
sys.stdout.write("the script asked for {}\n".format(code))
