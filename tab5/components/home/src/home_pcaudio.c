/* The PC's sound out of the speaker: see home_pcaudio.h for what it is. This file is the receiver.
 *
 * One thread, alive only while the switch is on. It connects to the Link's GET /audio/stream, reads packets
 * (12-byte header + n mono 48 kHz samples; n = 0 with the idle flag is a keepalive: the PC plays nothing),
 * and feeds the speaker's queue. The jitter buffer is that queue itself (hal_play_queued_ms): the output task
 * starts a stream at ~170 ms queued, so the whole delay is that plus the network, and the feeder keeps it near
 * FILL_TARGET_MS by playing a packet a hair short or long (a 1-3% stretch, under 0.5 semitone, for a second
 * or two) rather than letting the two clocks drift until it runs dry or the delay grows. A stall that leaves
 * far too much queued drops a packet. No per-packet work reaches the UI: it reads home_pcaudio_get() on its
 * own refresh. The stack is small and everything big is on the heap. */
#include "home_pcaudio.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "hal.h"
#include "hal_play.h"
#include "link.h"
#include "voice.h"

#define RATE 48000
#define PKT_HDR 12
#define PKT_MAX 4800                   /* samples in one packet, the PC's cap */
#define RX_BYTES (PKT_HDR + PKT_MAX * 2 + 2048)
#define FILL_TARGET_MS 190
#define FILL_HI_MS 280                 /* above: play shorter */
#define FILL_LO_MS 130                 /* below: play longer */
#define FILL_DROP_MS 450               /* above: a stall's backlog, drop the packet */
#define IDLE_RELEASE_S 1.2             /* this long with nothing playing on the PC: give the speaker back */
#define ACK_S 0.25

static struct {
    pthread_mutex_t lock;
    bool want, running;
    char host_override[96];
    double hold_until;             /* the card's player has asked for the speaker */
    pca_t st;
} A = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void publish(pca_state_t s, const char *note)
{
    pthread_mutex_lock(&A.lock);
    if (A.st.state != s || strcmp(A.st.note, note ? note : "")) {
        A.st.state = s;
        snprintf(A.st.note, sizeof A.st.note, "%s", note ? note : "");
        A.st.gen++;
    }
    pthread_mutex_unlock(&A.lock);
}

static void stats(int fill, unsigned under, unsigned packets, int kbps)
{
    pthread_mutex_lock(&A.lock);
    A.st.fill_ms = fill;
    A.st.underruns = under;
    A.st.packets = packets;
    A.st.rate_kbps = kbps;
    pthread_mutex_unlock(&A.lock);
}

void home_pcaudio_get(pca_t *out)
{
    pthread_mutex_lock(&A.lock);
    *out = A.st;
    pthread_mutex_unlock(&A.lock);
}

void home_pcaudio_host(const char *url)
{
    pthread_mutex_lock(&A.lock);
    snprintf(A.host_override, sizeof A.host_override, "%s", url ? url : "");
    pthread_mutex_unlock(&A.lock);
}

static bool wanted(void)
{
    pthread_mutex_lock(&A.lock);
    bool w = A.want;
    pthread_mutex_unlock(&A.lock);
    return w;
}

/* ---- sockets ---- */

static bool wait_fd(int fd, bool write, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select(fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &tv) > 0;
}

static int dial(const char *host, int port)
{
    char p[8];
    snprintf(p, sizeof p, "%d", port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, p, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (r != 0 && !wait_fd(fd, true, 4000)) {
        close(fd);
        return -1;
    }
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static bool send_all(int fd, const void *data, size_t n)
{
    const char *p = data;
    while (n) {
        if (!wait_fd(fd, true, 1500)) return false;
        ssize_t w = send(fd, p, n, 0);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (w > 0) {
            p += w;
            n -= (size_t)w;
        }
    }
    return true;
}

/* "http://192.168.0.20:8765" → host and port */
static bool split_url(const char *url, char *host, size_t hn, int *port)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    const char *colon = strchr(h, ':');
    const char *end = h + strcspn(h, "/");
    size_t l = (colon && colon < end ? colon : end) - h;
    if (!l || l >= hn) return false;
    memcpy(host, h, l);
    host[l] = 0;
    *port = colon && colon < end ? atoi(colon + 1) : 80;
    return *port > 0;
}

/* ---- the speaker ---- */

typedef struct {
    bool open;              /* the speaker is ours */
    double tried;           /* hal_seconds of the last open attempt */
    int16_t *tmp;           /* a packet, stretched or squeezed */
    unsigned under_seen, under_total;
} spk_t;

static bool assistant_busy(void)
{
    vo_state_t v = voice_state();
    if (v != VO_OFF && v != VO_IDLE) return true;
    pthread_mutex_lock(&A.lock);
    bool held = hal_seconds() < A.hold_until;
    pthread_mutex_unlock(&A.lock);
    return held;
}

void home_pcaudio_yield(void)
{
    pthread_mutex_lock(&A.lock);
    A.hold_until = hal_seconds() + 4.0;
    pthread_mutex_unlock(&A.lock);
}

static void spk_close(spk_t *s)
{
    if (s->open) {
        if (hal_play_owned()) hal_play_close();
        s->open = false;
    }
}

/* try to take the speaker, at most every 300 ms */
static bool spk_open(spk_t *s, double now)
{
    if (s->open && hal_play_owned()) return true;
    s->open = false; /* the assistant took it, or never ours */
    if (now - s->tried < 0.3) return false;
    s->tried = now;
    if (assistant_busy()) return false;
    s->open = hal_play_open(RATE, 1);
    if (s->open) s->under_seen = hal_play_underruns();
    return s->open;
}

/* n samples into the speaker, playing a little short or long when the queue strays from its target */
static void feed(spk_t *s, const int16_t *pcm, int n)
{
    int fill = hal_play_queued_ms();
    if (fill > FILL_DROP_MS) return;
    /* proportional: 1 ms over the target is 1/6000 of a packet shorter (280 ms: 1.5%, 450: 4.3%); under the
     * target, longer by the same, up to 1.5% */
    int m = n;
    if (fill > FILL_HI_MS - 40 || (fill > 0 && fill < FILL_LO_MS)) {
        int d = (int)((long)(fill - FILL_TARGET_MS) * n / 6000);
        if (d > n / 20) d = n / 20;
        if (d < -n / 60) d = -n / 60;
        m = n - d;
    }
    const int16_t *src = pcm;
    if (m != n && n > 8) {
        for (int i = 0; i < m; i++) {
            float pos = (float)i * (float)(n - 1) / (float)(m - 1);
            int k = (int)pos;
            float f = pos - (float)k;
            int16_t b = k + 1 < n ? pcm[k + 1] : pcm[k];
            s->tmp[i] = (int16_t)((float)pcm[k] + ((float)b - (float)pcm[k]) * f);
        }
        src = s->tmp;
    } else {
        m = n;
    }
    int done = 0;
    while (done < m && hal_play_owned()) {
        int w = hal_play_write(src + done, m - done, 150);
        if (w < 0) break;
        done += w;
        if (w == 0) break;
    }
}

/* ---- one connection ---- */

typedef enum { END_WANT, END_LOST, END_REFUSED } end_t;

static const char *s_why = ""; /* what ended the last connection (one writer: the receiver thread) */
#define LOST(w) (s_why = (w), END_LOST)

static end_t session(int fd, spk_t *s, uint8_t *rx, int *backoff_s)
{
    size_t have = 0;
    double last_ack = 0, idle_since = 0, rate_t = hal_seconds();
    unsigned packets = 0, last_seq = 0;
    size_t rate_bytes = 0;
    int kbps = 0;
    bool idle = true, got_any = false;
    double last_rx = hal_seconds();
    s->under_total = 0;

    /* the response head, then packets */
    size_t hl = 0;
    char *head = (char *)rx;
    for (;;) {
        if (!wanted()) return END_WANT;
        if (!wait_fd(fd, false, 300)) {
            if (hal_seconds() - last_rx > 6) return LOST("no answer");
            continue;
        }
        ssize_t r = recv(fd, head + hl, RX_BYTES - 1 - hl, 0);
        if (r <= 0) return LOST("closed in the head");
        hl += (size_t)r;
        head[hl] = 0;
        char *eoh = strstr(head, "\r\n\r\n");
        if (!eoh) {
            if (hl > 1500) return LOST("bad head");
            continue;
        }
        int status = atoi(head + 9);
        if (status != 200) {
            /* the Link's own words: {"ok":false,"error":"..."} (the body may still be arriving) */
            wait_fd(fd, false, 300);
            ssize_t more = recv(fd, head + hl, RX_BYTES - 1 - hl, 0);
            if (more > 0) hl += (size_t)more;
            head[hl] = 0;
            char note[80] = "";
            char *e = strstr(eoh, "\"error\"");
            if (e && (e = strchr(e + 7, '"'))) {
                e++; /* past the opening quote of the value */
                size_t l = strcspn(e, "\"");
                snprintf(note, sizeof note, "%.*s", (int)(l < sizeof note - 1 ? l : sizeof note - 1), e);
            }
            if (status == 401) snprintf(note, sizeof note, "the pc refused the tablet: pair it again");
            else if (status == 403) snprintf(note, sizeof note, "switched off on the pc");
            else if (status == 404) snprintf(note, sizeof note, "update catalyst link on the pc for its sound");
            else if (!note[0]) snprintf(note, sizeof note, "the pc answered %d", status);
            publish(PCA_WAITING, note);
            *backoff_s = status == 401 ? 10 : 4;
            return END_REFUSED;
        }
        have = hl - (size_t)(eoh + 4 - head);
        memmove(rx, eoh + 4, have);
        break;
    }
    publish(PCA_QUIET, "");
    *backoff_s = 1;

    for (;;) {
        if (!wanted()) return END_WANT;
        double now = hal_seconds();
        if (s->open && assistant_busy()) spk_close(s); /* the card's player asked: out of its way at once */
        if (wait_fd(fd, false, 100)) {
            ssize_t r = recv(fd, rx + have, RX_BYTES - have, 0);
            if (r <= 0) return LOST(r == 0 ? "the pc closed it" : "recv error");
            have += (size_t)r;
            rate_bytes += (size_t)r;
            last_rx = now;
        } else if (now - last_rx > 6) {
            return LOST("silence over 6 s"); /* the PC or the Wi-Fi went away without closing */
        }
        /* whole packets */
        size_t off = 0;
        while (have - off >= PKT_HDR) {
            const uint8_t *h = rx + off;
            unsigned n = (unsigned)h[2] | (unsigned)h[3] << 8;
            if (h[0] != 1 || n > PKT_MAX) return LOST("out of step"); /* start over */
            if (have - off < PKT_HDR + (size_t)n * 2) break;
            unsigned flags = h[1];
            last_seq = (unsigned)h[4] | (unsigned)h[5] << 8 | (unsigned)h[6] << 16 | (unsigned)h[7] << 24;
            packets++;
            got_any = true;
            if (n == 0) {
                if (!idle) idle_since = now;
                idle = true;
                (void)flags;
            } else {
                if (idle) {
                    idle = false;
                    idle_since = 0;
                    s->under_seen = hal_play_underruns();
                }
                bool hold = assistant_busy();
                if (hold) {
                    spk_close(s);
                } else if (spk_open(s, now)) {
                    /* a dry queue while audio is flowing is a dropout; one after a silence is just the start */
                    feed(s, (const int16_t *)(const void *)(h + PKT_HDR), (int)n);
                }
                publish(hold || !s->open ? PCA_HELD : PCA_LIVE, hold ? "the assistant has the speaker" : "");
            }
            off += PKT_HDR + (size_t)n * 2;
        }
        if (off) {
            have -= off;
            memmove(rx, rx + off, have);
        }
        if (idle) {
            if (idle_since == 0) idle_since = now;
            if (now - idle_since > IDLE_RELEASE_S && s->open && hal_play_queued_ms() == 0) spk_close(s);
            if (now - idle_since > 0.5) publish(PCA_QUIET, "");
        }
        /* dropouts: the speaker ran dry while the PC was playing */
        unsigned u = hal_play_underruns();
        if (!idle && s->open) s->under_total += u - s->under_seen;
        s->under_seen = u;
        if (now - rate_t >= 1.0) {
            kbps = (int)((double)rate_bytes * 8 / 1000 / (now - rate_t));
            rate_bytes = 0;
            rate_t = now;
        }
        stats(s->open ? hal_play_queued_ms() : 0, s->under_total, packets, kbps);
        if (got_any && now - last_ack >= ACK_S) {
            last_ack = now;
            uint8_t ack[16] = { 3, 0 };
            int fill = hal_play_queued_ms();
            ack[2] = (uint8_t)fill;
            ack[3] = (uint8_t)(fill >> 8);
            memcpy(ack + 4, &last_seq, 4);
            memcpy(ack + 8, &s->under_total, 4);
            uint32_t ms = (uint32_t)(now * 1000);
            memcpy(ack + 12, &ms, 4);
            if (!send_all(fd, ack, sizeof ack)) return LOST("ack not sent");
        }
    }
}

static void *receiver(void *arg)
{
    (void)arg;
    uint8_t *rx = malloc(RX_BYTES);
    spk_t s = { 0 };
    s.tmp = malloc((PKT_MAX + PKT_MAX / 33 + 8) * sizeof(int16_t));
    for (;;) {
        pthread_mutex_lock(&A.lock);
        if (!A.want || !rx || !s.tmp) {
            A.running = false;
            pthread_mutex_unlock(&A.lock);
            break;
        }
        pthread_mutex_unlock(&A.lock);

        char url[96], token[80], host[64];
        int port = 0, backoff = 2;
        link_status_t ls;
        link_status(&ls);
        bool paired = link_credentials(url, sizeof url, token, sizeof token);
        pthread_mutex_lock(&A.lock);
        if (A.host_override[0]) snprintf(url, sizeof url, "%s", A.host_override);
        pthread_mutex_unlock(&A.lock);
        if (!paired || !split_url(url, host, sizeof host, &port)) {
            publish(PCA_WAITING, "no pc paired: settings, pc link");
            backoff = 3;
        } else if (!ls.reachable) {
            publish(PCA_WAITING, "the pc isn't answering");
            backoff = 2;
        } else {
            publish(PCA_CONNECTING, "");
            int fd = dial(host, port);
            if (fd < 0) {
                publish(PCA_WAITING, "couldn't reach the pc's sound");
            } else {
                char req[200];
                int l = snprintf(req, sizeof req, "GET /audio/stream HTTP/1.1\r\nHost: %s\r\nX-Link-Token: %s\r\n"
                                 "Connection: close\r\n\r\n", host, token);
                memset(token, 0, sizeof token);
                if (send_all(fd, req, (size_t)l)) {
                    end_t e = session(fd, &s, rx, &backoff);
                    if (e == END_LOST) {
                        char w[80];
                        snprintf(w, sizeof w, "lost the pc (%s): reconnecting", s_why);
                        printf("pcaudio: %s after %u packets\n", w, A.st.packets);
                        publish(PCA_WAITING, w);
                        pthread_mutex_lock(&A.lock);
                        A.st.reconnects++;
                        pthread_mutex_unlock(&A.lock);
                    }
                }
                close(fd);
                spk_close(&s);
                stats(0, s.under_total, 0, 0);
            }
        }
        /* wait before trying again, but leave at once when switched off */
        for (int i = 0; i < backoff * 10 && wanted(); i++) usleep(100 * 1000);
    }
    spk_close(&s);
    free(rx);
    free(s.tmp);
    if (!wanted()) publish(PCA_OFF, "");
    return NULL;
}

void home_pcaudio_enable(bool on)
{
    bool start = false;
    pthread_mutex_lock(&A.lock);
    if (A.want != on) {
        A.want = on;
        A.st.enabled = on;
        A.st.gen++;
    }
    if (on && !A.running) {
        A.running = true;
        start = true;
    }
    if (!on && !A.running && A.st.state != PCA_OFF) {
        A.st.state = PCA_OFF;
        A.st.note[0] = 0;
        A.st.gen++;
    }
    pthread_mutex_unlock(&A.lock);
    if (start && !hal_thread("pcaudio", receiver, NULL, 6144)) {
        pthread_mutex_lock(&A.lock);
        A.running = false;
        pthread_mutex_unlock(&A.lock);
        publish(PCA_WAITING, "no memory for the sound stream");
    }
}
