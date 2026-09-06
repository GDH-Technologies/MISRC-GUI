/*
 * MISRC GUI - HTTP query-string helpers for the net mode.
 *
 * Pure functions with no socket or app dependency, so a guard can compile
 * and drive them standalone. The control protocol is GET-only with query
 * arguments (see gui_net.h), and the settings setter carries arbitrary
 * strings (paths with spaces, Windows backslashes) percent-encoded.
 */
#ifndef GUI_NET_QUERY_H
#define GUI_NET_QUERY_H

#include <stdbool.h>
#include <stddef.h>

/* Copy the raw (still encoded) value of key from a query string of the form
 * "a=1&b=two" into out. Returns false when the key is absent or out is too
 * small. An exact key match only: "value" does not match "xvalue". */
bool net_query_get(const char *query, const char *key, char *out, size_t cap);

/* Decode %XX escapes in place. A '+' stays a literal '+' (the encoder never
 * produces one for a space). Returns false, leaving s partially decoded, on
 * a malformed escape: a bare '%', a short one, or a non-hex digit. */
bool net_percent_decode(char *s);

/* Percent-encode in into out, keeping only the RFC 3986 unreserved set
 * (A-Z a-z 0-9 - _ . ~). Returns the length the full encoding needs, like
 * snprintf: a result >= cap means it was truncated. */
size_t net_percent_encode(const char *in, char *out, size_t cap);

#endif /* GUI_NET_QUERY_H */
