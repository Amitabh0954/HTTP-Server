#include "HttpRequest.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

std::optional<HttpRequest> HttpRequest::parse(const std::string& raw) {
    // Split into lines on "\r\n". HTTP requires \r\n specifically (not just
    // \n) as the line terminator, so we split on the two-char sequence
    // rather than std::getline (which only knows about \n).
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t end = raw.find("\r\n", start);
        if (end == std::string::npos) {
            break;
        }
        lines.push_back(raw.substr(start, end - start));
        start = end + 2;
        if (lines.back().empty()) {
            // The blank line marks the end of the header block.
            break;
        }
    }

    if (lines.empty()) {
        return std::nullopt;
    }

    // Request line looks like: "GET /path HTTP/1.1"
    std::istringstream request_line(lines[0]);
    HttpRequest req;
    if (!(request_line >> req.method >> req.path >> req.version)) {
        return std::nullopt; // missing one of the three tokens
    }

    // Remaining lines (except the trailing blank one) are "Name: value" pairs.
    for (size_t i = 1; i < lines.size() && !lines[i].empty(); ++i) {
        const std::string& line = lines[i];
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue; // malformed header line -- skip rather than fail the whole request
        }
        std::string name = toLower(line.substr(0, colon));

        // Skip the colon and any single space that conventionally follows it
        // (RFC 7230 allows optional whitespace after the colon).
        size_t value_start = colon + 1;
        while (value_start < line.size() && line[value_start] == ' ') {
            ++value_start;
        }
        req.headers[name] = line.substr(value_start);
    }

    return req;
}

bool HttpRequest::keepAlive() const {
    auto it = headers.find("connection");
    if (it != headers.end()) {
        return toLower(it->second) == "keep-alive";
    }
    return version == "HTTP/1.1";
}
