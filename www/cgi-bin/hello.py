#!/usr/bin/env python3
"""A minimal CGI script: headers, a blank line, then the body."""

import os
import sys

body = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Hello from CGI - Webserv</title>
<link rel="stylesheet" href="/assets/site.css">
<link rel="icon" href="/favicon.ico">
</head>
<body>

<div class="bg-anim" aria-hidden="true"></div>
<div class="bg-anim" aria-hidden="true"></div>
<div class="bg-anim" aria-hidden="true"></div>

<h1>Hello</h1>

<main class="container">
	<div class="zone wide">
		<h2>From a Forked Child</h2>
		<p>Run by <code>{interpreter}</code> as pid <code>{pid}</code>.
		Method <code>{method}</code>, query <code>{query}</code>.</p>
		<a href="/">Back to the Beat</a>
		<a href="/tour/">Take the Tour</a>
	</div>
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
