#include "RequestHandler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

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

} // namespace

RequestHandler::RequestHandler(const std::string& docRoot) {
    // Thrown deliberately (not error_code): this runs once at startup, not
    // per-request, so a bad docRoot should fail loudly and immediately.
    try {
        docRoot_ = fs::canonical(docRoot);
    } catch (const fs::filesystem_error& e) {
        std::fprintf(stderr, "Invalid document root '%s': %s\n", docRoot.c_str(), e.what());
        std::exit(EXIT_FAILURE);
    }
}

HttpResponse RequestHandler::handle(const std::string& requestPath) const {
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
