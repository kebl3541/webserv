#!/usr/bin/env python3
"""The fortune cookie, ported from the original fortune.php.

The original was a PHP script with a `#!/usr/bin/php-cgi` shebang, and the same
fortunes buried in about six hundred lines of echoed HTML. The fortunes are the
part worth keeping, so they are kept exactly; the page around them is now the
site stylesheet, like every other page.

Python rather than PHP because php-cgi is not installed on this machine, and a
demo that 502s in front of an interviewer is not a demo. The server can run a
second interpreter whenever you want one: give the location a different
cgi_extension and cgi_interpreter. conf/default.conf has the block for it,
commented out.
"""

import html
import random
import sys

FORTUNES = [
    "You will find great success in your near future. How near? Not sure.",
    "A pleasant surprise is waiting for you. Somewhere. Somehow. That's all I know, OK?",
    "Your talents will be recognized and rewarded. One day...",
    "Good things come to those who wait patiently. So wait. Just wait...",
    "A new adventure awaits you around the corner. :)",
    "Wisdom will guide you to make the right choices. It's a promise.",
    "Happiness is not a destination, it's a journey. Don't put your fate in fortune cookies.",
    "Your creativity will lead you to new opportunities. Nice.",
    "A friend will bring you unexpected joy soon. How cool.",
    "The best is yet to come, stay optimistic!",
    "You will soon pass a very cool version of Webserv and mark it as outstanding! Yay!",
    "No idea. The future is open.",
    "A big rock may try to catch you in the following days. Don't ask why...",
    "If you fail this interview, you'll get community service real soon. And your "
    "pillow will always be wet. I don't call the shots...",
    "Tomorrow you will have the choice to give a smile to a stranger. That's it.",
    "Tomorrow you will have the choice to give without wanting anything.",
    "Life is beautiful and you are soon going to embrace that.",
    "You are gonna be emotional.",
    "Stay away from dreams that are too big and know that whatever awaits for you is good.",
    "Start walking the way and the way will show itself.",
]

PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fortune Cookie - Webserv</title>
<link rel="stylesheet" href="/assets/site.css">
<link rel="icon" href="/favicon.ico">
</head>
<body>

<div class="bg-anim" aria-hidden="true"></div>
<div class="bg-anim" aria-hidden="true"></div>
<div class="bg-anim" aria-hidden="true"></div>
<div class="bg-anim" aria-hidden="true"></div>

<h1>Fortune</h1>

<main class="container">
	<div class="zone wide">
		<h2>Your Cookie Says</h2>
		<p>{fortune}</p>
		<a href="/cgi-bin/fortune.py">Another One</a>
		<a href="/">Back to the Beat</a>
	</div>
</main>

</body>
</html>
"""


def main():
    fortune = random.choice(FORTUNES)
    body = PAGE.format(fortune=html.escape(fortune))

    sys.stdout.write("Status: 200 OK\r\n")
    sys.stdout.write("Content-Type: text/html; charset=utf-8\r\n")
    # Without this a browser will happily hand you the same fortune twice.
    sys.stdout.write("Cache-Control: no-store\r\n")
    sys.stdout.write("\r\n")
    sys.stdout.write(body)


if __name__ == "__main__":
    main()
