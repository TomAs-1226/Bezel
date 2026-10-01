/* wifi_saved — the networks the tablet remembers, and which one to join. Pure: no radio, no storage, so the
 * choices are tested on the host (test/test_wifi.c); hal_tab5_net.c keeps the set in kv and acts on it.
 *
 * Every network the tablet has joined is kept with its password, WS_MAX at most; a new one past that takes
 * the place of the one used longest ago. Each slot is one kv value ("wifi_s0".."wifi_s15"), so a change
 * rewrites one small string. Passwords never leave this set except to the radio: nothing here prints one. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_MAX 16
#define WS_COMPARABLE_DB 8   /* the last-used network is kept unless another is louder by more than this */
#define WS_ENC_MAX 160       /* a slot's stored form: "used:hex(ssid):password" */

typedef struct {
    char ssid[33];           /* "" an empty slot */
    char pass[65];           /* "" an open network */
    uint32_t used;           /* a counter, not a time: the larger was used later */
} ws_net_t;

typedef struct {
    ws_net_t n[WS_MAX];
} ws_set_t;

typedef struct {
    char ssid[33];
    int rssi;
} ws_seen_t;

int ws_count(const ws_set_t *s);
int ws_find(const ws_set_t *s, const char *ssid);            /* the slot, -1 not saved */
/* Saves (or updates) a network and marks it the latest used. Returns its slot; *changed says whether the
 * slot's stored form is different now. Full: the one used longest ago is replaced. -1 for a name or a
 * password that can't be one. */
int ws_put(ws_set_t *s, const char *ssid, const char *pass, bool *changed);
/* Marks a saved network the latest used; true when that changed anything (it wasn't already). */
bool ws_touch(ws_set_t *s, int slot);
bool ws_remove(ws_set_t *s, int slot);
/* The saved network to join, given what a scan heard: the loudest one in range, except that the one used
 * last is kept when it is within WS_COMPARABLE_DB of the loudest (two networks of similar strength never
 * trade places). Slots in `skip` (bit n) are passed over, ones that just refused the password, unless
 * nothing else is in range. -1: none of them is in range. */
int ws_pick(const ws_set_t *s, const ws_seen_t *seen, int nseen, uint32_t skip);
/* A slot's stored form and back. encode returns the length (0: empty slot); decode false for anything else. */
size_t ws_encode(const ws_net_t *n, char *out, size_t max);
bool ws_decode(const char *in, ws_net_t *out);
