#pragma once

#include "HttpResponse.hpp"

#include <filesystem>
#include <string>

// Resolves a request path to a file-backed HttpResponse under a document
// root: MIME-type detection, binary-safe reads, 404/403, and directory-
// traversal protection.
//
// This is shared by BOTH concurrency models (the thread-pool Server and
// the epoll EpollServer) rather than duplicated in each. The traversal
// check in particular is security-critical -- having it in exactly one
// place means there's only one place it can be wrong, and fixing it once
// fixes it for every server that uses this class.
class RequestHandler {
public:
    explicit RequestHandler(const std::string& docRoot);

    HttpResponse handle(const std::string& requestPath) const;

private:
    std::filesystem::path docRoot_; // canonical (absolute, symlink-resolved)
};
