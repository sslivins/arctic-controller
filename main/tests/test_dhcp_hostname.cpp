// Host tests for dhcp_hostname_build: the name routers list the controller as.

#include <cstdio>
#include <cstring>

#include "dhcp_hostname.h"

static int g_failures = 0;

static const uint8_t MAC[6] = {0x30, 0xed, 0xa0, 0x11, 0x3f, 0x2a};

static void expect(const char* friendly, const char* want) {
    char out[DHCP_HOSTNAME_MAX + 1];
    dhcp_hostname_build(friendly, MAC, out, sizeof(out));
    if (std::strcmp(out, want) != 0) {
        std::fprintf(stderr, "'%s' -> '%s', want '%s'\n", friendly ? friendly : "(null)", out,
                     want);
        g_failures++;
    }
}

int main() {
    // No friendly name: Home Assistant's default name.
    expect("", "Macon-Heat-Pump-Controller-3F2A");
    expect(nullptr, "Macon-Heat-Pump-Controller-3F2A");
    // Nothing usable left after cleaning: same default.
    expect("   ", "Macon-Heat-Pump-Controller-3F2A");
    expect("\xE2\x98\x83", "Macon-Heat-Pump-Controller-3F2A");  // snowman

    expect("HP1", "HP1");
    expect("Heat Pump 1", "Heat-Pump-1");
    expect("  Sun Peaks -- HP2  ", "Sun-Peaks-HP2");
    expect("Chalet \xC3\x89t\xC3\xA9 #2", "Chalet-Ete-2");       // Été
    expect("Ni\xC3\xB1o's pump", "Nino-s-pump");                  // Niño
    expect("Stra\xC3\x9F" "e", "Strase");                         // ß
    expect("A\xE2\x98\x83" "B", "A-B");                           // emoji/symbol splits

    // 30 characters (the friendly-name limit) fit; longer ends at a word.
    expect("abcdefghijklmnopqrstuvwxyz0123", "abcdefghijklmnopqrstuvwxyz0123");
    expect("abcdefghijklmnopqrstuvwxyz012 3456", "abcdefghijklmnopqrstuvwxyz012-34");
    expect("abcdefghijklmnopqrstuvwxyz01234 5", "abcdefghijklmnopqrstuvwxyz01234");

    // A small buffer is respected.
    char small[6];
    dhcp_hostname_build("Heat Pump", MAC, small, sizeof(small));
    if (std::strcmp(small, "Heat") != 0) {
        std::fprintf(stderr, "small buffer -> '%s'\n", small);
        g_failures++;
    }

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dhcp_hostname: all tests passed\n");
    return 0;
}
