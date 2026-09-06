/*
 * Query-string helper harness: compiles misrc_gui/net/gui_net_query.c
 * standalone and checks the splitting, decoding and encoding the settings
 * setter relies on. A value that decodes wrongly lands in a setting, so the
 * malformed cases matter as much as the good ones.
 */
#include "gui_net_query.h"

#include <stdio.h>
#include <string.h>

static int s_fails = 0;

static void check(bool ok, const char *what) {
    if (ok) {
        printf("PASS: %s\n", what);
    } else {
        s_fails++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

int main(void) {
    char v[128];

    /* --- net_query_get ------------------------------------------------ */
    check(net_query_get("key=flac_level&value=6", "key", v, sizeof(v)) && strcmp(v, "flac_level") == 0, "first arg");
    check(net_query_get("key=flac_level&value=6", "value", v, sizeof(v)) && strcmp(v, "6") == 0, "second arg");
    check(net_query_get("value=%26&key=x", "value", v, sizeof(v)) && strcmp(v, "%26") == 0, "an encoded ampersand does not split the query");
    check(!net_query_get("xvalue=1", "value", v, sizeof(v)), "a longer key does not match a shorter one");
    check(!net_query_get("value", "value", v, sizeof(v)), "a key without '=' is not a value");
    check(net_query_get("a=&b=2", "a", v, sizeof(v)) && v[0] == '\0', "an empty value is found and empty");
    check(!net_query_get("a=1", "b", v, sizeof(v)), "a missing key is reported");
    check(!net_query_get("a=123456789", "a", v, 4), "an over-cap value is refused rather than truncated");
    check(!net_query_get(NULL, "a", v, sizeof(v)) && !net_query_get("a=1", NULL, v, sizeof(v)), "NULL inputs are refused");

    /* --- net_percent_decode ------------------------------------------ */
    strcpy(v, "%2Fmnt%2Fx9%20pro");
    check(net_percent_decode(v) && strcmp(v, "/mnt/x9 pro") == 0, "slashes and a space decode");
    strcpy(v, "C%3A%5CCaptures%5CTape%2001");
    check(net_percent_decode(v) && strcmp(v, "C:\\Captures\\Tape 01") == 0, "a Windows path decodes");
    strcpy(v, "a+b");
    check(net_percent_decode(v) && strcmp(v, "a+b") == 0, "'+' stays literal");
    strcpy(v, "caf%C3%A9");
    check(net_percent_decode(v) && strcmp(v, "caf\xc3\xa9") == 0, "UTF-8 bytes decode");
    strcpy(v, "%G1");
    check(!net_percent_decode(v), "a non-hex escape is refused");
    strcpy(v, "abc%2");
    check(!net_percent_decode(v), "a short trailing escape is refused");
    strcpy(v, "abc%");
    check(!net_percent_decode(v), "a bare trailing percent is refused");
    strcpy(v, "plain");
    check(net_percent_decode(v) && strcmp(v, "plain") == 0, "no escapes is a no-op");

    /* --- net_percent_encode ------------------------------------------ */
    char e[512];
    size_t n = net_percent_encode("/mnt/x9 pro", e, sizeof(e));
    check(n == strlen(e) && strcmp(e, "%2Fmnt%2Fx9%20pro") == 0, "encode keeps only the unreserved set");
    n = net_percent_encode("A-z_0.9~", e, sizeof(e));
    check(strcmp(e, "A-z_0.9~") == 0, "unreserved characters pass through");
    n = net_percent_encode("&=+", e, sizeof(e));
    check(strcmp(e, "%26%3D%2B") == 0, "query metacharacters are escaped");
    n = net_percent_encode("/mnt/x9 pro", e, 8);
    check(n == 17 && strlen(e) < 8, "a small buffer reports the needed length and never overruns");

    /* every byte round-trips */
    bool all_ok = true;
    for (int c = 1; c < 256 && all_ok; c++) {
        char one[2] = { (char)c, '\0' };
        char enc[8], dec[8];
        net_percent_encode(one, enc, sizeof(enc));
        strcpy(dec, enc);
        if (!net_percent_decode(dec) || strcmp(dec, one) != 0) all_ok = false;
    }
    check(all_ok, "every byte value survives encode -> decode");

    printf("%s\n", s_fails ? "NET QUERY HARNESS FAILED" : "net query harness passed");
    return s_fails ? 1 : 0;
}
