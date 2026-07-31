#!/usr/bin/env python3
"""Prints the CGI environment, so the meta-variables the server sets are visible."""

import html
import os
import sys

rows = []
for name in sorted(os.environ):
    rows.append(
        "<tr><td>{}</td><td>{}</td></tr>".format(
            html.escape(name), html.escape(os.environ[name])
        )
    )

body = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CGI environment</title>
<style>
  body {{ background:#0f1115; color:#e6e8eb; margin:0; padding:3rem 1.5rem;
          font-family:ui-monospace,SFMono-Regular,Menlo,monospace; font-size:.85rem; }}
  main {{ max-width:60rem; margin:0 auto; }}
  h1 {{ font-size:1.1rem; color:#9aa3ad; font-weight:600;
        border-bottom:1px solid #252932; padding-bottom:.75rem; }}
  div.scroll {{ overflow-x:auto; }}
  table {{ border-collapse:collapse; width:100%; }}
  td {{ padding:.4rem .75rem; border-bottom:1px solid #1c2029; vertical-align:top; }}
  td:first-child {{ color:#9ece6a; white-space:nowrap; }}
  td:last-child {{ color:#e6e8eb; word-break:break-all; }}
  a {{ color:#7aa2f7; text-decoration:none; }}
</style>
</head>
<body>
<main>
  <h1>CGI environment</h1>
  <div class="scroll"><table>{rows}</table></div>
  <p style="margin-top:2rem"><a href="/">back to the index</a></p>
</main>
</body>
</html>
""".format(rows="\n".join(rows))

sys.stdout.write("Content-Type: text/html; charset=utf-8\r\n")
sys.stdout.write("\r\n")
sys.stdout.write(body)
