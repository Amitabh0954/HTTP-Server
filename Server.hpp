#pragma once

#include "RequestHandler.hpp"
#include "Socket.hpp"
#include "ThreadPool.hpp"

#include <cstddef>
#include <string>

// Listens on a TCP port and serves static files out of a document root.
// The accepting thread only accepts and hands connections off to a fixed
// pool of worker threads -- it never blocks handling a client itself, so a
// slow client can't stall new connections from being accepted.
class Server {
public:
    Server(int port, const std::string& docRoot, size_t numThreads = 4);

    // Binds, listens, then loops forever: accept -> submit to pool -> accept -> ...
    [[noreturn]] void run();

private:
    // Reads and responds to requests on client_fd, looping for as long as
    // the connection is kept alive. Runs on a worker thread. Takes
    // ownership of client_fd (wraps it in a Socket internally) so it's
    // guaranteed closed exactly once, on every exit path.
    void handleClient(int client_fd);

    int port_;
    RequestHandler handler_;
    Socket listen_sock_;
    ThreadPool pool_;
};
