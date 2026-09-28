#include "device_name.h"

#include "app_preferences.h"
#include "fonts/fonts.h"

#include <ctype.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

static bool decode_utf8(const unsigned char* s, uint32_t* cp, int* len)
{
    if (s[0] < 0x80) {
        *cp = s[0];
        *len = 1;
        return true;
    }
    if ((s[0] & 0xE0) == 0xC0) {
        if ((s[1] & 0xC0) != 0x80) return false;
        *cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *len = 2;
        return *cp >= 0x80;
    }
    if ((s[0] & 0xF0) == 0xE0) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return false;
        *cp = ((uint32_t)(s[0] & 0x0F) << 12) |
              ((uint32_t)(s[1] & 0x3F) << 6) |
              (s[2] & 0x3F);
        *len = 3;
        return *cp >= 0x800 && (*cp < 0xD800 || *cp > 0xDFFF);
    }
    if ((s[0] & 0xF8) == 0xF0) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80) {
            return false;
        }
        *cp = ((uint32_t)(s[0] & 0x07) << 18) |
              ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) |
              (s[3] & 0x3F);
        *len = 4;
        return *cp >= 0x10000 && *cp <= 0x10FFFF;
    }
    return false;
}

static bool is_control(uint32_t cp)
{
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
}

static bool font_has_glyph(uint32_t cp)
{
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(&montserrat_32_latin, &dsc, cp, 0) &&
           !dsc.is_placeholder;
}

static void set_error(char* error, size_t error_len, const char* message)
{
    if (!error || error_len == 0) return;
    snprintf(error, error_len, "%s", message);
}

bool device_name_validate_and_normalize(const char* input,
                                        char* output,
                                        size_t output_len,
                                        char* error,
                                        size_t error_len)
{
    if (!input || !output || output_len == 0) {
        set_error(error, error_len, "Controller name must be a string");
        return false;
    }

    const char* start = input;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    const char* end = input + strlen(input);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }

    const unsigned char* s = (const unsigned char*)start;
    const unsigned char* stop = (const unsigned char*)end;
    size_t out_pos = 0;
    int codepoints = 0;

    while (s < stop) {
        uint32_t cp = 0;
        int len = 0;
        if (!decode_utf8(s, &cp, &len) || s + len > stop) {
            set_error(error, error_len, "Controller name must be valid UTF-8");
            return false;
        }
        if (is_control(cp)) {
            set_error(error, error_len, "Controller name cannot contain control characters");
            return false;
        }
        if (!font_has_glyph(cp)) {
            set_error(error, error_len, "Controller name contains a character this display cannot show");
            return false;
        }
        codepoints++;
        if (codepoints > APP_PREFS_DEVICE_NAME_MAX_CODEPOINTS) {
            set_error(error, error_len, "Controller name must be 30 characters or fewer");
            return false;
        }
        if (out_pos + (size_t)len >= output_len) {
            set_error(error, error_len, "Controller name is too long");
            return false;
        }
        memcpy(output + out_pos, s, len);
        out_pos += len;
        s += len;
    }

    output[out_pos] = '\0';
    if (error && error_len > 0) error[0] = '\0';
    return true;
}
