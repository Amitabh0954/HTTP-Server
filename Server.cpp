#include "Server.hpp"

#include "HttpRequest.hpp"
#include "HttpResponse.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Sized for load-testing at high concurrency (wrk -c1000, etc). Too small
// a backlog here causes the kernel to drop/cookie incoming SYNs under a
// burst of simultaneous connection attempts ("SYN flooding" in dmesg) --
// visible as client-side connect() stalls and timeouts that have nothing
// to do with how fast this program itself responds.
constexpr int kBacklog = 1024;
constexpr size_t kRecvChunk = 4096;
constexpr size_t kMaxHeaderBytes = 8192; // guards against an unbounded buffer
                                          // if a client never sends "\r\n\r\n"

// For one-time startup failures only (socket/bind/listen) -- the server
// genuinely cannot run without these, so exiting immediately is correct.
// This must NEVER be used for per-connection I/O once multiple clients are
// being served concurrently (see sendAll below).
[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "%s: %s\n", what, std::strerror(errno));
    std::exit(EXIT_FAILURE);
}

int createListenSocket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        die("socket");
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        die("setsockopt");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        die("bind");
    }
    if (listen(fd, kBacklog) == -1) {
        die("listen");
    }

    return fd;
}

// Returns false (instead of exiting the process) on failure. A send()
// failure means THIS client's connection is broken -- e.g. they closed
// their end mid-response -- which must not take down every other client
// currently being served by other worker threads.
bool sendAll(int fd, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = send(fd, data.data() + total_sent, data.size() - total_sent, 0);
        if (sent == -1) {
            std::fprintf(stderr, "send: %s\n", std::strerror(errno));
            return false;
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

} // namespace

Server::Server(int port, const std::string& docRoot, size_t numThreads)
    : port_(port),
      handler_(docRoot),
      listen_sock_(createListenSocket(port)),
      pool_(numThreads, [this](int fd) { handleClient(fd); }) {
    // Without this, writing to a socket whose peer already closed their
    // end delivers SIGPIPE, whose DEFAULT action terminates the whole
    // process -- before send() even gets a chance to return -1 for us to
    // check. Ignoring it means send() fails normally with errno == EPIPE
    // instead, which sendAll() already handles.
    signal(SIGPIPE, SIG_IGN);

    // stdout is only line-buffered when it's attached to a terminal. The
    // moment it's redirected to a file/pipe (as it will be for any real
    // deployment, or when we background it for a benchmark run), the C
    // library switches to fully-buffered mode: printf() output sits in an
    // internal buffer and never reaches the file until the buffer fills or
    // the process exits. Since this server runs forever, that means "never"
    // -- every log line would silently vanish. Forcing line-buffering here
    // makes each printf() visible immediately regardless of where stdout
    // points.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
}

void Server::run() {
    std::printf("Listening on port %d (thread pool)\n", port_);

    for (;;) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_sock_.fd(),
                               reinterpret_cast<sockaddr*>(&client_addr),
                               &client_len);
        if (client_fd == -1) {
            std::fprintf(stderr, "accept: %s\n", std::strerror(errno));
            continue;
        }

        // Disables Nagle's algorithm. Without this, the kernel can delay
        // transmitting a small write (like our headers, if a response
        // ever goes out in more than one send()) hoping to coalesce it
        // with more outgoing data -- which, combined with the peer's
        // delayed-ACK timer, can stall a single small response by tens to
        // hundreds of milliseconds. HTTP responses are latency-sensitive
        // and not a stream of unrelated small writes, so we always want
        // them sent immediately.
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::printf("Client connected: %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        // Hand the fd off to a worker and immediately go back to
        // accept()ing the next connection -- this thread never blocks on
        // a client's I/O.
        pool_.submit(client_fd);
    }
}

void Server::handleClient(int client_fd) {
    Socket client_sock(client_fd); // guarantees close(), on every return below

    std::string buffer;

    for (;;) { // one iteration per request on this (possibly persistent) connection
        while (buffer.find("\r\n\r\n") == std::string::npos) {
            if (buffer.size() > kMaxHeaderBytes) {
                std::fprintf(stderr, "Request headers too large, dropping connection.\n");
                return;
            }

            char chunk[kRecvChunk];
            ssize_t n = recv(client_sock.fd(), chunk, sizeof(chunk), 0);
            if (n == -1) {
                std::fprintf(stderr, "recv: %s\n", std::strerror(errno));
                return;
            }
            if (n == 0) {
                return; // client closed the connection
            }
            buffer.append(chunk, static_cast<size_t>(n));
        }

        // Only consume the header block we just found, not the whole
        // buffer -- any bytes after it already belong to the client's
        // NEXT request (e.g. if they pipelined, or we just happened to
        // read ahead in one recv() call) and must be preserved for the
        // next loop iteration rather than thrown away.
        size_t header_end = buffer.find("\r\n\r\n") + 4;
        std::string header_block = buffer.substr(0, header_end);
        buffer.erase(0, header_end);

        auto request = HttpRequest::parse(header_block);
        if (!request) {
            std::fprintf(stderr, "Malformed request line, dropping connection.\n");
            return;
        }

        bool keep_alive = request->keepAlive();

        std::printf("[%s] %s %s %s\n", request->method.c_str(),
                    request->path.c_str(), request->version.c_str(),
                    keep_alive ? "keep-alive" : "close");

        HttpResponse response = handler_.handle(request->path);
        response.setHeader("Connection", keep_alive ? "keep-alive" : "close");

        if (!sendAll(client_sock.fd(), response.toString())) {
            return; // this client's connection broke -- other clients are unaffected
        }

        if (!keep_alive) {
            return; // Socket destructor closes client_fd
        }
        // else: loop back and read the next request off the same connection
    }
}
