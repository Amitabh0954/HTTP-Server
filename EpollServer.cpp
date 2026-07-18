#include "EpollServer.hpp"

#include "HttpRequest.hpp"
#include "HttpResponse.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
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
constexpr int kMaxEvents = 128;

// Startup-only failures (socket/bind/listen/epoll_create1) -- the server
// genuinely cannot run without these, so exiting immediately is correct.
// Never used for per-connection I/O: one bad client must not take down
// every other connection this single thread is juggling.
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

void setNonBlocking(int fd) {
    // O_NONBLOCK is what makes recv()/send()/accept() return immediately
    // with EAGAIN instead of blocking the one thread we have -- without
    // it, a single slow client would freeze the entire event loop.
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        die("fcntl");
    }
}

} // namespace

EpollServer::EpollServer(int port, const std::string& docRoot)
    : port_(port),
      handler_(docRoot),
      listen_sock_(createListenSocket(port)),
      epoll_fd_(-1) {
    signal(SIGPIPE, SIG_IGN);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    setNonBlocking(listen_sock_.fd());

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) {
        die("epoll_create1");
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_sock_.fd();
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_sock_.fd(), &ev) == -1) {
        die("epoll_ctl");
    }
}

EpollServer::~EpollServer() {
    if (epoll_fd_ != -1) {
        close(epoll_fd_);
    }
}

void EpollServer::run() {
    std::printf("Listening on port %d (epoll)\n", port_);

    epoll_event events[kMaxEvents];

    for (;;) {
        // -1 timeout: block here (using zero CPU) until at least one
        // registered fd actually has something to do. This is the entire
        // point of epoll -- we never poll/spin, we just wait to be told.
        int n = epoll_wait(epoll_fd_, events, kMaxEvents, -1);
        if (n == -1) {
            if (errno == EINTR) {
                continue; // interrupted by a signal -- not a real error
            }
            std::fprintf(stderr, "epoll_wait: %s\n", std::strerror(errno));
            continue;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_sock_.fd()) {
                acceptNewConnections();
                continue;
            }

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                closeConnection(fd);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                onReadable(fd);
            }
            // onReadable() may have already closed this connection (e.g. a
            // malformed request) -- re-check it's still tracked before
            // also acting on EPOLLOUT for the same event.
            if ((events[i].events & EPOLLOUT) && connections_.count(fd)) {
                onWritable(fd);
            }
        }
    }
}

void EpollServer::acceptNewConnections() {
    // Level-triggered epoll will keep telling us listen_sock_ is readable
    // as long as ANY connection remains queued in the kernel's backlog, so
    // one accept() per wakeup would still eventually get to all of them --
    // but draining the whole backlog now, in one wakeup, means fewer
    // epoll_wait() round trips.
    for (;;) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_sock_.fd(),
                               reinterpret_cast<sockaddr*>(&client_addr),
                               &client_len);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; // backlog is empty -- nothing more to accept right now
            }
            std::fprintf(stderr, "accept: %s\n", std::strerror(errno));
            return;
        }

        setNonBlocking(client_fd);

        // See the comment on the same call in Server.cpp: disables Nagle's
        // algorithm so a small HTTP response is never held back by the
        // kernel waiting to coalesce it with more data.
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::printf("Client connected: %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        auto it = connections_.try_emplace(client_fd, client_fd).first;
        it->second.interest = EPOLLIN;

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = client_fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            std::fprintf(stderr, "epoll_ctl (add): %s\n", std::strerror(errno));
            connections_.erase(client_fd); // Socket destructor closes client_fd
        }
    }
}

void EpollServer::onReadable(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    Connection& conn = it->second;

    char chunk[kRecvChunk];
    ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Nothing to read right now -- NOT an error. Just stop and
            // wait for the next epoll_wait() to say this fd is ready again.
            return;
        }
        std::fprintf(stderr, "recv: %s\n", std::strerror(errno));
        closeConnection(fd);
        return;
    }
    if (n == 0) {
        closeConnection(fd); // peer performed an orderly shutdown
        return;
    }

    conn.inBuf.append(chunk, static_cast<size_t>(n));

    size_t header_end_pos = conn.inBuf.find("\r\n\r\n");
    if (header_end_pos == std::string::npos) {
        if (conn.inBuf.size() > kMaxHeaderBytes) {
            std::fprintf(stderr, "Request headers too large, dropping connection.\n");
            closeConnection(fd);
        }
        return; // headers incomplete -- wait for more data on a future EPOLLIN
    }

    // Only consume the header block itself -- any bytes after it already
    // belong to the client's next request and must survive for later.
    size_t header_end = header_end_pos + 4;
    std::string header_block = conn.inBuf.substr(0, header_end);
    conn.inBuf.erase(0, header_end);

    auto request = HttpRequest::parse(header_block);
    if (!request) {
        std::fprintf(stderr, "Malformed request line, dropping connection.\n");
        closeConnection(fd);
        return;
    }

    conn.keepAlive = request->keepAlive();
    std::printf("[%s] %s %s %s\n", request->method.c_str(), request->path.c_str(),
                request->version.c_str(), conn.keepAlive ? "keep-alive" : "close");

    HttpResponse response = handler_.handle(request->path);
    response.setHeader("Connection", conn.keepAlive ? "keep-alive" : "close");
    conn.outBuf = response.toString();
    conn.outSent = 0;
    conn.state = Connection::State::Writing;

    // Try to send right now instead of only registering EPOLLOUT and
    // waiting for the next epoll_wait() wakeup. The socket is almost
    // always writable immediately (small response, empty send buffer),
    // so in the common case this finishes the whole request in a single
    // send() call with zero extra event-loop round trips. Only if this
    // first attempt can't complete do we fall back to waiting for EPOLLOUT.
    flushOutput(fd);
}

void EpollServer::onWritable(int fd) {
    flushOutput(fd);
}

void EpollServer::flushOutput(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    Connection& conn = it->second;

    ssize_t n = send(fd, conn.outBuf.data() + conn.outSent,
                     conn.outBuf.size() - conn.outSent, 0);
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Send buffer is full right now -- make sure we're registered
            // for EPOLLOUT so the next wakeup tells us when there's room
            // (a no-op if we already are, e.g. this is a follow-up call
            // from onWritable() itself).
            setEpollInterest(fd, EPOLLOUT);
            return;
        }
        std::fprintf(stderr, "send: %s\n", std::strerror(errno));
        closeConnection(fd);
        return;
    }

    conn.outSent += static_cast<size_t>(n);
    if (conn.outSent < conn.outBuf.size()) {
        setEpollInterest(fd, EPOLLOUT); // ensure we're watching for the rest
        return;
    }

    // Full response flushed.
    if (!conn.keepAlive) {
        closeConnection(fd);
        return;
    }

    conn.inBuf.clear();
    conn.outBuf.clear();
    conn.outSent = 0;
    conn.state = Connection::State::Reading;
    setEpollInterest(fd, EPOLLIN);
}

void EpollServer::closeConnection(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr); // ignore errors -- fd may already be gone
    connections_.erase(fd); // the erased Connection's Socket closes fd here
}

void EpollServer::setEpollInterest(int fd, uint32_t events) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    if (it->second.interest == events) {
        return; // already registered for exactly this -- skip the syscall
    }

    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
        std::fprintf(stderr, "epoll_ctl (mod): %s\n", std::strerror(errno));
        closeConnection(fd);
        return;
    }
    it->second.interest = events;
}
