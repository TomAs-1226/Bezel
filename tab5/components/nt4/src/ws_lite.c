#include "ws_lite.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

/* Bytes read off the socket but not yet parsed into a frame. */
#define RX_CAP (256 * 1024)
typedef struct {
    uint8_t buf[RX_CAP];
    size_t len;
} rx_t;

static uint32_t next_rand(ws_t *ws)
{
    /* xorshift32: the mask only has to be unpredictable to intermediaries, not cryptographic */
    uint32_t x = ws->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return ws->rng = x ? x : 0x9e3779b9u;
}

static void fail(ws_t *ws, const char *what)
{
    snprintf(ws->error, sizeof ws->error, "%s", what);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

static bool wait_fd(int fd, bool write, int timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &tv);
    return r > 0;
}

static bool send_all(ws_t *ws, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        if (!wait_fd(ws->fd, true, 2000)) { fail(ws, "send timeout"); return false; }
        ssize_t k = send(ws->fd, b, n, 0);
        if (k < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            fail(ws, "send failed");
            return false;
        }
        b += k;
        n -= (size_t)k;
        ws->tx_bytes += (uint64_t)k;
    }
    return true;
}

bool ws_connect(ws_t *ws, const char *host, int port, const char *path, const char *protocols,
                char *chosen, size_t chosen_len, int timeout_ms)
{
    uint32_t seed = ws->rng;
    memset(ws, 0, sizeof *ws);
    ws->fd = -1;
    struct timeval now;
    gettimeofday(&now, NULL);
    ws->rng = seed ^ (uint32_t)now.tv_usec ^ ((uint32_t)now.tv_sec << 10) ^ 0xa5a5a5a5u;

    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        fail(ws, "no such host");
        return false;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        fail(ws, "socket failed");
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        fail(ws, "connect refused");
        return false;
    }
    if (rc < 0) {
        if (!wait_fd(fd, true, timeout_ms)) {
            close(fd);
            fail(ws, "connect timeout");
            return false;
        }
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) {
            close(fd);
            fail(ws, "connect refused");
            return false;
        }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ws->fd = fd;
    ws->rx = calloc(1, sizeof(rx_t));
    if (!ws->rx) { fail(ws, "out of memory"); ws_close(ws); return false; }

    uint8_t nonce[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = next_rand(ws);
        memcpy(nonce + i, &r, 4);
    }
    char key[32];
    base64(nonce, 16, key);
    char req[512];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
                     "Sec-WebSocket-Protocol: %s\r\n\r\n",
                     path, host, port, key, protocols);
    if (!send_all(ws, req, (size_t)n)) { ws_close(ws); return false; }

    /* Read the response header byte by byte up to the blank line, so nothing of the first frame
     * (servers may send announcements immediately) is swallowed with it. */
    char resp[1024];
    size_t got = 0;
    while (got < sizeof resp - 1) {
        if (!wait_fd(fd, false, timeout_ms)) { fail(ws, "upgrade timeout"); ws_close(ws); return false; }
        ssize_t k = recv(fd, resp + got, 1, 0);
        if (k <= 0) {
            if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            fail(ws, "upgrade closed");
            ws_close(ws);
            return false;
        }
        got++;
        if (got >= 4 && memcmp(resp + got - 4, "\r\n\r\n", 4) == 0) break;
    }
    resp[got] = 0;
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        fail(ws, "upgrade rejected");
        ws_close(ws);
        return false;
    }
    if (chosen && chosen_len) {
        chosen[0] = 0;
        const char *h = strcasestr(resp, "Sec-WebSocket-Protocol:");
        if (h) {
            h += 23;
            while (*h == ' ') h++;
            size_t i = 0;
            while (h[i] && h[i] != '\r' && i + 1 < chosen_len) { chosen[i] = h[i]; i++; }
            chosen[i] = 0;
        }
    }
    ws->msg_op = 0;
    ws->msg = NULL;
    ws->msg_cap = ws->msg_len = 0;
    return true;
}

bool ws_send(ws_t *ws, ws_opcode_t op, const void *data, size_t len)
{
    if (ws->fd < 0) return false;
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = 0x80 | (uint8_t)op;
    if (len < 126) {
        hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xffff) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)len;
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    uint32_t m = next_rand(ws);
    uint8_t mask[4];
    memcpy(mask, &m, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;
    if (!send_all(ws, hdr, h)) return false;
    /* Mask in chunks so a large publish never needs a second full-size buffer. */
    uint8_t chunk[512];
    const uint8_t *src = data;
    for (size_t off = 0; off < len; off += sizeof chunk) {
        size_t n = len - off < sizeof chunk ? len - off : sizeof chunk;
        for (size_t i = 0; i < n; i++) chunk[i] = src[off + i] ^ mask[(off + i) & 3];
        if (!send_all(ws, chunk, n)) return false;
    }
    return true;
}

static bool append_msg(ws_t *ws, const uint8_t *p, size_t n)
{
    if (ws->msg_len + n + 1 > ws->msg_cap) {
        size_t cap = ws->msg_cap ? ws->msg_cap : 4096;
        while (cap < ws->msg_len + n + 1) cap *= 2;
        uint8_t *m = realloc(ws->msg, cap);
        if (!m) { fail(ws, "out of memory"); return false; }
        ws->msg = m;
        ws->msg_cap = cap;
    }
    memcpy(ws->msg + ws->msg_len, p, n);
    ws->msg_len += n;
    ws->msg[ws->msg_len] = 0; /* text messages can be parsed in place */
    return true;
}

/* Parses one frame from rx if complete. Returns 1 for a finished data message, 0 if more bytes are
 * needed or a control/continuation frame was consumed, -1 on protocol error or close. */
static int parse_frame(ws_t *ws, rx_t *rx, bool *consumed)
{
    *consumed = false;
    if (rx->len < 2) return 0;
    const uint8_t *b = rx->buf;
    bool fin = b[0] & 0x80;
    int op = b[0] & 0x0f;
    bool masked = b[1] & 0x80;
    uint64_t plen = b[1] & 0x7f;
    size_t h = 2;
    if (plen == 126) {
        if (rx->len < 4) return 0;
        plen = (uint64_t)b[2] << 8 | b[3];
        h = 4;
    } else if (plen == 127) {
        if (rx->len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = plen << 8 | b[2 + i];
        h = 10;
    }
    if (masked) h += 4;
    if (plen > RX_CAP - 16) {
        /* A robot's periodic batch is a few kilobytes; a frame this size is a broken stream. */
        fail(ws, "frame too large");
        return -1;
    }
    if (rx->len < h + plen) return 0;
    const uint8_t *payload = b + h;
    size_t take = (size_t)plen;

    int result = 0;
    if (op == WS_PING) {
        uint8_t tmp[125];
        size_t n = take < sizeof tmp ? take : sizeof tmp;
        memcpy(tmp, payload, n);
        ws_send(ws, WS_PONG, tmp, n);
    } else if (op == WS_CLOSE) {
        fail(ws, "closed by server");
        result = -1;
    } else if (op == WS_PONG) {
        /* keepalive answers carry nothing */
    } else {
        if (op != 0) {
            ws->msg_op = op;
            ws->msg_len = 0;
        }
        if (!append_msg(ws, payload, take)) return -1;
        if (fin) result = 1;
    }
    size_t used = h + take;
    memmove(rx->buf, rx->buf + used, rx->len - used);
    rx->len -= used;
    *consumed = true;
    return result;
}

int ws_recv(ws_t *ws, int timeout_ms, int *op, const uint8_t **data, size_t *len)
{
    if (ws->fd < 0) return -1;
    rx_t *rx = ws->rx;
    struct timeval start, now;
    gettimeofday(&start, NULL);
    for (;;) {
        bool consumed = true;
        while (consumed) {
            int r = parse_frame(ws, rx, &consumed);
            if (r < 0) return -1;
            if (r == 1) {
                *op = ws->msg_op;
                *data = ws->msg;
                *len = ws->msg_len;
                return 1;
            }
        }
        gettimeofday(&now, NULL);
        int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000);
        int left = timeout_ms - elapsed;
        if (left <= 0) return 0;
        if (!wait_fd(ws->fd, false, left)) return 0;
        ssize_t k = recv(ws->fd, rx->buf + rx->len, RX_CAP - rx->len, 0);
        if (k == 0) { fail(ws, "connection closed"); return -1; }
        if (k < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            fail(ws, "connection reset");
            return -1;
        }
        rx->len += (size_t)k;
        ws->rx_bytes += (uint64_t)k;
    }
}

void ws_close(ws_t *ws)
{
    if (ws->fd >= 0) close(ws->fd);
    free(ws->rx);
    ws->rx = NULL;
    ws->fd = -1;
    free(ws->msg);
    ws->msg = NULL;
    ws->msg_cap = ws->msg_len = 0;
}
