/*
 * MISRC GUI - HTTP query-string helpers. See gui_net_query.h.
 */
#include "gui_net_query.h"

#include <string.h>

bool net_query_get(const char *query, const char *key, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!query || !key || !key[0]) return false;
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = seg - klen - 1;
            if (vlen >= cap) return false;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool net_percent_decode(char *s) {
    if (!s) return false;
    char *w = s;
    for (const char *r = s; *r; ) {
        if (*r == '%') {
            int hi = r[1] ? hex_value(r[1]) : -1;
            int lo = (hi >= 0 && r[2]) ? hex_value(r[2]) : -1;
            if (hi < 0 || lo < 0) {
                *w = '\0';
                return false;
            }
            *w++ = (char)((hi << 4) | lo);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return true;
}

static bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

size_t net_percent_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t need = 0;
    if (out && cap) out[0] = '\0';
    if (!in) return 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (is_unreserved(*p)) {
            if (out && need + 1 < cap) { out[need] = (char)*p; out[need + 1] = '\0'; }
            need += 1;
        } else {
            if (out && need + 3 < cap) {
                out[need] = '%';
                out[need + 1] = hex[*p >> 4];
                out[need + 2] = hex[*p & 0xF];
                out[need + 3] = '\0';
            }
            need += 3;
        }
    }
    return need;
}
