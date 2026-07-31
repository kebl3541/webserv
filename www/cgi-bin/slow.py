#!/usr/bin/env python3
"""Sleeps far longer than the CGI timeout, to prove the server kills it and
answers 504 instead of hanging the event loop."""

import sys
import time

time.sleep(120)

sys.stdout.write("Content-Type: text/plain\r\n")
sys.stdout.write("\r\n")
sys.stdout.write("this should never be reached\n")
