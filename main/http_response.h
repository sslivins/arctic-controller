// Length of a complete HTTP/1.1 response, for a client that must read it to
// the end without relying on the server closing the connection.
//
// The web server's self-probe needs this: closing a socket with unread bytes
// makes lwIP answer with a RST, which the server logs as
// "httpd_sock_err: error in send/recv : 104" on every probe. And esp_http_server
// ignores "Connection: close", so reading until EOF would wait out the timeout.

#pragma once

#include <cstddef>

namespace http_response_detail {
inline bool starts_with_nocase(const char* s, const char* lower, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != lower[i]) {
            return false;
        }
    }
    return true;
}
}  // namespace http_response_detail

// Returns the total length (headers + body) of the response at the start of
// buf, or 0 if the headers are not complete yet. A response without a
// Content-Length header is treated as having no body.
inline size_t http_response_length(const char* buf, size_t len)
{
    size_t header_end = 0;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            header_end = i + 4;
            break;
        }
    }
    if (header_end == 0) {
        return 0;
    }
    static const char name[] = "content-length:";
    const size_t name_len = sizeof(name) - 1;
    size_t body = 0;
    // Each header line starts right after a CRLF; the status line is skipped.
    for (size_t i = 0; i + 2 + name_len <= header_end; i++) {
        if (buf[i] != '\r' || buf[i + 1] != '\n' ||
            !http_response_detail::starts_with_nocase(buf + i + 2, name, name_len)) {
            continue;
        }
        size_t p = i + 2 + name_len;
        while (p < header_end && (buf[p] == ' ' || buf[p] == '\t')) {
            p++;
        }
        while (p < header_end && buf[p] >= '0' && buf[p] <= '9') {
            body = body * 10 + (size_t)(buf[p] - '0');
            p++;
        }
        break;
    }
    return header_end + body;
}
