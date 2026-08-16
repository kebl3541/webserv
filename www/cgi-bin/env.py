#!/usr/bin/env python3
"""Prints the CGI environment, so the meta-variables the server sets are visible."""

import html
import os
import sys

rows = []
for name in sorted(os.environ):
    rows.append(
        "\t\t\t<tr><td>{}</td><td>{}</td></tr>".format(
            html.escape(name), html.escape(os.environ[name])
        )
    )

body = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CGI Environment - Webserv</title>
<link rel="stylesheet" href="/assets/site.css">
<link rel="icon" href="/favicon.ico">
<style>
  .scroll {{ overflow-x: auto; }}
  table {{ border-collapse: collapse; width: 100%; font-size: 14px; }}
  td {{ padding: 6px 10px; border-bottom: 1px dashed rgba(255, 204, 0, .4);
        vertical-align: top; text-align: left; }}
  td:first-child {{ white-space: nowrap; font-weight: bold; }}
  td:last-child {{ word-break: break-all; }}
</style>
</head>
<body>

<div class="bg-anim" aria-hidden="true"></div>
<div class="bg-anim" aria-hidden="true"></div>
<div class="bg-anim" aria-hidden="true"></div>

<h1>The Env</h1>

<main class="container">
	<div class="zone wide">
		<h2>What the Server Hands a Script</h2>
		<div class="scroll">
		<table>
{rows}
		</table>
		</div>
		<p style="margin-top:15px"><a href="/">Back to the Beat</a>
		<a href="/cookies/">Cookie Jar</a></p>
	</div>
</main>

</body>
</html>
""".format(rows="\n".join(rows))

sys.stdout.write("Content-Type: text/html; charset=utf-8\r\n")
sys.stdout.write("Cache-Control: no-store\r\n")
sys.stdout.write("\r\n")
sys.stdout.write(body)
