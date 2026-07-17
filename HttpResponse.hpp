#pragma once

#include <string>
#include <utility>
#include <vector>

// Builds the raw bytes of an HTTP response. Content-Length is always
// computed from the body automatically, rather than trusted to the
// caller, so it can never drift out of sync with the actual body size.
class HttpResponse {
public:
    HttpResponse(int status_code, std::string status_text);

    void setHeader(std::string name, std::string value);
    void setBody(std::string body);

    // Serializes to: "HTTP/1.1 <code> <text>\r\n" + headers + "\r\n" + body
    std::string toString() const;

private:
    int status_code_;
    std::string status_text_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
};
