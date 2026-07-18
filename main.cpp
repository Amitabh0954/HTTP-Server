#include "EpollServer.hpp"
#include "Server.hpp"

#include <cstring>

int main(int argc, char* argv[]) {
    // Both server implementations serve the exact same www/ document root
    // through the exact same RequestHandler -- only the concurrency model
    // (thread pool vs. single-threaded epoll) differs. This flag exists so
    // we can benchmark both from one binary and compare like-for-like.
    bool useEpoll = (argc > 1 && std::strcmp(argv[1], "--epoll") == 0);

    if (useEpoll) {
        EpollServer server(8080, "www");
        server.run();
    } else {
        Server server(8080, "www");
        server.run();
    }
}
