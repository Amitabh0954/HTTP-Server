#!/usr/bin/env python3
"""Generates DOCUMENTATION.md: fundamentals, phase-by-phase history, and a
live snapshot of the project's file structure, all in one place.

Run: python3 generate_docs.py

The output is gitignored on purpose -- it's a build artifact regenerated
from this script plus whatever files actually exist in the repo, so it can
never go stale or drift out of sync the way a hand-maintained doc can.
Edit the content in THIS script, not DOCUMENTATION.md directly.
"""

import datetime
import os
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent
OUTPUT_FILE = PROJECT_ROOT / "DOCUMENTATION.md"

EXCLUDE_DIRS = {".git", "__pycache__", ".vscode", ".idea"}
EXCLUDE_FILES = {"DOCUMENTATION.md"}
EXCLUDE_SUFFIXES = (".o",)

# One-line description for files worth explaining. Anything that exists on
# disk but isn't listed here still shows up in the tree, just undescribed --
# so adding a new source file doesn't require touching this script to keep
# the structure section accurate (only to keep it *annotated*).
FILE_DESCRIPTIONS = {
    "Socket.hpp": "RAII wrapper around a raw file descriptor -- closes it automatically, exactly once, on any exit path.",
    "Socket.cpp": "Socket implementation: move-only (copying would risk a double-close), closes fd_ in the destructor.",
    "HttpRequest.hpp": "Struct for a parsed request (method/path/version/headers) plus a keepAlive() helper.",
    "HttpRequest.cpp": "Parses raw request bytes into an HttpRequest.",
    "HttpResponse.hpp": "Builder for the exact bytes of an HTTP response.",
    "HttpResponse.cpp": "Serializes status line + headers + body; computes Content-Length itself so it can't drift from the body.",
    "RequestHandler.hpp": "Shared request-to-response logic: MIME detection, directory-traversal protection, binary-safe file reads.",
    "RequestHandler.cpp": "RequestHandler implementation -- used by BOTH server models below, so the security-critical traversal check exists in exactly one place.",
    "ThreadPool.hpp": "Fixed-size worker-thread pool: queue of client fds guarded by a mutex + condition_variable.",
    "ThreadPool.cpp": "ThreadPool implementation.",
    "Server.hpp": "Phase 4a server: thread-pool concurrency model.",
    "Server.cpp": "Accepts connections, hands each fd to the ThreadPool, runs the keep-alive request loop per client.",
    "EpollServer.hpp": "Phase 4b server: single-threaded epoll event-loop concurrency model.",
    "EpollServer.cpp": "Non-blocking sockets, one epoll_wait loop, per-connection READING/WRITING state machine.",
    "main.cpp": "Entry point -- runs the thread-pool server by default, or the epoll server with --epoll.",
    "Makefile": "Build recipe: g++, C++17, ASan/UBSan, -pthread.",
    "generate_docs.py": "This script -- regenerates DOCUMENTATION.md from the project's current files.",
    "README.md": "Short, practical quick-reference: build/run/test instructions.",
}

FUNDAMENTALS = r"""## Part 1: What even is a "server"?

Imagine a restaurant.

- The kitchen is always open and ready -- that's your server program,
  sitting there waiting.
- A customer walks in and asks for a table -- that's a browser connecting
  to your server.
- The waiter takes the order to the kitchen, and brings the food back --
  that's your server reading a request and sending back a response.

A "server" is just a program that waits for someone to connect, and then
responds to whatever they ask for. That's it -- just a program in a loop,
waiting.

### Where is the "restaurant"? (IP addresses and ports)

- Every computer on a network has an address -- like a street address for
  a building. This is the IP address (e.g. `127.0.0.1` means "this same
  computer", also called `localhost`).
- A single computer can run many programs that all want to listen for
  connections (a web server, a game server, etc). So each program picks a
  port -- think of it like an apartment number inside that building. Your
  server uses port 8080. When your browser goes to
  `http://localhost:8080`, it's saying "go to this computer, apartment
  8080."

### How do they actually talk? (sockets and TCP)

- A socket is like a telephone line -- a two-way connection between your
  server and one specific visitor.
- TCP is the rule they follow while talking: it guarantees the words
  arrive in order, and none go missing. This matters because a raw
  network connection is really just a stream of bytes -- TCP is what
  makes it reliable.
- Importantly: TCP doesn't care about "messages." It's just a stream of
  bytes flowing between two socket endpoints. If your server wants to
  know "has the customer finished ordering yet?", it has to figure that
  out by looking at the bytes itself -- the network won't tell it. This
  is exactly why the code buffers incoming data and watches for a marker
  (`"\r\n\r\n"`) that means "the request is complete."

### What language do they speak? (HTTP)

Once the socket is connected, browser and server need a shared language
to actually request/serve pages. That language is HTTP. A request looks
like plain text:

    GET /index.html HTTP/1.1
    Host: localhost:8080

...and a response looks like:

    HTTP/1.1 200 OK
    Content-Type: text/html
    Content-Length: 505

    <html>...the actual page...</html>

The server's whole job is: read text that looks like the first block,
and write back text that looks like the second block.

## Part 2: A few words worth knowing

| Word | Simple meaning |
|---|---|
| File descriptor (fd) | A small number the OS gives you as a "handle" to something open -- a file, or here, a network connection. |
| bind | Claiming a specific port for your program, so the OS routes traffic for that port to you. |
| listen | Telling the OS "I'm ready to accept visitors on this port now." |
| accept | Waiting until a visitor actually connects, then handing you a *new* connection specifically for them. |
| recv / send | Read bytes from / write bytes to a connection. |
| RAII | A C++ habit: wrap a risky resource (like a socket) in a class so it's automatically cleaned up when you're done, even if something goes wrong halfway through. |
| Thread | A separate worker that can run code at the same time as other workers, so the server can handle more than one visitor simultaneously. |
| epoll | A Linux mechanism that tells one thread exactly which of many open connections actually have something ready right now, instead of that thread blocking on any single one. |
"""

PHASE_HISTORY = r"""## Part 3: Project history, phase by phase

### Phase 1 -- TCP echo server (done)
The simplest possible server: accept one visitor, and whatever they type,
send it right back. No HTTP understanding yet -- just proving the
plumbing (socket -> bind -> listen -> accept -> recv/send) works.

### Phase 2 -- Understanding HTTP (done)
Reads a real request (method + path + headers) instead of echoing raw
text, and replies with a proper HTTP response (status code, headers,
body).

### Phase 3 -- Serving real files (done)
Looks up the requested path inside www/, detects the MIME type from the
extension, and sends the file's actual bytes back (binary-safe, so images
work). Added 404 for missing files and 403 for directory-traversal
attempts (e.g. `GET /../../etc/passwd`), enforced via
`std::filesystem::weakly_canonical` plus a component-by-component prefix
check against the document root.

### Phase 4a -- Thread pool (done)
A fixed pool of worker threads (default 4) serves multiple clients
concurrently instead of one at a time: the accept loop only accepts and
hands connections off, never blocking on a client's own I/O. Added
Connection: keep-alive so one browser connection can make several
requests without reconnecting.

Benchmark (wrk, sanitized ASan/UBSan debug build, 4 worker threads):
- 50 concurrent connections: ~1945 req/sec, ~2.6ms avg latency
- 1000 concurrent connections: ~1658 req/sec, ~2.4ms avg latency, 0 timeouts

### Phase 4b -- epoll event loop (done)
A single thread, non-blocking sockets (O_NONBLOCK), one epoll_wait loop
multiplexing every connection through a READING/WRITING state machine
with per-connection buffers, instead of one worker thread per client.

Benchmark (same build, same www/ files):
- 50 concurrent connections: ~209 req/sec, ~226ms avg latency
- 1000 concurrent connections: ~202 req/sec, ~999ms avg latency, 1646 timeouts

This is slower than the thread pool, not faster -- and that result is the
actual lesson of this phase. epoll only makes *socket* I/O non-blocking;
regular file I/O (open/read/stat, which RequestHandler calls on every
request) has no non-blocking mode. Confirmed by watching the process sit
in Linux's D state (disk-wait) at ~10% CPU while under load -- it wasn't
computing, it was blocked on disk. The thread pool has 4 threads, so up
to 4 of those blocking file reads can happen at once; the epoll server
has exactly one thread, so every connection's already-ready socket has to
wait behind whichever single disk read is currently blocking that one
thread. epoll solves "many idle/slow *network* connections" -- it does
not remove blocking *disk* I/O from the picture. Real async servers
(nginx, Node.js/libuv) solve this by farming blocking file I/O out to a
small internal worker-thread pool while keeping socket handling on the
event-loop thread -- a hybrid of the two models built here.

### Phase 5 -- stretch goals (planned)
Graceful SIGINT shutdown, request timeouts, logging, gzip, a tiny router
for dynamic routes.

## Part 4: Following one request through the code

What literally happens when you type `http://localhost:8080/` in a
browser and hit enter (thread-pool server):

1. The browser opens a TCP connection to 127.0.0.1:8080.
2. The server's main loop is sitting in accept() (Server.cpp) -- it wakes
   up, gets a new file descriptor for this one visitor, and hands that
   number off to the thread pool (`pool_.submit(client_fd)`).
3. A free worker thread picks it up and calls `handleClient(client_fd)`.
4. It wraps the fd in a Socket (guaranteed to close later), then calls
   recv() in a loop, gluing bytes into a buffer until it spots
   `"\r\n\r\n"` -- the end of the headers.
5. `HttpRequest::parse(...)` turns that raw text into a struct: method
   "GET", path "/", version "HTTP/1.1", plus a map of headers.
6. `RequestHandler::handle("/")` rewrites "/" to "/index.html", safely
   joins it onto the www/ folder, double-checks it didn't escape that
   folder, reads the file's bytes, and picks Content-Type: text/html
   based on the .html extension.
7. `HttpResponse::toString()` turns all that into the exact text format
   HTTP expects, and sendAll() writes it back over the same socket.
8. The browser receives those bytes, recognizes it as HTML, and draws
   the page.

`RequestHandler` (step 6) is shared code, used by whichever server
implementation is running -- it's the "look up the file" logic, kept
separate from "how do we juggle many visitors," which is the one part
that's genuinely different between Phase 4a and 4b.

With `./server --epoll`, steps 2-8 still all happen, just not as one
straight-line function call. accept() and "wait for more bytes" both go
through epoll_wait() instead: rather than a thread sitting inside recv()
until data arrives, the single thread asks "which connections actually
have something ready?", handles exactly those, then asks again. Each
connection's half-read buffer has to be saved somewhere between those
asks (in `EpollServer::Connection`) since there's no per-client function
call sitting on the stack holding it the way handleClient's local
variable did in Phase 4a.
"""


def build_file_tree() -> str:
    lines = []
    for root, dirs, files in os.walk(PROJECT_ROOT):
        dirs[:] = sorted(d for d in dirs if d not in EXCLUDE_DIRS)
        rel_root = Path(root).relative_to(PROJECT_ROOT)
        is_root = str(rel_root) == "."
        depth = 0 if is_root else len(rel_root.parts)

        if not is_root:
            lines.append(f"{'  ' * (depth - 1)}{rel_root.name}/")

        for name in sorted(files):
            if name in EXCLUDE_FILES or name.endswith(EXCLUDE_SUFFIXES):
                continue
            if is_root and name == "server":  # compiled binary, not source
                continue
            desc = FILE_DESCRIPTIONS.get(name)
            entry = f"{'  ' * depth}{name}"
            if desc:
                entry += f" -- {desc}"
            lines.append(entry)

    return "\n".join(lines)


def main() -> None:
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    tree = build_file_tree()

    content = f"""# HTTP Server: Full Documentation

Auto-generated by generate_docs.py on {timestamp}.
Do not edit this file directly -- edit generate_docs.py and rerun it.

{FUNDAMENTALS}
{PHASE_HISTORY}
## Part 5: File structure (live, generated from disk)

```
{tree}
```
"""

    OUTPUT_FILE.write_text(content, encoding="utf-8")
    print(f"Wrote {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
