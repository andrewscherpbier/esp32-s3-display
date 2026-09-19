#pragma once

typedef struct net_http_session *net_http_session_t;

/**
 * HTTPS GET `url` into `buf` (certificates checked against ESP-IDF's CA bundle). The
 * body is NUL-terminated for convenience; binary bodies (images) work too. Returns the
 * body length, or -1 on a connection failure, a non-200 status, or a body that doesn't
 * fit in `size - 1` bytes. Blocks; call from a task that can wait up to ~10 s.
 *
 * Opens and closes a connection per call. For repeated requests, use a session.
 */
int net_https_get(const char *url, char *buf, int size);

/*
 * A session keeps its TLS connection open between requests to the same host, which saves
 * the 2-3 s handshake each time. A session belongs to one task at a time.
 */
net_http_session_t net_http_session_create(void);
int net_http_session_get(net_http_session_t session, const char *url, char *buf, int size);
void net_http_session_destroy(net_http_session_t session);
