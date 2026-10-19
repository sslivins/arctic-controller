/*
 * Arctic Heat Pump Controller
 * DHCP hostname - the name routers (UniFi etc.) list the controller under.
 *
 * The controller's friendly name when it has one, else the same default Home
 * Assistant uses ("Macon Heat Pump Controller" + last 4 MAC hex digits).
 * Hostnames may only hold ASCII letters, digits and hyphens, so the name is
 * reduced to that: accented Latin letters lose their accent, and every other
 * run of characters becomes one hyphen ("Chalet Été #2" -> "Chalet-Ete-2").
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Longest hostname we produce (a DNS label allows 63).
static const size_t DHCP_HOSTNAME_MAX = 32;

// ASCII base letter for U+00C0..U+00FF, or 0 if none.
static inline char dhcp_hostname_latin1_base(uint32_t cp)
{
    static const char map[] =
        "AAAAAAACEEEEIIII"   // C0-CF
        "DNOOOOO\0OUUUUY\0s" // D0-DF (x, thorn: none)
        "aaaaaaaceeeeiiii"   // E0-EF
        "dnooooo\0ouuuuy\0y";// F0-FF
    if (cp < 0xC0 || cp > 0xFF) return 0;
    return map[cp - 0xC0];
}

// `out` must hold DHCP_HOSTNAME_MAX + 1 bytes.
static inline void dhcp_hostname_build(const char* friendly, const uint8_t mac[6], char* out,
                                       size_t out_len)
{
    size_t n = 0;
    bool hyphen = false;
    const unsigned char* s = (const unsigned char*)(friendly ? friendly : "");
    const size_t cap = out_len - 1 < DHCP_HOSTNAME_MAX ? out_len - 1 : DHCP_HOSTNAME_MAX;
    while (*s && n < cap) {
        char c = 0;
        if (*s < 0x80) {
            if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')) {
                c = (char)*s;
            }
            s++;
        } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            c = dhcp_hostname_latin1_base(((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F));
            s += 2;
        } else {
            s++;
            while ((*s & 0xC0) == 0x80) s++;
        }
        if (c) {
            if (hyphen && n > 0) {
                if (n + 2 > cap) break;  // no room for "-x": end at the word
                out[n++] = '-';
            }
            hyphen = false;
            out[n++] = c;
        } else {
            hyphen = true;
        }
    }
    out[n] = '\0';
    if (n == 0) {
        snprintf(out, out_len, "Macon-Heat-Pump-Controller-%02X%02X", mac[4], mac[5]);
    }
}
