# Audit of the original implementation

What was wrong with the first version of this project, how each defect was
found, why it matters, and what replaced it.

This exists so that the claims in [INTERVIEW.md](INTERVIEW.md) are backed by
something specific. Every entry marked **verified** was reproduced against the
running original binary, not inferred from reading the code.

The original is a group project of roughly 4,000 lines across 12 translation
units, with a working `poll()` event loop, config parser, and CGI
implementation. The architecture was sound. What follows is about the gap
between working and correct.

---

## Summary

| Severity | Count | Examples |
|---|---|---|
| Crash or undefined behaviour | 3 | Remote denial of service from one request |
| Resource leak | 2 | CGI children never reaped |
| Protocol violation | 6 | `HEAD` unsupported, malformed `Date` |
| Correctness under load | 2 | Truncated responses, O(n²) parsing |
| Portability | 1 | Did not compile on macOS |
| Maintainability | several | Untyped control flow, dead code |

---

## 1. Remote denial of service via malformed multipart

**Severity: critical. Verified.**

### What happened

```bash
printf 'POST /upload/ HTTP/1.1\r\nHost: localhost\r\n\
Content-Type: multipart/form-data\r\nContent-Length: 5\r\n\r\nhello' \
  | nc 127.0.0.1 8080
```

The process died. Both configured virtual servers went down, along with every
connection either was serving.

### The code

In `HttpRequest.cpp`, the boundary was located like this:

```cpp
char *HttpRequest::getBoundary(const char *buffer)
{
    const char *result = strstr(buffer, "boundary");
    result += 9;                    // no NULL check
    int index_start = result - buffer;
    ...
}
```

`strstr` returns `NULL` when the needle is absent. A `Content-Type` of
`multipart/form-data` with no `boundary` parameter is unusual but perfectly
well-formed at the HTTP level, and it makes `result` null. Adding nine to a null
pointer and then dereferencing it in the loop below is an immediate segmentation
fault.

The caller compounded it:

```cpp
b = getBoundary(buffer.c_str());
...
if (boundariesIndexes.size() > 3)
    throw NotImplementedException();
int firstB = boundariesIndexes[1];   // never checked for size >= 3
int secondB = boundariesIndexes[2];
```

The guard rejects *too many* boundaries and says nothing about too few. With an
empty vector, elements one and two are out of bounds.

### Why it matters

Unauthenticated. No special access, no crafted binary payload, one request.
Anyone who could reach the port could stop the server, repeatedly. In an
interview this is the clearest possible illustration of why input validation
belongs at every boundary rather than only at the ones you happened to think of.

### The fix

`Upload::extractBoundary` returns a boolean and the caller answers 400 when it
is false. The boundary must be present, non-empty, and within the 70-character
limit RFC 2046 sets. Every subsequent index is bounds-checked, and a part
without a closing delimiter is a malformed body rather than an assumption.

Three variants are now permanent regression tests: no boundary, an empty
boundary, and a truncated body. Each asserts the server is still alive
afterwards, so a reintroduction fails loudly.

---

## 2. Crash on CGI pipe error

**Severity: high. Found by reading.**

```cpp
int ClientHandler::readStdout(int fd)
{
    while (1)
    {
        res = read(fd, buffer, BUFFER - 1);
        if (res == 0)
            break;
        this->raw_data.append(buffer, res);    // res may be -1
    }
```

The loop exits on end of file but not on error. When `read` returns `-1`, that
value is passed as the length argument to `std::string::append`, which takes a
`size_t`. The conversion makes it 18,446,744,073,709,551,615. `append` throws
`std::length_error`, which nothing catches, so `std::terminate` runs.

Worse, on a `-1` that is not fatal the loop never exits, because the only exit
condition is a return of exactly zero.

### The fix

`CgiProcess::readChunk` handles all three cases separately: a positive count
appends exactly that many bytes, zero means end of file and closes the pipe, and
`-1` is a genuine error because `poll()` had already reported the descriptor
ready. The error path closes the pipe and answers 502 rather than crashing.

---

## 3. Iterator invalidation in the event loop

**Severity: high. Found by reading. Latent rather than immediately reproducible.**

```cpp
for (it = poll_sets.begin(); it != poll_sets.end();)
{
    ...
    poll_sets.push_back(CGIPoll);       // may reallocate
    ...
    poll_sets.erase(poll_sets.begin() + i);
    ...
    it++;
}
```

`push_back` on a `std::vector` invalidates all iterators when it reallocates.
The loop then continues walking through freed memory.

### Why it did not crash

The constructor called `poll_sets.reserve(MAX)` with `MAX` defined as 1024. The
vector therefore had capacity for 1024 entries from the start, and `push_back`
did not reallocate below that. Tested with 30 and then 50 concurrent clients,
the original behaved correctly.

This is the most instructive defect in the audit. The code was undefined
behaviour that happened to be masked by an unrelated line, and the mask fails
precisely at high descriptor counts, which is when a server is under the most
load and the failure is hardest to diagnose. No amount of testing at normal
scale would have found it.

### The fix

Structural rather than a patch. `EventLoop` keeps a registry of descriptors and
rebuilds the `pollfd` array from scratch at the top of each iteration. Handlers
mutate the registry; the array being iterated is never touched. Adding two CGI
pipes or dropping a connection mid-batch is now safe by construction.

Closing was also centralised. Descriptors are marked for closing and swept once
at the end of an iteration, because a descriptor closed in the middle of a batch
can be immediately reissued by the kernel to a new connection, at which point a
later event in the same batch refers to something entirely different.

---

## 4. CGI children were never reaped

**Severity: high. Found by reading.**

`waitpid` appears nowhere in the original. Every CGI request left a zombie: a
process table entry holding an uncollected exit status. They accumulate for the
lifetime of the server, and once the table is full no process on the machine can
fork.

The timeout path called `kill(pid, SIGTERM)` and then closed the pipe without
waiting, so even deliberately terminated children leaked.

### The fix

`CgiProcess::reap` uses `waitpid` with `WNOHANG` from the event loop, so
collection never blocks. `terminate` sends `SIGKILL` and then waits blocking,
which is safe because the process is guaranteed to die. The destructor closes
both pipes and reaps, so an abandoned request cannot leak either.

A test asserts that no zombie children exist after the timeout case.

---

## 5. Partial writes were ignored

**Severity: high. Found by reading.**

```cpp
int bytes = send(client_fd, this->response.c_str(), this->response.size(), 0);
if (bytes == -1)
    return -1;
this->response.clear();
```

`send` returns the number of bytes accepted, which may be fewer than offered
once the socket send buffer fills. The original checked only for outright
failure and then discarded the response, so anything not accepted in the first
call was silently lost.

This is invisible over loopback with small files, which is why it survived. It
appears with large responses, slow clients, or congestion, and it manifests as
truncated files rather than as an error, which makes it a data-corruption bug
rather than an availability one.

### The fix

`Connection` keeps a write offset and stays in the writing state until the
buffer is drained, resuming whenever `poll()` reports the socket writable again.
The regression test requests a 2 MiB file and asserts the received byte count
matches exactly.

---

## 6. Quadratic request parsing

**Severity: medium. Found by reading.**

```cpp
int ClientHandler::checkRequestStatus(void)
{
    std::string stringLowerCases = this->raw_data;
    std::transform(stringLowerCases.begin(), stringLowerCases.end(), ...);
    if (stringLowerCases.find("\r\n\r\n") == std::string::npos)
        return 0;
```

Called on every readability event. It copies the entire accumulated buffer,
lowercases the copy, and searches it, then throws the copy away. For a request
arriving in *n* segments, that is O(n²) total work and O(n²) allocation churn.

A client sending one byte at a time makes this arbitrarily expensive, which is
the Slowloris shape: cheap for the attacker, costly for the server.

### The fix

`HttpRequest` is a state machine that keeps a cursor into its buffer and never
re-examines a byte it has already consumed. Header names are lowercased once,
individually, as they are parsed.

---

## 7. Protocol violations

All **verified** against the running original.

| Request | Original | Correct | Why it matters |
|---|---|---|---|
| `HEAD /` | 405 | 200 with headers, no body | Mandatory in HTTP/1.1; breaks health checks and link checkers |
| `Host:localhost` | 400 | 200 | RFC 7230 makes the space after the colon optional |
| `GET /style.css` | `Content-Type: text/html` | `text/css` | Browsers with strict MIME checking refuse to apply the stylesheet |
| any response | `Date: Fri Jul 31 22:18:32 2026` | `Date: Fri, 31 Jul 2026 21:18:32 GMT` | RFC 7231 requires IMF-fixdate in GMT; caches parse this |
| `DELETE` on a read-only path | 405, no `Allow` | 405 with `Allow` | The header is required, and it tells the client what to do instead |
| unknown method | 405 | 501 | 405 means "not here", 501 means "not at all" |

The MIME problem was structural rather than a missing case. Types were inferred
from the content itself:

```cpp
if (str.find("<html") != std::string::npos ...)
    return "text/html";
else if (str[1] == '{')
    return "application/json";
magicNumber = findFileType(str);      // JPEG, PNG, GIF, ICO magic bytes
```

Content sniffing cannot distinguish CSS from JavaScript from plain text, because
they are all just text. It also reads `str[1]` without checking that the string
has two characters. The rewrite maps file extensions to media types, which is
what every real server does.

---

## 8. Did not compile on macOS

**Severity: medium. Verified: the build failed on the first attempt.**

```cpp
fd = socket(serverInfo->ai_family, hints.ai_socktype | SOCK_NONBLOCK, 0);
```

`SOCK_NONBLOCK` is a Linux extension. It does not exist on macOS or the BSDs, so
the first translation unit failed to compile and the project could not be built
at all on the machine it was audited on.

The portable form is a separate `fcntl` call, which is what `ListenSocket` does.

---

## 9. Untyped control flow

**Severity: maintainability.**

Progress was signalled by bare integers returned up through the event loop:

```cpp
else if (result == 2)
    it->events = POLLOUT;
else if (result == 3)
{
    ...
}
```

with `DISCONNECTED`, `STATIC`, `CGI`, `READ` and `WRITE` defined as macros in
one header and the literals `0`, `1`, `2` and `3` used interchangeably with them
elsewhere. `manageRequest` returned `0`, `1`, `2` or `3`, each meaning something
different, and the meaning of `2` depended on which function you were in.

This is not a defect on its own. It is the condition under which the other
defects became hard to see, because you cannot tell from a call site which
transitions are legal.

The rewrite names the states, so the compiler participates in the argument.

---

## 10. Smaller items

- `if ((method_count == 0) | (route.methods.find(method) == ...))` uses bitwise
  or where logical or was meant. It happens to work for boolean operands but
  defeats short-circuiting.
- The whole POST body was written into the CGI pipe in one blocking loop, which
  deadlocks once the body exceeds the pipe buffer and the child is
  simultaneously blocked writing output nobody is reading. See
  [EXPLAINED/CgiProcess.md](EXPLAINED/CgiProcess.md).
- CGI pipe descriptors leaked on several error paths.
- `Config::operator=` allocated an array, immediately overwrote the pointer with
  another, and leaked the allocation. The class is non-copyable in the rewrite.
- Interpreter paths were hardcoded to `/usr/bin/python3` and `/usr/bin/php`,
  which are wrong on macOS with Homebrew. They are configuration now.
- Roughly 200 lines of commented-out code, including an entire abandoned CGI
  design left in place beneath the working one.
- Typos in the public interface: `composeRespone`, "Gateaway Timeout",
  "Service Unavailabled".

---

## What the rewrite is verified against

- 58 integration tests driving the real binary over real sockets.
- The same suite under AddressSanitizer and UndefinedBehaviorSanitizer, clean.
- `-Wall -Wextra -Werror -std=c++98 -pedantic`, no warnings.
- Builds and runs on macOS and Linux.

Two defects in the rewrite itself were caught by that suite before it was
published, and both are worth naming, because a test suite that never fails is
not evidence of anything:

1. The `Allow` header on a 405 was set on a response object that the error path
   then discarded and rebuilt. The header vanished. It now travels with the
   routing result.
2. The per-location body limit was never applied, because the limit is
   configured per location and the location is not known until the headers have
   been parsed. The limit is now narrowed as soon as the headers name a path,
   and lowering it re-checks whatever has already been buffered.

A third was caught in manual testing: the CGI child changes directory into the
script's directory, so a relative script path no longer resolved after the
`chdir`, and every CGI request returned 502. The path handed to `execve` is
resolved to an absolute one first.
