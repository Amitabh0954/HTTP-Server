#pragma once

// RAII wrapper around a raw file descriptor for a socket.
// Why: every syscall that can fail (bind, listen, accept...) has an early
// return on error. Without RAII you'd have to remember to close(fd) on
// every one of those paths. The destructor guarantees it happens exactly
// once, no matter how the function exits.
class Socket {
public:
    // Takes ownership of an fd that was already created by socket()/accept().
    // fd == -1 represents "no socket" (e.g. a moved-from Socket).
    explicit Socket(int fd);

    ~Socket();

    // Not copyable: two Socket objects must never both think they own the
    // same fd, or we'd double-close() it.
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Movable: ownership of the fd can be transferred (e.g. returning a
    // Socket from a function, or storing one in a container).
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    int fd() const { return fd_; }

private:
    int fd_;
};
