#include "Socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kPort = 8080;
constexpr int kBacklog = 16;     // max pending connections queued by the OS
constexpr size_t kBufSize = 4096;

// Wraps a syscall-failure pattern we repeat a lot: print errno's message
// and exit. errno is only meaningful immediately after the failing call,
// which is why every call site checks its return value right away instead
// of batching checks later.
[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "%s: %s\n", what, std::strerror(errno));
    std::exit(EXIT_FAILURE);
}

} // namespace

int main() {
    // AF_INET: IPv4. SOCK_STREAM: TCP (reliable, ordered byte stream).
    // The third arg (protocol) is 0 to mean "the default for this
    // family+type", which for SOCK_STREAM/AF_INET is TCP.
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        die("socket");
    }
    Socket listen_sock(listen_fd); // now owned; will close() on any exit path

    // SO_REUSEADDR: without this, restarting the server right after it
    // exits fails with "Address already in use". When a TCP connection
    // closes, the port lingers in TIME_WAIT for a while (to catch
    // stray packets from the old connection) and the kernel refuses to
    // rebind it by default. This flag says "let me rebind anyway" — safe
    // for a server we're actively developing and restarting constantly.
    int opt = 1;
    if (setsockopt(listen_sock.fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        die("setsockopt");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;   // listen on all local interfaces
    addr.sin_port = htons(kPort);        // host byte order -> network byte order

    // bind() fails (EADDRINUSE) if something else already holds this port,
    // or (EACCES) if the port is <1024 and we're not root.
    if (bind(listen_sock.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        die("bind");
    }

    // listen() switches the socket into "passive" mode: it will now accept
    // incoming connections instead of being used to initiate one. kBacklog
    // bounds how many fully-established connections can queue up waiting
    // for us to accept() them before the kernel starts refusing new ones.
    if (listen(listen_sock.fd(), kBacklog) == -1) {
        die("listen");
    }

    std::printf("Listening on port %d...\n", kPort);

    // accept() blocks (sleeps the calling thread) until a client completes
    // the TCP handshake, then returns a NEW fd representing that one
    // connection. The original listen_fd keeps listening for further
    // clients — but in Phase 1 we only handle a single client and stop.
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_sock.fd(),
                           reinterpret_cast<sockaddr*>(&client_addr),
                           &client_len);
    if (client_fd == -1) {
        die("accept");
    }
    Socket client_sock(client_fd);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    std::printf("Client connected: %s:%d\n", client_ip, ntohs(client_addr.sin_port));

    // Echo loop. recv() returns however many bytes happen to be available
    // right now — TCP is a byte STREAM, not a sequence of messages, so one
    // send() on the client side is not guaranteed to arrive in one recv()
    // here. We must loop until the client closes the connection.
    char buf[kBufSize];
    for (;;) {
        ssize_t n = recv(client_sock.fd(), buf, sizeof(buf), 0);
        if (n == -1) {
            die("recv");
        }
        if (n == 0) {
            // Peer performed an orderly shutdown (closed their side).
            std::printf("Client disconnected.\n");
            break;
        }

        // send() can also write fewer bytes than requested (e.g. if the
        // socket's send buffer is full), so it must be looped too.
        ssize_t total_sent = 0;
        while (total_sent < n) {
            ssize_t sent = send(client_sock.fd(), buf + total_sent, n - total_sent, 0);
            if (sent == -1) {
                die("send");
            }
            total_sent += sent;
        }
    }

    return 0;
}
