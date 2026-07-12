#include "Socket.hpp"

#include <unistd.h>   // close()
#include <utility>    // std::exchange

Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket() {
    if (fd_ != -1) {
        close(fd_);
    }
}

Socket::Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (fd_ != -1) {
            close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}
