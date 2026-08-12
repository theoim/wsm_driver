/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Request-line parsing and response headers (see http_core.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_core.h"

int http_parse_request(const char *line, http_request_t *req)
{
    if (strncmp(line, "GET ", 4) != 0) {
        return -1;
    }
    const char *target = line + 4;

    /* The target ends at the space before the version, or at the end of the
     * line for an HTTP/0.9-style request. */
    const char *end = strpbrk(target, " \r\n");
    if (end == NULL) {
        end = target + strlen(target);
    }

    const char *question = memchr(target, '?', (size_t)(end - target));
    const char *path_end = (question != NULL) ? question : end;

    size_t path_len = (size_t)(path_end - target);
    if (path_len == 0 || path_len >= HTTP_PATH_MAX) {
        return -1;
    }
    memcpy(req->path, target, path_len);
    req->path[path_len] = '\0';

    req->query[0] = '\0';
    if (question != NULL) {
        size_t query_len = (size_t)(end - question - 1);
        if (query_len >= HTTP_QUERY_MAX) {
            return -1;
        }
        memcpy(req->query, question + 1, query_len);
        req->query[query_len] = '\0';
    }
    return 0;
}

/* Find "key=" at the start of the query or just after an '&', so that looking
 * for "v" does not match the "v" inside "srv=1". */
static const char *find_value(const char *query, const char *key)
{
    size_t key_len = strlen(key);

    for (const char *p = query; *p != '\0'; ) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            return p + key_len + 1;
        }
        const char *amp = strchr(p, '&');
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    return NULL;
}

int http_query_int(const char *query, const char *key, int fallback)
{
    const char *value = find_value(query, key);
    if (value == NULL) {
        return fallback;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return (int)parsed;
}

int http_query_str(const char *query, const char *key, char *out, size_t size)
{
    const char *value = find_value(query, key);
    if (value == NULL) {
        return -1;
    }

    const char *end = strchr(value, '&');
    size_t len = (end != NULL) ? (size_t)(end - value) : strlen(value);
    if (len >= size) {
        return -1;
    }
    memcpy(out, value, len);
    out[len] = '\0';
    return 0;
}

int http_header(char *buf, size_t size, const char *status,
                const char *content_type, int content_length,
                const char *extra)
{
    int n = snprintf(buf, size, "HTTP/1.1 %s\r\n", status);
    if (n < 0 || (size_t)n >= size) {
        return -1;
    }

    if (content_type != NULL) {
        n += snprintf(buf + n, size - n, "Content-Type: %s\r\n", content_type);
    }
    if (content_length >= 0) {
        n += snprintf(buf + n, size - n, "Content-Length: %d\r\n",
                      content_length);
    }
    if (extra != NULL) {
        n += snprintf(buf + n, size - n, "%s", extra);
    }

    /* One request per connection, so say so rather than leaving a browser to
     * guess: without it a client may hold the socket open waiting for a second
     * response, and on the TOE that socket is a listener the server needs back. */
    n += snprintf(buf + n, size - n, "Connection: close\r\n\r\n");

    return (n > 0 && (size_t)n < size) ? n : -1;
}
