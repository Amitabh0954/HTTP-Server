# HTTP Server (built from scratch in C++)

A web server written by hand in C++17, using only raw POSIX sockets
(`socket`, `bind`, `listen`, `accept`, `recv`, `send`) -- no external
libraries, no HTTP framework.

For the full write-up (server fundamentals explained simply, phase-by-phase
history, benchmarks, and the live file structure), generate
`DOCUMENTATION.md` -- see [Full documentation](#full-documentation) below.

## Requirements

This needs a real Linux environment -- raw POSIX sockets and `epoll` don't
work properly under MinGW on Windows. Everything here is built and run
inside WSL Ubuntu.

- WSL Ubuntu with `build-essential` installed (`g++`, `make`)
- For load testing: `wrk` (`sudo apt-get install -y wrk`)
- For generating the full doc: Python 3 (no extra packages needed)

## How to build and run

Open a WSL Ubuntu terminal (`wsl -d Ubuntu` from PowerShell, or launch the
Ubuntu app), then:

```bash
cd /mnt/c/Users/Dell/OneDrive/HTTP-Server
make              # compiles everything into ./server
./server          # thread-pool server, listens on port 8080
./server --epoll  # epoll event-loop server instead, same port
```

Leave that terminal open -- the server just sits there waiting for
connections. To stop it, press Ctrl+C.

## How to try it out

- Browser: open `http://localhost:8080/` -- serves the test site in `www/`
  (an HTML page, a stylesheet, a script, and a test image).
- curl: `curl -v http://localhost:8080/style.css`
- netcat (raw, to see the exact bytes on the wire):
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
| `RequestHandler.hpp/cpp` | Turns a request path into a response: MIME-type detection, binary-safe file reads, 404/403, directory-traversal protection. Shared by both server implementations below. |
| `Server.hpp/cpp` | The thread-pool server (Phase 4a) -- accepts connections and hands each one to the thread pool. |
| `EpollServer.hpp/cpp` | The epoll event-loop server (Phase 4b) -- one thread, non-blocking sockets, `epoll_wait` to multiplex many connections. |
| `main.cpp` | Starts the server on port 8080, serving files from `www/`. Runs the thread-pool version by default, or the epoll version with `./server --epoll`. |
| `www/` | The actual website content being served (HTML/CSS/JS/images) -- not server code. |
| `Makefile` | Builds everything with `make`. |
| `generate_docs.py` | Generates `DOCUMENTATION.md` (see below). |

## Build flags

```
g++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -pthread
```

`-fsanitize=address,undefined` (ASan/UBSan) catches memory bugs and
undefined behavior immediately during testing, at the cost of real runtime
overhead -- so any performance benchmark of this build is not
representative of a production/release build, only useful for comparing
this project's own phases against each other.

## Full documentation

`DOCUMENTATION.md` is generated, not checked into git (it's in
`.gitignore`), so it can't go stale against the actual files in the repo.
Regenerate it any time with:

```bash
python3 generate_docs.py
```

It covers: what a server is and how the underlying concepts work (in
plain language), a phase-by-phase history of what was built and why, the
Phase 4a vs 4b benchmark results and why epoll came out slower, and a
live listing of the project's file structure.
