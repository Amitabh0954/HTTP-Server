#pragma once

#include <map>
#include <optional>
#include <string>

// A parsed HTTP request line + headers. Body parsing is deferred to a
// later phase -- GET requests (all we test with for now) don't have one.
struct HttpRequest {
    std::string method;   // e.g. "GET"
    std::string path;     // e.g. "/index.html"
    std::string version;  // e.g. "HTTP/1.1"

    // Header names are stored lowercased so lookups don't have to worry
    // about "Content-Type" vs "content-type" -- HTTP header names are
    // case-insensitive per RFC 7230.
    std::map<std::string, std::string> headers;

    // Parses a raw buffer that is known to already contain a full
    // request-line + headers block terminated by "\r\n\r\n" (the caller is
    // responsible for buffering recv() until that terminator appears).
    // Returns std::nullopt if the request line itself is malformed.
    static std::optional<HttpRequest> parse(const std::string& raw);

    // Whether the connection this request arrived on should stay open
    // afterwards. HTTP/1.1 defaults to persistent connections unless told
    // otherwise; HTTP/1.0 defaults to closing unless told otherwise -- an
    // explicit Connection header always overrides the version's default.
    bool keepAlive() const;
};
