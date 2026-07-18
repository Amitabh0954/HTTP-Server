#pragma once

#include "RequestHandler.hpp"
#include "Socket.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

// A single-threaded, non-blocking HTTP server built around epoll.
//
// Compare with Server (Phase 4a): there, a fixed pool of THREADS each
// block in recv()/send() on one client at a time, and a client beyond the
// pool's size just queues. Here there is exactly ONE thread, and it never
// blocks on any single client -- every socket is non-blocking (O_NONBLOCK),
// and epoll_wait() tells this thread exactly which sockets have work
// ready *right now*, so it only ever touches a socket when doing so won't
// block. That lets one thread juggle far more concurrent connections than
// there are OS threads.
class EpollServer {
public:
    EpollServer(int port, const std::string& docRoot);
    ~EpollServer();

    EpollServer(const EpollServer&) = delete;
    EpollServer& operator=(const EpollServer&) = delete;

    [[noreturn]] void run();

private:
    // Per-connection state. With one thread serving many clients at once,
    // each connection needs its OWN buffers -- there's no per-client stack
    // frame to hold them the way Server::handleClient's local `buffer` did.
    struct Connection {
        enum class State { Reading, Writing };

        explicit Connection(int fd) : sock(fd) {}

        Socket sock;             // owns client_fd; closes it on erase from the map
        std::string inBuf;       // bytes received but not yet parsed into a request
        std::string outBuf;      // the response being sent
        size_t outSent = 0;      // how much of outBuf has been written so far
        bool keepAlive = false;  // decided when the request was parsed; used once outBuf is fully sent
        State state = State::Reading;
        uint32_t interest = 0;   // events currently registered with epoll for this fd
    };

    void acceptNewConnections();
    void onReadable(int fd);
    void onWritable(int fd);

    // Tries to send whatever of Connection::outBuf hasn't gone out yet.
    // Called both right after a response is built (so the common case --
    // the whole thing fits in one send() -- never has to wait for an
    // epoll_wait() round trip at all) and again from onWritable() if an
    // earlier attempt only completed partially.
    void flushOutput(int fd);

    void closeConnection(int fd);
    void setEpollInterest(int fd, uint32_t events);

    int port_;
    RequestHandler handler_;
    Socket listen_sock_;
    int epoll_fd_;

    std::unordered_map<int, Connection> connections_;
};
