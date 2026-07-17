# HTTP Server (built from scratch in C++)

A web server written by hand in C++17, using only raw POSIX sockets
(`socket`, `bind`, `listen`, `accept`, `recv`, `send`) — no external
libraries, no HTTP framework. Built step-by-step to understand exactly how
a web server works under the hood, from the network handshake up to
serving real files to a browser.

## What it does, in plain terms

A web server is a program that waits for someone (a browser, `curl`,
etc.) to connect and ask for something — like "give me `/index.html`" —
and hands back the right response. This project builds that up in
phases: first just echoing bytes back, then understanding real HTTP
requests, then serving actual files from disk, then handling many
visitors at once instead of one at a time.

## Requirements

This needs a real Linux environment — raw POSIX sockets and (later)
`epoll` don't work properly under MinGW on Windows. Everything here is
built and run inside **WSL Ubuntu**.

- WSL Ubuntu with `build-essential` installed (`g++`, `make`)
- For load testing: `wrk` (`sudo apt-get install -y wrk`)

## How to build and run

Open a WSL Ubuntu terminal (`wsl -d Ubuntu` from PowerShell, or launch
the Ubuntu app), then:

```bash
cd /mnt/c/Users/Dell/OneDrive/HTTP-Server
make            # compiles everything into ./server
./server        # starts listening on port 8080
```

Leave that terminal open — the server just sits there waiting for
connections. To stop it, press `Ctrl+C`.

## How to try it out

- **Browser**: open `http://localhost:8080/` — serves the test site in
  `www/` (an HTML page, a stylesheet, a script, and a test image).
- **curl**: `curl -v http://localhost:8080/style.css`
- **netcat** (raw, to see the exact bytes on the wire):
  ```bash
  printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | nc 127.0.0.1 8080
  ```

To rebuild after changing code: `make`. To clean up compiled files:
`make clean`.

## Project structure

| File | What it's responsible for |
|---|---|
| `Socket.hpp/cpp` | Wraps a raw socket file descriptor so it's always closed automatically (RAII), never leaked. |
| `HttpRequest.hpp/cpp` | Parses raw bytes from the client into a method, path, version, and headers. |
| `HttpResponse.hpp/cpp` | Builds the exact bytes of a reply (status line, headers, body). |
| `ThreadPool.hpp/cpp` | A fixed pool of worker threads that pick up client connections from a shared queue, so multiple people can be served at once. |
| `Server.hpp/cpp` | The main engine — accepts connections, hands them to the thread pool, maps URLs to files, decides MIME types, guards against directory-traversal attacks. |
| `main.cpp` | Starts the server on port 8080, serving files from `www/`. |
| `www/` | The actual website content being served (HTML/CSS/JS/images) — not server code. |
| `Makefile` | Builds everything with `make`. |

## Build flags

```
g++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -pthread
```

`-fsanitize=address,undefined` (ASan/UBSan) catches memory bugs and
undefined behavior immediately during testing, at the cost of real
runtime overhead — so any performance benchmark of this build is not
representative of a production/release build, only useful for comparing
this project's own phases against each other.

## Progress by phase

- ✅ **Phase 1 — TCP echo server**: accept one client, echo bytes back.
  Proved the basic socket lifecycle (create → bind → listen → accept →
  recv/send → close) works.
- ✅ **Phase 2 — HTTP parsing**: buffers `recv()` until a full header
  block (`\r\n\r\n`) arrives, parses it into method/path/version/headers,
  and returns a real HTTP response.
- ✅ **Phase 3 — Static file serving**: maps request paths onto files
  under `www/`, detects MIME type by extension, serves binary files
  (images) correctly, returns 404/403, and blocks directory-traversal
  attacks (e.g. `GET /../../etc/passwd`) via canonical-path checking.
- ✅ **Phase 4a — Thread pool**: a fixed pool of worker threads serves
  multiple clients concurrently instead of one at a time, plus
  `Connection: keep-alive` so one browser connection can make several
  requests without reconnecting.
  - Benchmark (via `wrk`, sanitized debug build, 4 worker threads):
    ~1945 req/sec at 50 concurrent connections, ~1500 req/sec at 4
    concurrent connections (avg latency ~2.6ms either way).
- ⏭️ **Phase 4b — epoll event loop** (next): a single-threaded,
  non-blocking design using `epoll`, to compare against the thread-pool
  approach above under load.
- 🔜 **Phase 5 (stretch)**: graceful shutdown, timeouts, logging, gzip,
  a tiny router.
