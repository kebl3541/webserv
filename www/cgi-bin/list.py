#!/usr/bin/env python3
"""Lists the upload directory as JSON, for the download and delete pages.

The original site had a Download Zone and a Delete Zone, each with an empty
<div class="image-gallery"> and a comment reading "Dynamic content placeholder:
replace with server-generated image list". Nothing ever replaced it, so both
pages showed an empty box for ever. This is the missing half.

The server chdir()s the child into the script's own directory before exec, so
the upload directory is one level up.
"""

import json
import os
import sys

UPLOAD_DIR = os.path.join("..", "uploads")
IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".bmp", ".ico")


def reply(status_code, status_text, payload):
    body = json.dumps(payload)
    sys.stdout.write("Status: %d %s\r\n" % (status_code, status_text))
    sys.stdout.write("Content-Type: application/json\r\n")
    # This listing changes every time something is uploaded or deleted.
    sys.stdout.write("Cache-Control: no-store\r\n")
    sys.stdout.write("\r\n")
    sys.stdout.write(body)
    sys.exit(0)


def main():
    method = os.environ.get("REQUEST_METHOD", "").upper()
    if method not in ("GET", "HEAD"):
        reply(405, "Method Not Allowed",
              {"status": "method not allowed",
               "message": "405 - Wrong Dance Move. This One Only Reads!"})

    if not os.path.isdir(UPLOAD_DIR):
        reply(200, "OK", {"status": "ok", "files": []})

    files = []
    try:
        for name in sorted(os.listdir(UPLOAD_DIR)):
            if name.startswith("."):
                continue
            path = os.path.join(UPLOAD_DIR, name)
            if not os.path.isfile(path):
                continue
            files.append({
                "name": name,
                "size": os.path.getsize(path),
                "url": "/uploads/" + name,
                "image": name.lower().endswith(IMAGE_EXTENSIONS),
            })
    except OSError:
        reply(500, "Internal Server Error",
              {"status": "internal server error",
               "message": "500 - Server Lost Its Groove, Ouch!"})

    reply(200, "OK", {"status": "ok", "files": files})


if __name__ == "__main__":
    main()
