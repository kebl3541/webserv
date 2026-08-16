#!/usr/bin/env python3
"""Takes a multipart upload over CGI and answers in JSON.

This is the dynamic half of the demo: the same upload the server handles by
itself at /uploads/, done instead by a script the server forks, feeds on stdin,
and reads back over a pipe.

It differs from the version in the original repository in one structural way.
That one read the whole body and wrote it to disk verbatim, taking the target
name from a FILE_NAME environment variable that the old server invented and set
itself. Two problems: the file on disk was the raw multipart envelope, headers
and boundaries included, rather than the file the user chose; and a script that
only runs behind one particular server is not a CGI script. CGI/1.1 gives a
script CONTENT_TYPE and a body on stdin, and parsing that body is the script's
job. So it is parsed here.
"""

import json
import os
import re
import sys

UPLOAD_DIR = os.path.join("..", "uploads")
MAX_BODY = 5 * 1024 * 1024	# mirrors client_max_body_size on this route

MESSAGES = {
    400: "400 - Bad Request: Yo, Your Upload's Got No Vibe!",
    403: "403 - Forbidden: No Write Vibes Here!",
    405: "405 - Method Not Allowed: Wrong Dance Move. Today You Cannot Groove!",
    409: "409 - Conflict: File Already Exists, Pick a New Groove!",
    413: "413 - Payload Too Large: That File's Too Chunky for Our Dance Floor!",
    500: "500 - Internal Server Error: Server Lost Its Groove, Ouch!",
}


def reply(status_code, status_text, message, extra=None):
    payload = {"status": status_text.lower(), "message": message}
    if extra:
        payload.update(extra)
    body = json.dumps(payload)
    sys.stdout.write("Status: %d %s\r\n" % (status_code, status_text))
    sys.stdout.write("Content-Type: application/json\r\n")
    sys.stdout.write("\r\n")
    sys.stdout.flush()
    sys.stdout.buffer.write(body.encode("utf-8"))
    sys.exit(0)


def fail(status_code, status_text):
    reply(status_code, status_text, MESSAGES[status_code])


def safe_name(raw):
    """Reduce a client-supplied filename to something safe to open().

    A browser may send a full path and an attacker certainly will, so only the
    last component survives: "../../etc/passwd" is stored as "passwd" inside the
    upload directory, never above it. What is left has to be an ordinary name,
    and anything else, control characters, dot-files, shell metacharacters, is
    refused rather than rewritten into something else's name.

    This matches Upload::sanitiseFilename in the server, deliberately. Two paths
    that reach the same directory should not disagree about what a filename is.
    """
    name = raw.replace("\\", "/").split("/")[-1].strip()
    if not name or name in (".", "..") or name.startswith("."):
        return ""
    if not re.match(r"^[A-Za-z0-9._ -]{1,255}$", name):
        return ""
    return name


def boundary_from_content_type(content_type):
    match = re.search(r'boundary="?([^";]+)"?', content_type, re.IGNORECASE)
    if not match:
        return b""
    return match.group(1).strip().encode("latin-1")


def parse_multipart(body, boundary):
    """Return (filename, content) for the first part that carries a filename."""
    delimiter = b"--" + boundary
    for chunk in body.split(delimiter):
        if chunk in (b"", b"--", b"--\r\n", b"\r\n"):
            continue
        chunk = chunk.lstrip(b"\r\n")
        split = chunk.find(b"\r\n\r\n")
        if split == -1:
            continue
        headers = chunk[:split].decode("latin-1", "replace")
        content = chunk[split + 4:]
        # Every part is followed by CRLF before the next delimiter.
        if content.endswith(b"\r\n"):
            content = content[:-2]
        match = re.search(r'filename="([^"]*)"', headers, re.IGNORECASE)
        if match and match.group(1):
            return match.group(1), content
    return "", b""


def main():
    if os.environ.get("REQUEST_METHOD", "").upper() != "POST":
        fail(405, "Method Not Allowed")

    try:
        content_length = int(os.environ.get("CONTENT_LENGTH", "0"))
    except ValueError:
        fail(400, "Bad Request")

    if content_length <= 0:
        fail(400, "Bad Request")
    if content_length > MAX_BODY:
        fail(413, "Payload Too Large")

    body = sys.stdin.buffer.read(content_length)

    content_type = os.environ.get("CONTENT_TYPE", "")
    if "multipart/form-data" not in content_type.lower():
        fail(400, "Bad Request")

    boundary = boundary_from_content_type(content_type)
    if not boundary:
        # The crash that started all of this: the original server searched the
        # Content-Type for "boundary=", advanced nine bytes past the result
        # without checking it, and dereferenced NULL. One header, whole process.
        fail(400, "Bad Request")

    raw_name, content = parse_multipart(body, boundary)
    if not raw_name or not content:
        fail(400, "Bad Request")

    filename = safe_name(raw_name)
    if not filename:
        fail(400, "Bad Request")

    if not os.path.isdir(UPLOAD_DIR):
        try:
            os.makedirs(UPLOAD_DIR, 0o755)
        except OSError:
            fail(500, "Internal Server Error")

    if not os.access(UPLOAD_DIR, os.W_OK):
        fail(403, "Forbidden")

    target = os.path.join(UPLOAD_DIR, filename)
    if os.path.exists(target):
        fail(409, "Conflict")

    try:
        with open(target, "wb") as handle:
            handle.write(content)
    except OSError:
        fail(500, "Internal Server Error")

    reply(201, "CREATED", "Upload Grooved to Perfection, Baby!",
          {"file": filename, "url": "/uploads/" + filename, "bytes": len(content)})


if __name__ == "__main__":
    main()
