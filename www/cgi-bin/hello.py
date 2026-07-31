#!/usr/bin/env python3
"""A minimal CGI script: headers, a blank line, then the body."""

import os
import sys

body = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CGI</title>
<style>
  body {{ margin:0; min-height:100vh; display:flex; align-items:center;
          justify-content:center; background:#0f1115; color:#e6e8eb;
          font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif; }}
  main {{ text-align:center; padding:2rem; }}
  h1 {{ font-size:2rem; margin:0 0 .5rem; font-weight:600; }}
  p {{ color:#9aa3ad; margin:.25rem 0; }}
  code {{ font-family:ui-monospace,SFMono-Regular,Menlo,monospace; color:#9ece6a; }}
  a {{ color:#7aa2f7; text-decoration:none; font-size:.9rem; }}
</style>
</head>
<body>
  <main>
    <h1>Hello from a CGI script</h1>
    <p>Run by <code>{interpreter}</code> as pid <code>{pid}</code>.</p>
    <p>Method <code>{method}</code>, query <code>{query}</code>.</p>
    <p style="margin-top:1.5rem"><a href="/">back to the index</a></p>
  </main>
</body>
</html>
""".format(
    interpreter=sys.executable,
    pid=os.getpid(),
    method=os.environ.get("REQUEST_METHOD", "?"),
    query=os.environ.get("QUERY_STRING", "") or "(none)",
)

sys.stdout.write("Content-Type: text/html; charset=utf-8\r\n")
sys.stdout.write("\r\n")
sys.stdout.write(body)
