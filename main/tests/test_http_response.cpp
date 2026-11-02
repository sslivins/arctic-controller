// Host tests for http_response_length: how much of a response the web
// server's self-probe reads before closing, so it never closes on unread bytes.

#include <cstdio>
#include <cstring>

#include "http_response.h"

static int g_failures = 0;

static void expect(const char* what, const char* buf, size_t len, size_t want) {
    const size_t got = http_response_length(buf, len);
    if (got != want) {
        std::fprintf(stderr, "%s: got %zu, want %zu\n", what, got, want);
        g_failures++;
    }
}

int main() {
    static const char full[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: 15\r\n\r\n{\"status\":\"ok\"}";
    const size_t n = sizeof(full) - 1;
    expect("whole response", full, n, n);
    // Only part of the body has arrived: the total is already known.
    expect("partial body", full, n - 5, n);
    // Headers still arriving: keep reading.
    expect("partial headers", full, 30, 0);
    expect("empty", full, 0, 0);

    static const char lower[] = "HTTP/1.1 200 OK\r\ncontent-length:3\r\n\r\nabc";
    expect("lower-case header, no space", lower, sizeof(lower) - 1, sizeof(lower) - 1);

    static const char none[] = "HTTP/1.1 204 No Content\r\nX-A: b\r\n\r\n";
    expect("no Content-Length", none, sizeof(none) - 1, sizeof(none) - 1);

    // A header that merely ends in content-length is not it.
    static const char other[] = "HTTP/1.1 200 OK\r\nX-Content-Length: 99\r\n\r\n";
    expect("other header", other, sizeof(other) - 1, sizeof(other) - 1);

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("http_response: all tests passed\n");
    return 0;
}
