/* ws_lite — a small RFC 6455 WebSocket client over BSD sockets.
 *
 * The same file builds against lwIP on the ESP32-P4 and against POSIX on the host simulator, which is
 * the point: the NetworkTables client above it is tested on a laptop against a fake robot and runs
 * unchanged on the tablet. Client frames are masked (the RFC requires it), control frames are answered
 * inline, fragmented messages are reassembled. No TLS — robots speak plain ws:// on 5810. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { WS_TEXT = 1, WS_BINARY = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 } ws_opcode_t;

typedef struct {
    int fd;
    uint8_t *msg;       /* reassembled message, grown as needed */
    size_t msg_cap, msg_len;
    int msg_op;
    void *rx;           /* bytes read but not yet parsed into a frame */
    uint32_t rng;
    uint64_t rx_bytes, tx_bytes;
    char error[64];
} ws_t;

/* Resolves `host`, connects within `timeout_ms`, and performs the upgrade on `path` offering
 * `protocols` (comma separated, preferred first). The protocol the server chose is copied to
 * `chosen` (may be NULL). Returns false with ws->error filled on any failure. */
bool ws_connect(ws_t *ws, const char *host, int port, const char *path, const char *protocols,
                char *chosen, size_t chosen_len, int timeout_ms);
bool ws_send(ws_t *ws, ws_opcode_t op, const void *data, size_t len);
/* Waits up to `timeout_ms` for a complete data message. Returns 1 with op, data and len set (data is
 * valid until the next call), 0 on timeout, -1 on a closed or broken connection. Pings are answered
 * and never returned. */
int ws_recv(ws_t *ws, int timeout_ms, int *op, const uint8_t **data, size_t *len);
void ws_close(ws_t *ws);
