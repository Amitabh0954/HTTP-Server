#include "Server.hpp"

#include "HttpRequest.hpp"
#include "HttpResponse.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

constexpr int kBacklog = 16;
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

std::string mimeTypeFor(const std::string& extension) {
    static const std::unordered_map<std::string, std::string> kMimeTypes = {
        {".html", "text/html"},
        {".htm", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".ico", "image/x-icon"},
    };

    auto it = kMimeTypes.find(extension);
    return it != kMimeTypes.end() ? it->second : "application/octet-stream";
}

HttpResponse makeErrorResponse(int code, const std::string& text) {
    HttpResponse response(code, text);
    response.setHeader("Content-Type", "text/plain");
    response.setBody(text);
    return response;
}

// Decides whether the connection should stay open after this response.
// HTTP/1.1 defaults to persistent connections unless told otherwise;
// HTTP/1.0 defaults to closing unless told otherwise -- an explicit
// Connection header always overrides the version's default.
bool wantsKeepAlive(const HttpRequest& request) {
    auto it = request.headers.find("connection");
    if (it != request.headers.end()) {
        std::string value = it->second;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return value == "keep-alive";
    }
    return request.version == "HTTP/1.1";
}

} // namespace

Server::Server(int port, const std::string& docRoot, size_t numThreads)
    : port_(port),
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

    // Thrown deliberately (not error_code): this runs once at startup, not
    // per-request, so a bad docRoot should fail loudly and immediately.
    try {
        docRoot_ = fs::canonical(docRoot);
    } catch (const fs::filesystem_error& e) {
        std::fprintf(stderr, "Invalid document root '%s': %s\n", docRoot.c_str(), e.what());
        std::exit(EXIT_FAILURE);
    }
}

void Server::run() {
    std::printf("Listening on port %d, serving %s\n", port_, docRoot_.c_str());

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

        std::printf("[%s] %s %s %s\n", request->method.c_str(),
                    request->path.c_str(), request->version.c_str(),
                    wantsKeepAlive(*request) ? "keep-alive" : "close");

        bool keep_alive = wantsKeepAlive(*request);

        HttpResponse response = serveFile(request->path);
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

HttpResponse Server::serveFile(const std::string& requestPath) const {
    // Drop a query string ("/page?x=1" -> "/page") -- we don't support
    // query parameters yet, and leaving them on would make every request
    // resolve to a nonexistent file.
    std::string cleanPath = requestPath.substr(0, requestPath.find('?'));

    if (cleanPath == "/") {
        cleanPath = "/index.html";
    }

    // fs::path's operator/ DISCARDS the left-hand side entirely if the
    // right-hand side is an absolute path (e.g. path("www") / path("/etc")
    // == "/etc", not "www/etc"). Every HTTP path starts with '/', so we
    // must strip that leading slash before appending onto docRoot_ --
    // otherwise every request would silently resolve from the filesystem
    // root instead of our document root, bypassing the traversal check
    // entirely (it would just never trigger, not because paths are safe,
    // but because we'd never actually be joining against docRoot_).
    fs::path relative(cleanPath.substr(1));
    fs::path requested = docRoot_ / relative;

    // weakly_canonical resolves ".." and symlinks in whatever prefix of the
    // path actually exists, and lexically normalizes the rest -- unlike
    // fs::canonical, it does NOT throw/fail if the final file is missing,
    // which is exactly what we need: we want traversal detection to work
    // even for a path that happens not to exist.
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(requested, ec);
    if (ec) {
        return makeErrorResponse(404, "Not Found");
    }

    // Traversal guard: compare path COMPONENT BY COMPONENT, not as raw
    // strings. A naive canonical.string().starts_with(docRoot_.string())
    // would wrongly accept "/var/www_evil" as being "inside" "/var/www" --
    // component comparison respects path boundaries instead of just bytes.
    auto mismatch_pair = std::mismatch(docRoot_.begin(), docRoot_.end(),
                                        canonical.begin(), canonical.end());
    if (mismatch_pair.first != docRoot_.end()) {
        std::fprintf(stderr, "Blocked traversal attempt: %s\n", requestPath.c_str());
        return makeErrorResponse(403, "Forbidden");
    }

    if (!fs::is_regular_file(canonical, ec) || ec) {
        return makeErrorResponse(404, "Not Found");
    }

    std::ifstream file(canonical, std::ios::binary);
    if (!file) {
        return makeErrorResponse(404, "Not Found");
    }

    // rdbuf() streaming reads the whole file as raw bytes -- no text-mode
    // translation, so this is safe for binary content like PNGs, not just
    // text files.
    std::ostringstream contents;
    contents << file.rdbuf();

    HttpResponse response(200, "OK");
    response.setHeader("Content-Type", mimeTypeFor(canonical.extension().string()));
    response.setBody(contents.str());
    return response;
}
