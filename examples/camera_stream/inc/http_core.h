/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Just enough HTTP to serve a page, a stream and a handful of JSON endpoints.
 *
 * Not a general server, and the shape of what it refuses is the interesting
 * part: one request per connection, GET only, no chunked bodies, no keep-alive
 * negotiation. A camera stream is a single long-lived response, so the request
 * side never has to do more than this, and pulling in a full HTTP stack would
 * bury the thing the example is actually about.
 *
 * No sockets here. The server hands over the request line it read.
 */
#ifndef HTTP_CORE_H
#define HTTP_CORE_H

#include <stddef.h>
#include <stdint.h>

#define HTTP_PATH_MAX   96
#define HTTP_QUERY_MAX  96

typedef struct {
    char path[HTTP_PATH_MAX];    /* "/api/res", no query string */
    char query[HTTP_QUERY_MAX];  /* "v=640x480", no leading '?' */
} http_request_t;

/*
 * Parse a request line ("GET /api/res?v=640x480 HTTP/1.1").
 * Returns 0 on success, -1 if it is not a GET or does not fit.
 */
int http_parse_request(const char *line, http_request_t *req);

/*
 * Read one integer parameter out of a query string.
 * Returns the value, or `fallback` when the key is absent or unparsable --
 * a control the page did not send should keep its current value rather than
 * snapping to zero.
 */
int http_query_int(const char *query, const char *key, int fallback);

/*
 * Copy a string parameter into `out`. Returns 0 on success, -1 if the key is
 * absent or the value does not fit.
 */
int http_query_str(const char *query, const char *key, char *out, size_t size);

/*
 * Build a response header into `buf`. `content_type` may be NULL for 204.
 * A negative `content_length` omits the header, which is what a stream wants:
 * its length is not known and the connection close is the terminator.
 * Returns the header length, or -1 if it does not fit.
 */
int http_header(char *buf, size_t size, const char *status,
                const char *content_type, int content_length,
                const char *extra);

#endif /* HTTP_CORE_H */
