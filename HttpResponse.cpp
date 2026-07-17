#include "HttpResponse.hpp"

HttpResponse::HttpResponse(int status_code, std::string status_text)
    : status_code_(status_code), status_text_(std::move(status_text)) {}

void HttpResponse::setHeader(std::string name, std::string value) {
    headers_.emplace_back(std::move(name), std::move(value));
}

void HttpResponse::setBody(std::string body) {
    body_ = std::move(body);
}

std::string HttpResponse::toString() const {
    std::string out = "HTTP/1.1 " + std::to_string(status_code_) + " " + status_text_ + "\r\n";

    for (const auto& [name, value] : headers_) {
        out += name + ": " + value + "\r\n";
    }

    // Computed here, not settable via setHeader, so it always matches body_.
    out += "Content-Length: " + std::to_string(body_.size()) + "\r\n";
    out += "\r\n";
    out += body_;
    return out;
}
