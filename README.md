# webserv

An HTTP/1.1 server written from scratch in C++98. One process, one thread, one
`poll()` loop, no external libraries. Every socket, every byte of parsing and
every child process is managed by hand.

```
make && ./webserv conf/default.conf
```

Then open <http://127.0.0.1:8080/>.

## The site

`www/` is the demo site from the original project, kept as it was and made to
work. Each page exercises a different part of the server rather than describing
it:

| | |
|---|---|
| `/` | The front page. One shared stylesheet, which the original server could not serve at all: it guessed media types from a file's first bytes, and CSS has no magic number, so every stylesheet went out as `text/plain` |
| `/upload/` | A plain form posting to `/uploads/`. The server parses the multipart body itself |
| `/cgi-bin/upload.py` | The same upload through CGI, answering in JSON |
| `/downloads/`, `/delete/` | Both driven by `/cgi-bin/list.py`. The delete page issues a real `DELETE`, which an HTML form cannot do |
| `/cgi-bin/fortune.py` | A script that chooses its own status code |
| `/tour/` | The protocol side: autoindex, `alias`, media types, `return 301`, the second port |
| `/errors/*.html` | One page per status the server can emit, wired up with `error_page` |

---

## What it does

| | |
|---|---|
| **Protocol** | HTTP/1.1 with keep-alive, pipelining, chunked transfer decoding, `HEAD`, and correct `Content-Length` framing |
| **Methods** | `GET`, `HEAD`, `POST`, `DELETE` — permitted per location, with `Allow` advertised on rejection |
| **Static files** | Extension-based MIME types, directory indexes, optional autoindex listings |
| **Uploads** | `multipart/form-data` and raw bodies, written to a configured directory |
| **CGI** | CGI/1.1 over non-blocking pipes, with timeouts and child reaping |
| **Config** | nginx-style blocks: several servers, prefix-matched locations, `root`/`alias`, redirects, custom error pages, per-location body limits |
| **Robustness** | Idle and CGI timeouts, body-size limits enforced while streaming, path-traversal rejection, bounded header and connection counts |

Everything is verified by an integration suite that drives the real binary over
real sockets: `make test` runs 104 cases, and the whole suite also passes under
AddressSanitizer and UndefinedBehaviorSanitizer.

---

## Architecture

```
                    ┌──────────────────────────────────────────┐
                    │                 main()                   │
                    │   parse config, install signal handlers  │
                    └────────────────────┬─────────────────────┘
                                         │
                    ┌────────────────────▼─────────────────────┐
                    │               EventLoop                  │
                    │   owns every descriptor; one poll() call  │
                    │   per iteration over a freshly built set  │
                    └───┬─────────────┬──────────────┬─────────┘
                        │             │              │
              ┌─────────▼──┐   ┌──────▼──────┐  ┌────▼─────────┐
              │ListenSocket│   │ Connection  │  │  CgiProcess  │
              │ accept()   │   │ per client  │  │ fork + pipes │
              └────────────┘   └──────┬──────┘  └──────────────┘
                                      │
                    ┌─────────────────┼─────────────────┐
                    │                 │                 │
            ┌───────▼──────┐  ┌───────▼──────┐  ┌───────▼──────┐
            │ HttpRequest  │  │RequestHandler│  │ HttpResponse │
            │  incremental │  │   routing    │  │ serialisation│
            │    parser    │  │  + handlers  │  │              │
            └──────────────┘  └──────────────┘  └──────────────┘
```

### The event loop

A single `poll()` call multiplexes listening sockets, client sockets and CGI
pipes. Nothing in the request path ever blocks, so one slow client or one slow
script cannot stall anyone else.

Each iteration does the same four things:

1. **Build the poll set** from the descriptor registry. Interest is derived from
   each connection's state, so the loop never asks for writability on a socket
   with nothing to write.
2. **Poll**, with a one-second timeout so timers still fire when traffic is idle.
3. **Dispatch** each ready descriptor to its owner.
4. **Sweep**: advance CGI exchanges, enforce timeouts, and close whatever was
   marked for closing.

The poll set is rebuilt from scratch rather than mutated during iteration, and
descriptors are closed in exactly one place at the end of an iteration. Those
two rules are what make it safe for a handler to start a CGI child (adding two
descriptors) or drop a connection (removing several) without corrupting the walk
that is in progress.

### The connection state machine

```
   ┌──────────────────┐  request complete   ┌──────────────┐
   │ READING_REQUEST  ├────────────────────►│ RUNNING_CGI  │
   │                  │                     └──────┬───────┘
   │  incremental     │                            │ script finished
   │  parse, no       │  response ready            ▼
   │  rescanning      ├─────────────────────►┌──────────────────┐
   └────────▲─────────┘                      │ WRITING_RESPONSE │
            │                                └────────┬─────────┘
            │  keep-alive: recycle,                   │
            └─────────────────────────────────────────┤
                     carrying over pipelined bytes    │ Connection: close
                                                      ▼
                                              ┌──────────────┐
                                              │   CLOSING    │
                                              └──────────────┘
```

### The request parser

The parser is fed whatever bytes `recv()` happened to return and keeps its own
state between calls. A request split across fifty TCP segments costs the same as
one delivered in a single read, because no byte is examined twice.

It enforces its limits while parsing rather than afterwards: an oversized body
is refused as it streams, and the limit is narrowed to the matched location's as
soon as the headers reveal which location that is.

It also refuses several things that are syntactically tempting but dangerous:
`Content-Length` together with `Transfer-Encoding`, duplicate `Content-Length`,
and obsolete line folding — each of which is a request-smuggling primitive.

### CGI

A script gets a `fork()` and two pipes, both non-blocking and both registered
with the same `poll()` loop as everything else. The request body is written in
whatever slices the pipe accepts; output is read as it appears.

Writing a body larger than the pipe buffer in one blocking loop deadlocks as
soon as the script starts writing output the parent is not yet reading, so the
write is incremental and the read is concurrent with it. Children are reaped
with `waitpid(WNOHANG)`, and one that overruns its budget is killed and reaped
before a 504 goes out.

---

## Configuration

```nginx
server {
    listen               127.0.0.1:8080;
    server_name          localhost;
    root                 ./www;
    index                index.html;
    client_max_body_size 10485760;

    error_page 404 /errors/404.html;

    location / {
        allow_methods GET HEAD;
    }

    location /uploads/ {
        allow_methods        GET HEAD POST DELETE;
        upload_store         ./www/uploads;
        autoindex            on;
        client_max_body_size 5242880;
    }

    location /cgi-bin/ {
        allow_methods   GET POST;
        cgi_extension   .py;
        cgi_interpreter /usr/bin/python3;
    }

    location /old-page {
        return 301 /;
    }
}
```

Locations are matched by longest prefix, the same rule nginx uses, and a prefix
only counts when it ends at a path boundary — so `/cgi` does not match
`/cgi-bin/script.py`.

### Directives

**Server scope**

| Directive | Meaning |
|---|---|
| `listen` | `PORT` or `HOST:PORT` |
| `server_name` | Names for this block |
| `root` | Filesystem root |
| `index` | Default file for a directory |
| `client_max_body_size` | Byte cap on request bodies |
| `error_page CODE... PATH` | Custom page for one or more status codes |

**Location scope**

| Directive | Meaning |
|---|---|
| `allow_methods` | Permitted methods; anything else gets 405 with `Allow` |
| `root` / `alias` | `root` appends the URI path, `alias` replaces the matched prefix |
| `index` | Default file for a directory |
| `autoindex on\|off` | Generate a listing when no index file exists |
| `upload_store` | Directory for `POST` bodies; also enables uploads |
| `cgi_extension` / `cgi_interpreter` | Which files are scripts, and what runs them |
| `return CODE TARGET` | Redirect with a 3xx |
| `client_max_body_size` | Overrides the server value here |

---

## Building and testing

```bash
make            # release build, -Wall -Wextra -Werror -std=c++98 -pedantic
make debug      # -g3 -O0, for a debugger
make sanitize   # AddressSanitizer + UndefinedBehaviorSanitizer
make test       # start the server and run the integration suite
make re         # rebuild from clean
```

`WEBSERV_LOG` selects the log level (`debug`, `info`, `warn`, `error`, `none`).

The test suite covers static serving, MIME mapping, header correctness, method
handling, parser edge cases, traversal defences, upload and delete, body limits,
CGI including timeouts and zombie reaping, keep-alive and pipelining, fifty
concurrent clients, and a 2 MiB response that must arrive whole.

Several cases are regression tests named after defects this project used to
have. The clearest of them:

```bash
printf 'POST /uploads/ HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data\r\nContent-Length: 5\r\n\r\nhello' \
  | nc 127.0.0.1 8080
```

A `multipart/form-data` content type with no `boundary` parameter. The earlier
version searched for the boundary with `strstr`, advanced the returned pointer
without checking it for `NULL`, and then indexed a vector of boundary offsets
without checking its size. That one request killed the process and every
connection it was serving. It is now a 400, and it is a test.

---

## Layout

```
├── src/                 implementation
│   ├── main.cpp             startup, signals
│   ├── EventLoop.cpp        poll() loop, descriptor ownership
│   ├── ListenSocket.cpp     bind, listen
│   ├── Connection.cpp       per-client state machine
│   ├── HttpRequest.cpp      incremental parser
│   ├── HttpResponse.cpp     serialisation
│   ├── RequestHandler.cpp   routing, static files, autoindex, delete
│   ├── CgiProcess.cpp       fork, pipes, reaping
│   ├── Upload.cpp           multipart and raw body storage
│   ├── Config.cpp           tokeniser and parser
│   ├── MimeTypes.cpp        extension to media type
│   ├── Logger.cpp           levelled logging
│   └── Utils.cpp            strings, paths, dates
├── include/             one header per translation unit
├── conf/default.conf    two servers on two ports
├── tests/run_tests.sh   integration suite
├── www/, www2/          document roots
└── Makefile
```

---

## Known limits

Stated plainly, because knowing where a design stops matters more than pretending
it does not.

- **Responses are built in memory.** A large file is read fully before it is
  sent. Streaming it from disk, or handing it to `sendfile()`, would be the next
  change.
- **`poll()` is O(n) per iteration.** Fine for hundreds of connections, wrong for
  tens of thousands. `epoll` or `kqueue` is the fix, and the event loop is the
  only file that would need to change.
- **One server block per endpoint.** Name-based virtual hosting on a shared port
  is not implemented; the `Host` header is validated but not used for routing.
- **No TLS.** Terminate it in front.
