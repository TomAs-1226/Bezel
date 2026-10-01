/* The saved networks and the choice among them (wifi_saved.h). Pure. */
#include "wifi_saved.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ws_count(const ws_set_t *s)
{
    int k = 0;
    for (int i = 0; i < WS_MAX; i++) k += s->n[i].ssid[0] != 0;
    return k;
}

int ws_find(const ws_set_t *s, const char *ssid)
{
    if (!ssid || !ssid[0]) return -1;
    for (int i = 0; i < WS_MAX; i++)
        if (!strcmp(s->n[i].ssid, ssid)) return i;
    return -1;
}

bool ws_touch(ws_set_t *s, int slot)
{
    if (slot < 0 || slot >= WS_MAX || !s->n[slot].ssid[0]) return false;
    /* already later than every other: nothing to do */
    uint32_t m = 0;
    for (int i = 0; i < WS_MAX; i++)
        if (i != slot && s->n[i].ssid[0] && s->n[i].used > m) m = s->n[i].used;
    if (s->n[slot].used > m) return false;
    s->n[slot].used = m + 1;
    return true;
}

int ws_put(ws_set_t *s, const char *ssid, const char *pass, bool *changed)
{
    if (changed) *changed = false;
    if (!ssid || !ssid[0] || strlen(ssid) > 32) return -1;
    if (!pass) pass = "";
    if (strlen(pass) > 64) return -1;
    int k = ws_find(s, ssid);
    bool ch = false;
    if (k < 0) {
        for (int i = 0; i < WS_MAX && k < 0; i++)
            if (!s->n[i].ssid[0]) k = i;
        if (k < 0) { /* full: the one used longest ago makes room */
            k = 0;
            for (int i = 1; i < WS_MAX; i++)
                if (s->n[i].used < s->n[k].used) k = i;
        }
        memset(&s->n[k], 0, sizeof s->n[k]);
        snprintf(s->n[k].ssid, sizeof s->n[k].ssid, "%s", ssid);
        ch = true;
    }
    if (strcmp(s->n[k].pass, pass)) {
        memset(s->n[k].pass, 0, sizeof s->n[k].pass);
        snprintf(s->n[k].pass, sizeof s->n[k].pass, "%s", pass);
        ch = true;
    }
    if (ws_touch(s, k)) ch = true;
    if (changed) *changed = ch;
    return k;
}

bool ws_remove(ws_set_t *s, int slot)
{
    if (slot < 0 || slot >= WS_MAX || !s->n[slot].ssid[0]) return false;
    memset(&s->n[slot], 0, sizeof s->n[slot]);
    return true;
}

static int pick(const ws_set_t *s, const ws_seen_t *seen, int nseen, uint32_t skip)
{
    int best = -1, best_rssi = -999, last = -1, last_rssi = -999;
    for (int i = 0; i < WS_MAX; i++) {
        if (!s->n[i].ssid[0] || (skip >> i & 1)) continue;
        int r = -999;
        for (int k = 0; k < nseen; k++)
            if (!strcmp(seen[k].ssid, s->n[i].ssid) && seen[k].rssi > r) r = seen[k].rssi;
        if (r == -999) continue;
        if (best < 0 || r > best_rssi) best = i, best_rssi = r;
        if (last < 0 || s->n[i].used > s->n[last].used) last = i, last_rssi = r;
    }
    if (best < 0) return -1;
    return last_rssi >= best_rssi - WS_COMPARABLE_DB ? last : best;
}

int ws_pick(const ws_set_t *s, const ws_seen_t *seen, int nseen, uint32_t skip)
{
    int k = pick(s, seen, nseen, skip);
    if (k < 0 && skip) k = pick(s, seen, nseen, 0);
    return k;
}

size_t ws_encode(const ws_net_t *n, char *out, size_t max)
{
    if (!max) return 0;
    out[0] = 0;
    if (!n->ssid[0]) return 0;
    size_t o = (size_t)snprintf(out, max, "%u:", (unsigned)n->used);
    for (const unsigned char *p = (const unsigned char *)n->ssid; *p && o + 3 < max; p++)
        o += (size_t)snprintf(out + o, max - o, "%02x", *p);
    if (o >= max) return 0;
    o += (size_t)snprintf(out + o, max - o, ":%s", n->pass);
    if (o >= max) {
        memset(out, 0, max);
        return 0;
    }
    return o;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ws_decode(const char *in, ws_net_t *out)
{
    memset(out, 0, sizeof *out);
    if (!in || !in[0]) return false;
    char *e;
    unsigned long u = strtoul(in, &e, 10);
    if (e == in || *e != ':') return false;
    const char *h = e + 1, *c = strchr(h, ':');
    size_t hl = c ? (size_t)(c - h) : 0;
    if (!c || !hl || hl % 2 || hl / 2 > 32 || strlen(c + 1) > 64) return false;
    for (size_t i = 0; i < hl / 2; i++) {
        int a = hexv(h[2 * i]), b = hexv(h[2 * i + 1]);
        if (a < 0 || b < 0 || !(a | b)) {
            memset(out, 0, sizeof *out);
            return false;
        }
        out->ssid[i] = (char)(a << 4 | b);
    }
    snprintf(out->pass, sizeof out->pass, "%s", c + 1);
    out->used = (uint32_t)u;
    return true;
}
