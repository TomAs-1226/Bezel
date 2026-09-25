/* The simulator's network half of hal.h: a real HTTP/1.1 client (plain sockets, OpenSSL for https),
 * threads, and stand-ins for what only the tablet has — the USB tether, mDNS, H.264 clips.
 *
 * SIM_TETHER=1 pretends Systemcore is on the USB-A port (so the UI's tether states can be seen);
 * SIM_LINK=host:port answers an mDNS browse for _catalyst-link. */
#include "hal.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- threads ---- */
bool hal_thread(const char *name, void *(*fn)(void *), void *arg, int stack)
{
    (void)name;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, stack < 65536 ? 65536 : (size_t)stack);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    bool ok = pthread_create(&th, &at, fn, arg) == 0;
    pthread_attr_destroy(&at);
    return ok;
}

bool hal_thread_internal(const char *name, void *(*fn)(void *), void *arg, int stack)
{
    return hal_thread(name, fn, arg, stack);
}

/* ---- tether, mDNS ---- */
void hal_tether(hal_tether_t *o)
{
    memset(o, 0, sizeof *o);
    const char *e = getenv("SIM_TETHER");
    if (!e || !*e || *e == '0') return;
    o->present = o->up = o->dhcp = true;
    snprintf(o->kind, sizeof o->kind, "ncm");
    snprintf(o->ip, sizeof o->ip, "172.26.0.100");
    snprintf(o->gw, sizeof o->gw, "172.26.0.1");
    snprintf(o->mask, sizeof o->mask, "255.255.255.0");
    static const uint8_t mac[6] = { 0x02, 0x5c, 0x0e, 0x00, 0x00, 0x01 };
    memcpy(o->mac, mac, 6);
    double t = hal_seconds();
    o->rx_bytes = (uint64_t)(t * 41000.0);
    o->tx_bytes = (uint64_t)(t * 6200.0);
    o->mbps = 480;
}

void hal_tether_fallback(const char *ip, const char *mask) { (void)ip; (void)mask; }

int hal_mdns_browse(const char *service, const char *proto, hal_service_t *out, int max, int timeout_ms)
{
    (void)proto;
    (void)timeout_ms;
    const char *e = getenv("SIM_LINK");
    if (!e || max < 1 || strcmp(service, "_catalyst-link") != 0) return 0;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "sim-pc");
    snprintf(out->host, sizeof out->host, "%s", e);
    char *c = strchr(out->host, ':');
    out->port = 8765;
    if (c) {
        *c = 0;
        out->port = atoi(c + 1);
    }
    snprintf(out->ip, sizeof out->ip, "%s", out->host);
    return 1;
}

/* ---- clips: a stand-in file, so the flow (record, list, send to the PC) can be exercised ---- */
static FILE *g_clip;
static double g_clip_t0, g_clip_max;
bool hal_clip_start(const char *path, double max_s)
{
    if (g_clip) return false;
    g_clip = fopen(path, "wb");
    if (!g_clip) return false;
    g_clip_t0 = hal_seconds();
    g_clip_max = max_s;
    return true;
}

double hal_clip_stop(void)
{
    if (!g_clip) return 0;
    double s = hal_seconds() - g_clip_t0;
    if (s > g_clip_max) s = g_clip_max;
    fprintf(g_clip, "simulated H.264 clip, %.1f s\n", s);
    fclose(g_clip);
    g_clip = NULL;
    return s;
}

bool hal_clip_active(double *seconds)
{
    if (!g_clip) return false;
    double s = hal_seconds() - g_clip_t0;
    if (s >= g_clip_max) {
        hal_clip_stop();
        return false;
    }
    if (seconds) *seconds = s;
    return true;
}

/* ---- HTTP ---- */
struct hal_http {
    int fd;
    SSL *ssl;
    char buf[8192];
    int pos, len;                /* buffered bytes not yet handed out */
    bool chunked, done;
    long remaining;              /* in the current chunk, or of a Content-Length body; -1 unknown */
    char last_modified[40];
};

static SSL_CTX *g_ssl;
static pthread_once_t g_ssl_once = PTHREAD_ONCE_INIT;
static void ssl_init(void)
{
    g_ssl = SSL_CTX_new(TLS_client_method());
    if (!g_ssl) return;
    SSL_CTX_set_verify(g_ssl, SSL_VERIFY_PEER, NULL);
    const char *bundle = getenv("SSL_CERT_FILE");
    if (!bundle || SSL_CTX_load_verify_locations(g_ssl, bundle, NULL) != 1) SSL_CTX_set_default_verify_paths(g_ssl);
}

static int raw_read(hal_http_t *h, char *b, int n)
{
    if (h->ssl) {
        int r = SSL_read(h->ssl, b, n);
        if (r > 0) return r;
        int e = SSL_get_error(h->ssl, r);
        return e == SSL_ERROR_ZERO_RETURN ? 0 : -1;
    }
    for (;;) {
        ssize_t r = recv(h->fd, b, (size_t)n, 0);
        if (r < 0 && errno == EINTR) continue;
        return (int)r;
    }
}

static bool raw_write(hal_http_t *h, const char *b, size_t n)
{
    while (n) {
        int r = h->ssl ? SSL_write(h->ssl, b, (int)n) : (int)send(h->fd, b, n, MSG_NOSIGNAL);
        if (r <= 0) return false;
        b += r;
        n -= (size_t)r;
    }
    return true;
}

/* refills the buffer; false at EOF or error */
static bool fill(hal_http_t *h)
{
    if (h->pos < h->len) return true;
    int r = raw_read(h, h->buf, sizeof h->buf);
    if (r <= 0) return false;
    h->pos = 0;
    h->len = r;
    return true;
}

static int getline_crlf(hal_http_t *h, char *out, int max)
{
    int n = 0;
    for (;;) {
        if (!fill(h)) return n ? n : -1;
        char c = h->buf[h->pos++];
        if (c == '\n') break;
        if (c != '\r' && n < max - 1) out[n++] = c;
    }
    out[n] = 0;
    return n;
}

static void set_err(char *err, size_t n, const char *m)
{
    if (err && n) snprintf(err, n, "%s", m);
}

hal_http_t *hal_http_open(const hal_http_req_t *req, int *status, char *err, size_t errn)
{
    if (status) *status = -1;
    const char *u = req->url;
    bool tls = false;
    if (!strncmp(u, "https://", 8)) {
        tls = true;
        u += 8;
    } else if (!strncmp(u, "http://", 7)) {
        u += 7;
    } else {
        set_err(err, errn, "bad url");
        return NULL;
    }
    char host[128], port[8];
    const char *slash = strchr(u, '/');
    const char *path = slash ? slash : "/";
    size_t hl = slash ? (size_t)(slash - u) : strlen(u);
    if (hl >= sizeof host) hl = sizeof host - 1;
    memcpy(host, u, hl);
    host[hl] = 0;
    char *colon = strchr(host, ':');
    snprintf(port, sizeof port, "%s", tls ? "443" : "80");
    if (colon) {
        *colon = 0;
        snprintf(port, sizeof port, "%s", colon + 1);
    }

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) {
        set_err(err, errn, "no such host");
        return NULL;
    }
    int timeout = req->timeout_ms > 0 ? req->timeout_ms : 10000;
    int fd = -1;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { timeout / 1000, (timeout % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) {
        set_err(err, errn, "can't connect");
        return NULL;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    hal_http_t *h = calloc(1, sizeof *h);
    h->fd = fd;
    h->remaining = -1;
    if (tls) {
        pthread_once(&g_ssl_once, ssl_init);
        h->ssl = g_ssl ? SSL_new(g_ssl) : NULL;
        if (!h->ssl) {
            set_err(err, errn, "no TLS");
            hal_http_close(h);
            return NULL;
        }
        SSL_set_fd(h->ssl, fd);
        SSL_set_tlsext_host_name(h->ssl, host);
        SSL_set1_host(h->ssl, host);
        if (SSL_connect(h->ssl) != 1) {
            set_err(err, errn, "TLS handshake failed");
            hal_http_close(h);
            return NULL;
        }
    }

    char head[1024];
    int n = snprintf(head, sizeof head,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: catalyst-tab\r\nConnection: close\r\n"
                     "Content-Length: %zu\r\n",
                     req->method ? req->method : "GET", path, host, req->body ? req->body_len : 0);
    if (!raw_write(h, head, (size_t)n) || (req->headers && !raw_write(h, req->headers, strlen(req->headers))) ||
        !raw_write(h, "\r\n", 2) || (req->body && req->body_len && !raw_write(h, req->body, req->body_len))) {
        set_err(err, errn, "send failed");
        hal_http_close(h);
        return NULL;
    }

    char line[1024];
    if (getline_crlf(h, line, sizeof line) < 0 || strncmp(line, "HTTP/1.", 7) != 0) {
        set_err(err, errn, "no response");
        hal_http_close(h);
        return NULL;
    }
    int st = atoi(line + 9);
    for (;;) {
        int l = getline_crlf(h, line, sizeof line);
        if (l < 0) {
            set_err(err, errn, "truncated head");
            hal_http_close(h);
            return NULL;
        }
        if (l == 0) break;
        if (!strncasecmp(line, "transfer-encoding:", 18) && strcasestr(line, "chunked")) h->chunked = true;
        else if (!strncasecmp(line, "content-length:", 15)) h->remaining = atol(line + 15);
        else if (!strncasecmp(line, "last-modified:", 14)) {
            const char *v = line + 14;
            while (*v == ' ' || *v == '	') v++;
            snprintf(h->last_modified, sizeof h->last_modified, "%s", v);
        }
    }
    if (h->chunked) h->remaining = 0; /* read the first chunk size on demand */
    if (status) *status = st;
    return h;
}

int hal_http_read(hal_http_t *h, char *out, int max)
{
    if (!h || h->done) return 0;
    if (h->chunked && h->remaining == 0) {
        char line[64];
        int l = getline_crlf(h, line, sizeof line);
        if (l == 0) l = getline_crlf(h, line, sizeof line); /* the CRLF ending the previous chunk */
        if (l < 0) return -1;
        h->remaining = strtol(line, NULL, 16);
        if (h->remaining == 0) {
            h->done = true;
            return 0;
        }
    }
    if (!h->chunked && h->remaining == 0) {
        h->done = true;
        return 0;
    }
    if (!fill(h)) {
        h->done = true;
        return h->chunked || h->remaining > 0 ? -1 : 0;
    }
    int n = h->len - h->pos;
    if (n > max) n = max;
    if (h->remaining >= 0 && n > h->remaining) n = (int)h->remaining;
    memcpy(out, h->buf + h->pos, (size_t)n);
    h->pos += n;
    if (h->remaining > 0) h->remaining -= n;
    return n;
}

const char *hal_http_last_modified(const hal_http_t *h) { return h ? h->last_modified : ""; }

void hal_http_close(hal_http_t *h)
{
    if (!h) return;
    if (h->ssl) {
        SSL_shutdown(h->ssl);
        SSL_free(h->ssl);
    }
    if (h->fd >= 0) close(h->fd);
    free(h);
}
