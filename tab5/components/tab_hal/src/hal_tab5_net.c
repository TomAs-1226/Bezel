/* hal_tab5_net — the network half of Catalyst Tab's hardware layer on the M5Stack Tab5: Wi-Fi through the
 * ESP32-C6, the USB tether on the USB-A port, mDNS browsing, the HTTP(S) client and worker threads.
 *
 * The USB-A port is the P4's USB 2.0 high-speed OTG controller (its UTMI PHY, P4 pins 49/50, through a
 * 33 Ω pair and a common-mode filter to J10 on the schematic), running as a host with the port's 5 V
 * switched by expander E2.P3. The USB-C port is wired to the full-speed PHY that carries USB-Serial/JTAG
 * and takes power in; it can't host.
 *
 * The tether uses Espressif's esp-iot-solution host drivers — iot_usbh_cdc underneath, iot_usbh_ecm and
 * iot_usbh_rndis on top, iot_eth to reach esp_netif — so anything that speaks CDC-ECM or RNDIS works:
 * Systemcore's USB-C gadget (a composite of ECM and RNDIS, see docs/tab5-hardware.md) and CDC-ECM
 * USB-Ethernet adapters. There is no CDC-NCM host driver for ESP-IDF, and none is written here.
 *
 * Verified by compiling only: nothing in this file has run on a Tab5. Items marked UNVERIFIED are the
 * ones most likely to need a correction on first boot. */
#include "hal.h"
#include "hal_tab5_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_ecm.h"
#include "iot_usbh_rndis.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <fcntl.h>
#include "lwip/inet.h"
#include "esp_sntp.h"
#include "mdns.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

static const char *TAG = "hal-net";

/* ------------------------------------------------------------------ Wi-Fi (the ESP32-C6) */

static struct {
    bool up;
    char ip[16];
    esp_netif_t *sta;
    /* The network's name and signal, kept here: asking the C6 is a round trip over SDIO that waits up to
     * 5 s when the C6 is busy, and hal_net() is asked from the UI's frame (the status band, home mode's
     * checks): the whole interface stood still for it. The name comes with the connect event, the signal
     * from a small task every few seconds. */
    char ssid[33];
    volatile int rssi;
    TaskHandle_t rssi_task;
    /* what the C6 said about itself at start-up, for the dev console's "net": the slave's esp-hosted
     * version tells the custom packet-mode slave (1.4.7) from M5Stack's factory one, and the rest are the
     * radio settings that a drop usually turns out to be about */
    char fw[16];
    int8_t txpower;      /* esp_wifi_get_max_tx_power, quarter-dBm */
    uint8_t proto;       /* esp_wifi_get_protocol bitmap */
    uint16_t inactive_s; /* the beacon-loss window actually in force */
    char country[4];
    uint8_t cc_lo, cc_n; /* the channels that country allows */
    /* the last drop, kept so it can be read back without the log */
    volatile int drop_reason;
    volatile int drop_rssi;
    volatile uint32_t drops;
} N;

static void tether_promote_all(void);

static volatile bool s_scanning;

/* Espressif's wifi_err_reason_t, the codes that actually turn up on a station. Anything else prints as a
 * number: the number is what matters, the name only saves a lookup. */
static const char *drop_name(int reason)
{
    switch (reason) {
    case 1: return "unspecified";
    case 2: return "auth expired";
    case 3: return "deauthenticated (AP said leave)";
    case 4: return "disassociated for inactivity";
    case 5: return "AP has too many stations";
    case 8: return "disassociated (AP said leave)";
    case 12: return "AP asked it to move (BSS transition)";
    case 14: return "MIC failure";
    case 15: return "4-way handshake timed out";
    case 16: return "group key update timed out";
    case 23: return "802.1X auth failed";
    case 39: return "timeout";
    case 200: return "beacon timeout";
    case 201: return "no AP found";
    case 202: return "auth failed (wrong password?)";
    case 203: return "association failed";
    case 204: return "handshake timeout";
    case 205: return "connection failed";
    case 206: return "AP reset its TSF";
    case 207: return "roaming";
    case 210: return "no AP found with compatible security";
    case 211: return "no AP found passing the authmode threshold";
    case 212: return "no AP found passing the RSSI threshold";
    default: return "?";
    }
}

/* Whether a network has been saved (hal_wifi_join; the driver keeps it in NVS). Without one there's
 * nothing to connect to, and a station forever connecting refuses to scan (ESP_ERR_WIFI_STATE). */
static bool wifi_has_network(void)
{
    wifi_config_t wc;
    /* M5Stack's factory test leaves its line's network in the C6's NVS: that one doesn't count */
    return esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] &&
           strcmp((const char *)wc.sta.ssid, "M5Stack-Production") != 0;
}

/* The reconnect, asked for here and done by rssi_task a moment later.
 *
 * It used to be esp_wifi_connect() straight out of the disconnect handler, and that is wrong twice over.
 * Every esp_wifi_* call is an RPC over SDIO to the C6 that blocks for up to 5 s, and the handler runs on
 * the default event loop's task — the same task esp-hosted posts its Wi-Fi events to (os_wrapper.c's
 * esp_event_post), so a slow connect stalls every other event behind it. And with the AP simply gone the
 * two lines chased each other: connect, "no AP found" (201), connect, for as long as the AP stayed away,
 * which leaves the C6 scanning without pause — the state the SDIO link is least happy in, and the one
 * that makes a scan from the settings page fail. Espressif's own guide says as much: reconnect on the
 * event, but "avoid infinite reconnection loops that starve scan operations".
 *
 * So: the handler only records, and the poll acts. First drop retries at once (a pit network that blinks
 * should not cost a second), then 3, 6, 12, 20, 30 s, and 30 s from then on. A won address clears it.
 *
 * And only ever one connect in the air. An all-channel scan and a join take several seconds — measured on
 * the tablet, 3 s from the request to "connected" and another second to the address — so a deadline set
 * blindly for 2 s later had the poll ask again while the first attempt was still working, and the C6
 * answered that by abandoning it: reason 205, CONNECTION_FAIL, at rssi -128, which is a failed attempt
 * rather than a lost link. s_issued holds the time the outstanding connect was asked for; a drop or an
 * address clears it, and 25 s of neither counts as an attempt that went missing. */
static volatile int64_t s_retry_at;  /* esp_timer time to try again; 0 = nothing wanted */
static volatile int64_t s_issued;    /* when the outstanding connect was asked for; 0 = none */
static volatile int s_retry_step;
static const int RETRY_S[] = { 0, 3, 6, 12, 20, 30 };
#define CONNECT_LOST_US (25 * 1000000LL)

static void reconnect_soon(bool fresh)
{
    if (fresh) s_retry_step = 0;
    int i = s_retry_step;
    if (i >= (int)(sizeof RETRY_S / sizeof *RETRY_S)) i = (int)(sizeof RETRY_S / sizeof *RETRY_S) - 1;
    s_retry_at = esp_timer_get_time() + (int64_t)RETRY_S[i] * 1000000LL;
    if (s_retry_step < (int)(sizeof RETRY_S / sizeof *RETRY_S) - 1) s_retry_step++;
}

/* The gateway that has answered a probe on this network, 0 if none ever has.
 *
 * Shared, because it is the one piece of evidence that separates "this network drops our probes" from
 * "the link has stopped working", and both the watchdog and wifi_heal() have to ask it. A gateway that
 * has never answered tells you nothing when it stays silent; one that used to answer and has gone
 * quiet tells you a great deal. Cleared whenever the address changes, since it is a fact about one
 * network and not about the tablet. */
static volatile uint32_t s_gw_answered;

/* This network intercepts: a name resolved, and TLS to the answer was not who we asked for. Recorded
 * so the interface can say "this network blocks it" instead of blaming Wi-Fi, and so a blocked host
 * cannot be mistaken for a broken radio. */
static volatile bool s_filtered;
bool hal_net_filtered(void) { return s_filtered; }

/* ---- the DNS the owner chose ---- */

static char s_dns[2][16];   /* "" each: whatever DHCP says */

/* Puts the chosen servers on the interface. DHCP has just overwritten them if this is a fresh lease,
 * which is exactly why this is called from GOT_IP and not only when the setting changes. */
static void dns_apply(void)
{
    if (!N.sta || !s_dns[0][0]) return;
    for (int i = 0; i < 2; i++) {
        if (!s_dns[i][0]) continue;
        esp_netif_dns_info_t d = { 0 };
        d.ip.type = ESP_IPADDR_TYPE_V4;
        d.ip.u_addr.ip4.addr = esp_ip4addr_aton(s_dns[i]);
        if (d.ip.u_addr.ip4.addr) esp_netif_set_dns_info(N.sta, i ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &d);
    }
}

void hal_wifi_dns(const char *primary, const char *secondary)
{
    snprintf(s_dns[0], sizeof s_dns[0], "%s", primary ? primary : "");
    snprintf(s_dns[1], sizeof s_dns[1], "%s", secondary ? secondary : "");
    hal_kv_set("dns1", s_dns[0]);
    hal_kv_set("dns2", s_dns[1]);
    if (s_dns[0][0]) {
        dns_apply();
    } else if (N.sta) {
        /* Back to DHCP's: the lease still holds them, so ask for it again rather than guessing. */
        esp_netif_dhcpc_stop(N.sta);
        esp_netif_dhcpc_start(N.sta);
    }
    /* Whatever was resolved under the old servers must not be reused: a block page's address would
     * otherwise stay in the cache and the change would look like it had not worked. */
    dns_clear_cache();
    s_filtered = false;
    ESP_LOGW(TAG, "wi-fi: DNS is now %s", s_dns[0][0] ? s_dns[0] : "whatever DHCP hands out");
}

bool hal_wifi_dns_get(char *primary, size_t pn, char *secondary, size_t sn)
{
    if (primary) snprintf(primary, pn, "%s", s_dns[0]);
    if (secondary) snprintf(secondary, sn, "%s", s_dns[1]);
    return s_dns[0][0] != 0;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (wifi_has_network()) reconnect_soon(true);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *e = data;
        snprintf(N.ssid, sizeof N.ssid, "%.32s", (const char *)e->ssid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        N.up = false;
        N.rssi = 0;
        wifi_event_sta_disconnected_t *e = data;
        /* Every drop is logged. It used to log only when the reason changed, which meant a network
         * dropping the same way every minute said so once and then went quiet — the one number that
         * names the fault, lost. A run of the same reason is collapsed into a count instead. */
        N.drops++;
        N.drop_rssi = e->rssi;
        if (e->reason != N.drop_reason || N.drops <= 1 || N.drops % 10 == 0)
            ESP_LOGW(TAG, "wi-fi: '%.32s' dropped, reason %d (%s), rssi %d, drop %u", e->ssid, e->reason,
                     drop_name(e->reason), e->rssi, (unsigned)N.drops);
        N.drop_reason = e->reason;
        s_issued = 0; /* whatever was outstanding is over, one way or the other */
        /* keep trying, backing off: the pit network comes and goes — but not during a scan, and not with
         * nothing saved */
        if (!s_scanning && wifi_has_network()) reconnect_soon(false);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_at = 0;
        s_retry_step = 0;
        s_issued = 0;
        ip_event_got_ip_t *e = data;
        snprintf(N.ip, sizeof N.ip, IPSTR, IP2STR(&e->ip_info.ip));
        N.up = true;
        s_gw_answered = 0;   /* a different network's gateway proves nothing about this one */
        s_filtered = false;
        dns_apply();         /* DHCP has just set its own: put the owner's choice back */
        /* if the C6 restarted, its netif may now sit ahead of the tether in lwIP's list: put the tether back
         * in front, so a subnet both share still goes out the USB cable (see tether_promote()) */
        tether_promote_all();
    }
}

/* the last three restarts the C6 watchdog made, as wall-clock times (the RTC chip keeps the time across them),
 * the newest last; [3] marks the record valid. Kept across the restart. */
static RTC_NOINIT_ATTR uint32_t s_c6_restarts[4];

/* Where the interface was when the watchdog restarted it, so it comes back there (kept across the restart) */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    int page;
    char app[24];
} s_resume;
#define RESUME_MAGIC 0x52534D31u
static void (*s_restart_hook)(void);

void hal_restart_hook(void (*fn)(void)) { s_restart_hook = fn; }

/* esp-hosted restarting the host (esp_hosted's os_wrapper.c, a weak hook there): the C6 reset itself, most
 * likely its own watchdog after a hang. Once the start has settled that's a recovery like the one above:
 * noted and planned. Before it has, it stays a failed start, so a C6 that never comes up still ends in safe
 * mode rather than restarting forever. */
void esp_hosted_host_restarting(void)
{
    if (!hal_boot_settled()) return;
    if (s_restart_hook) s_restart_hook();
    hal_restart_mark_planned();
}

/* the watchdog's restart, on demand (dev console "wdtest"): checks the resume path without a hung C6 */
void hal_c6_restart_test(void)
{
    ESP_LOGW(TAG, "wi-fi: watchdog restart test");
    if (s_restart_hook) s_restart_hook();
    vTaskDelay(pdMS_TO_TICKS(200));
    hal_restart_planned("watchdog restart test");
}

void hal_resume_note(int page, const char *app)
{
    s_resume.page = page;
    snprintf(s_resume.app, sizeof s_resume.app, "%s", app ? app : "");
    s_resume.magic = RESUME_MAGIC;
}

bool hal_resume_take(int *page, char *app, size_t n)
{
    if (s_resume.magic != RESUME_MAGIC) return false;
    s_resume.magic = 0;
    *page = s_resume.page;
    snprintf(app, n, "%s", s_resume.app);
    return true;
}

/* Restarts the tablet to reset a C6 that stopped passing anything, unless one was made too recently: 45 s
 * apart (with the clock unknown, the start must have lasted 45 s). It
 * never gives up: Wi-Fi dead for good until someone reboots was the worst of it (the old limit went quiet
 * after three, and summed uptimes, so reflashing never cleared it). false: not yet (it says so once). */
static volatile bool s_c6_hold; /* dev console "wdhold": a hang is kept for looking at, not reset */
void hal_c6_hold(bool on) { s_c6_hold = on; }

bool esp_hosted_sdio_kick_queued(int n); /* esp_hosted's sdio_drv.c (Catalyst Tab's addition) */
static bool gw_alive(uint32_t *gw);

static bool rpc_ok(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}
static bool gw_ok(void)
{
    uint32_t gw;
    return gw_alive(&gw);
}

/* ok: what failed, asked again after the kick (the RPCs, or the gateway) */
static bool c6_restart(const char *why, bool (*ok)(void))
{
    static bool told;
    /* The C6's usual hang is packets it queued that the host never read (see sdio_drv.c): reading them
     * without the new-packet bit frees it. Tried first when the count says they're there; if the C6 answers
     * again within 3 s, no restart. (A read with nothing queued can wedge the bus instead: the restart below
     * still comes, as it would have.) */
    /* in stress it freed the C6 about half the time on the first try: a second, longer one before giving up */
    for (int attempt = 0; attempt < 2 && !s_c6_hold; attempt++) {
        if (!esp_hosted_sdio_kick_queued(attempt ? 24 : 8)) break;
        ESP_LOGW(TAG, "wi-fi: %s: reading what the C6 queued (try %d)", why, attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (ok()) {
            ESP_LOGW(TAG, "wi-fi: the C6 answers again: no restart");
            return false;
        }
    }
    if (s_c6_hold) {
        static int64_t said;
        int64_t now = esp_timer_get_time();
        if (now - said > 10 * 1000000LL) ESP_LOGW(TAG, "wi-fi: %s (held: not restarting)", why);
        said = now;
        return false;
    }
    uint32_t wall = (uint32_t)time(NULL);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    bool clock_ok = wall > 1700000000u;
    /* a restart is ~6 s and comes back where it was; Wi-Fi dead is worse. Longer spacing in a busy stretch (10
     * min, then 3) left it dead for minutes when the C6 hung every few minutes under load: 45 s apart, always */
    bool wait = clock_ok ? s_c6_restarts[2] && wall - s_c6_restarts[2] < 45 : up < 45;
    if (wait) {
        if (!told) ESP_LOGE(TAG, "wi-fi: %s: restarted under 45 s ago, trying again shortly", why);
        told = true;
        return false;
    }
    s_c6_restarts[0] = s_c6_restarts[1];
    s_c6_restarts[1] = s_c6_restarts[2];
    s_c6_restarts[2] = clock_ok ? wall : 0;
    ESP_LOGE(TAG, "wi-fi: %s: restarting to reset the C6", why);
    if (s_restart_hook) s_restart_hook(); /* the interface notes where it is (hal_resume_note) */
    vTaskDelay(pdMS_TO_TICKS(200));
    hal_restart_planned(why);
    return true;
}

/* Does anything get through: a TCP connect to the gateway's port 80, 2 s at most. An answer either way (it
 * connects, or the router refuses the port) means packets go both ways; silence means they don't. */
static bool gw_alive(uint32_t *gw)
{
    esp_netif_ip_info_t ip = { 0 };
    *gw = 0;
    if (!N.sta || esp_netif_get_ip_info(N.sta, &ip) != ESP_OK || !ip.gw.addr) return true; /* nothing to ask */
    *gw = ip.gw.addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return true; /* out of sockets is not the link's fault */
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(80) };
    sa.sin_addr.s_addr = ip.gw.addr;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    bool alive = r == 0;
    if (!alive && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        struct timeval tv = { .tv_sec = 2 };
        int so = -1;
        socklen_t sl = sizeof so;
        if (select(fd + 1, NULL, &w, NULL, &tv) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0)
            alive = so == 0 || so == ECONNREFUSED || so == ECONNRESET;
    } else if (!alive) {
        alive = errno == ECONNREFUSED || errno == ECONNRESET;
    }
    close(fd);
    return alive;
}

static void rssi_task(void *arg)
{
    (void)arg;
    /* The signal, every 3 s, and a watchdog on the C6 with it. The C6 now and then stops answering
     * altogether (under traffic; esp-hosted 1.4.7 on both ends, SDIO at 20 MHz, the host polling its
     * interrupt register: seen all the same), and nothing short of resetting it brings Wi-Fi back: two
     * unanswered asks in a row while "connected" restart the tablet, whose start-up resets the C6 through
     * its reset line (c6_restart: spaced out when they come often, never given up). */
    if (s_c6_restarts[3] != 0xC6C6C6C6u) memset(s_c6_restarts, 0, sizeof s_c6_restarts);
    s_c6_restarts[3] = 0xC6C6C6C6u;
    int misses = 0, cycle = 0, dead = 0;
    bool was_up = false;
    uint32_t answered = 0; /* the gateway that has answered: only its silence is news (kept across a rejoin) */
    int64_t rejoined = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        wifi_ap_record_t ap;
        /* the reconnect the disconnect handler asked for, done here rather than on the event loop */
        int64_t now = esp_timer_get_time();
        if (s_issued && now - s_issued > CONNECT_LOST_US) s_issued = 0; /* neither joined nor dropped: gone */
        if (s_retry_at && now >= s_retry_at && !s_issued && !s_scanning && !N.up) {
            s_retry_at = 0;
            if (wifi_has_network()) {
                s_issued = now;
                esp_wifi_connect();
            }
            /* and a deadline for the next one either way: a connect that is refused, or accepted and then
             * never spoken of again, still gets asked about later. The address clears it (IP_EVENT), and
             * s_issued keeps this deadline from firing on top of an attempt that is still working. */
            reconnect_soon(false);
        }
        /* The beacon-loss window again on every fresh join, if it ever becomes settable: Espressif says it
         * is "not stored into flash" and doesn't say whether it outlives a disconnect, and a 6 s window
         * silently coming back would be the whole fault. Skipped while it reads as the default, which is
         * what esp-hosted 1.4.7 leaves it at. Here rather than in the connect event, for the same reason
         * the reconnect moved: the event loop's task must not wait on SDIO. */
        if (N.up && !was_up && N.inactive_s > 6) esp_wifi_set_inactive_time(WIFI_IF_STA, N.inactive_s);
        was_up = N.up;
        if (!N.up || s_scanning) {
            misses = 0;
            dead = 0;
            continue;
        }
        /* The other way the C6 fails: it answers its RPCs, reports "connected", and passes no packets. Only a
         * failed request used to notice (wifi_heal), so with nothing asking Wi-Fi stayed dead a minute or more.
         * The gateway is asked every 15 s, every 3 s once it has gone quiet: twice quiet, join again; quiet
         * twice more after that, reset the C6. A gateway that has never answered (one that drops port 80
         * without a word) says nothing either way: it's left alone. */
        if (++cycle >= 5 || dead) {
            cycle = 0;
            uint32_t gw;
            if (gw_alive(&gw)) {
                if (dead) ESP_LOGI(TAG, "wi-fi: the gateway answers again");
                dead = 0;
                answered = gw;
                s_gw_answered = gw;
            } else if (gw && gw == answered) {
                dead++;
                int64_t now = esp_timer_get_time();
                if (dead == 2 && (!rejoined || now - rejoined > 60 * 1000000LL)) {
                    ESP_LOGW(TAG, "wi-fi: connected but the gateway is silent: joining again");
                    rejoined = now;
                    hal_wifi_rejoin(); /* the disconnect handler asks for the reconnect, at once for a rejoin we chose */
                } else if (dead >= 4) {
                    c6_restart("connected, but nothing gets through even after joining again", gw_ok);
                }
            }
        }
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            N.rssi = ap.rssi;
            misses = 0;
            continue;
        }
        if (++misses < 2) continue;
        c6_restart("the C6 stopped answering", rpc_ok); /* returns only when it has to wait: asked again in 3 s */
    }
}

/* The station settings a drop usually turns out to be about. They travel to the C6 whole: esp-hosted's
 * wifi_sta_config proto message carries scan_method, sort_method, threshold, pmf_cfg, the rm/btm/mbo
 * bitmask and failure_retry_cnt, and rpc_req.c packs every one of them — so these are the C6's settings,
 * not the host's idea of them.
 *
 *  - ALL_CHANNEL_SCAN with sort by signal. The driver's default is FAST_SCAN, which "ends when the first
 *    matched AP is found": on a network with more than one AP — a mesh, an extender, a pit with repeaters
 *    — that is whichever answered first, not the strongest, and a station sitting on the far one loses
 *    beacons until it is thrown off (reason 200). All-channel scan looks at every AP with that name and
 *    takes the loudest. It also costs a full scan per attempt, which is why the reconnect above backs off.
 *  - failure_retry_cnt, which Espressif documents as only working with ALL_CHANNEL_SCAN: the C6 retries
 *    an AP three times, and moves to the next one with the same name, before it tells the host anything.
 *  - authmode threshold WPA, not WPA2. With a password set and no threshold the driver defaults to WPA2
 *    and refuses a WPA-only AP outright (reason 211) — this file used to ask for WPA2 explicitly, which is
 *    the same refusal written down. WPA2 and WPA3 are both above WPA, so nothing is given up but WEP.
 *  - pmf capable. The field is deprecated (the C6 turns PMF on whenever the AP offers it) but costs
 *    nothing to state, and WPA3 needs it.
 *  - no BSSID and no channel pinned, ever: both would keep the station on one AP, which is the fault above.
 *  - rm/btm/mbo (802.11k/v) left off deliberately. They make the station honour the AP's request to move to
 *    another BSS, and acting on one needs a slave built with the roaming app (CONFIG_ESP_WIFI_ENABLE_ROAMING_APP);
 *    firmware/c6's slave is not. Advertising the capability and then not moving invites an AP to deauthenticate
 *    it (reason 12) — worse than not claiming it. ft (11r) is for WPA2-Enterprise. */
static void sta_tune(wifi_sta_config_t *s)
{
    s->scan_method = WIFI_ALL_CHANNEL_SCAN;
    s->sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    s->failure_retry_cnt = 3;
    s->threshold.rssi = 0; /* no floor: a weak AP still beats no AP */
    s->threshold.authmode = s->password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    s->pmf_cfg.capable = true;
    s->pmf_cfg.required = false;
    s->bssid_set = false;
    s->channel = 0;
    s->rm_enabled = 0;
    s->btm_enabled = 0;
    s->mbo_enabled = 0;
    s->ft_enabled = 0;
}

static void wifi_init(void)
{
    /* esp_wifi_* calls are forwarded over SDIO to the C6 by esp_wifi_remote + esp_hosted */
    N.sta = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi co-processor didn't answer over SDIO");
        return;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    /* 4 KB: its gateway probe opens a socket and selects on it */
    hal_kv_get("dns1", s_dns[0], sizeof s_dns[0]);
    hal_kv_get("dns2", s_dns[1], sizeof s_dns[1]);
    dns_apply();
    xTaskCreatePinnedToCore(rssi_task, "rssi", 4096, NULL, 2, &N.rssi_task, 0);
    /* esp-hosted logs every RPC at info (one line each 3 s from the signal poll alone) */
    esp_log_level_set("rpc_core", ESP_LOG_WARN);
    esp_log_level_set("rpc_rsp", ESP_LOG_WARN);
    esp_err_t m = esp_wifi_set_mode(WIFI_MODE_STA);
    /* The saved network gets the settings above before the station starts. Before, so it can't collide with
     * the connect the start triggers: esp_wifi_set_config answers ESP_ERR_WIFI_STATE to anything asked
     * while a connect is in flight, and it would have answered it silently. */
    wifi_config_t wc = { 0 };
    esp_err_t t = ESP_ERR_NOT_FOUND;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0]) {
        /* Writing the config back writes the whole struct, password included — so the password has to have
         * come back. It does here (esp-hosted's wifi_sta_config carries it and rpc_rsp.c copies it), but if
         * a slave ever withheld it, writing an empty one would take the tablet's saved network with it.
         * A stored threshold above open says the network has a password: if one didn't come back with it,
         * leave the whole thing alone. The settings screen's next join tunes it instead. */
        if (!wc.sta.password[0] && wc.sta.threshold.authmode > WIFI_AUTH_OPEN) {
            t = ESP_ERR_INVALID_STATE;
            ESP_LOGW(TAG, "wi-fi: the C6 didn't give back the password for '%.32s': leaving its settings as they are",
                     (const char *)wc.sta.ssid);
        } else {
            sta_tune(&wc.sta);
            t = esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
    }
    esp_wifi_set_ps(WIFI_PS_NONE); /* latency over battery: a dashboard wants its packets now. It is also the
                                    * right way round for staying joined — an always-awake station answers
                                    * every poll, and what an AP throws a client off for is silence
                                    * (reason 4), never wakefulness. */
    esp_err_t s = esp_wifi_start();
    /* The first join, from here rather than from the STA_START event: the poll that issues every later
     * reconnect only wakes every 3 s, and Wi-Fi at boot shouldn't wait for it. The event's own
     * reconnect_soon stays as the backstop for a start that this misses. */
    if (s == ESP_OK && wifi_has_network()) {
        s_issued = esp_timer_get_time();
        esp_wifi_connect();
    }
    /* The beacon-loss window, asked for and — on this stack — refused. The C6's default is 6 s: miss six
     * seconds of the AP's beacons, trivial on a crowded 2.4 GHz channel or a pit full of radios, and it
     * sends five probe requests and throws the connection away (reason 200). 30 s would be better, and
     * nothing would be lost by waiting, because the gateway probe in rssi_task catches a genuinely dead AP
     * in 15 s without the radio's opinion.
     *
     * It cannot be set from here. esp-hosted 1.4.7 defines the RPCs (Req_WifiSetInactiveTime 325,
     * Req_WifiGetInactiveTime 326) and implements neither end, so esp_wifi_set_inactive_time() falls
     * through to esp_wifi_remote's weak stub and returns ESP_ERR_NOT_SUPPORTED — the same shape as the
     * antenna API. Measured on the tablet: it returns not-supported and the window stays 6 s. The call and
     * the read-back stay, so a slave or a host component that grows the RPC starts working without an
     * edit, and so "net" reports the window the radio really has rather than the one that was asked for. */
    esp_err_t it = esp_wifi_set_inactive_time(WIFI_IF_STA, 30);
    uint16_t got = 0;
    N.inactive_s = esp_wifi_get_inactive_time(WIFI_IF_STA, &got) == ESP_OK && got ? got : 6;
    if (it != ESP_OK)
        ESP_LOGW(TAG, "wi-fi: the beacon-loss window stays at %us: %s (esp-hosted 1.4.7 doesn't carry it)",
                 (unsigned)N.inactive_s, esp_err_to_name(it));
    wifi_country_t cc = { 0 };
    esp_err_t c = esp_wifi_get_country(&cc);
    if (c == ESP_OK) {
        snprintf(N.country, sizeof N.country, "%.2s", cc.cc);
        N.cc_lo = cc.schan;
        N.cc_n = cc.nchan;
    }
    esp_wifi_get_max_tx_power(&N.txpower);
    esp_wifi_get_protocol(WIFI_IF_STA, &N.proto);
    esp_hosted_coprocessor_fwver_t fw = { 0 };
    if (esp_hosted_get_coprocessor_fwversion(&fw) == ESP_OK) {
        snprintf(N.fw, sizeof N.fw, "%u.%u.%u", (unsigned)fw.major1, (unsigned)fw.minor1, (unsigned)fw.patch1);
        /* At warn, and kept for "net": this line is how anyone tells which slave the C6 is running. 1.4.7 is
         * the packet-mode one this repository builds (firmware/c6); M5Stack's factory image is 1.4.1, and on
         * that one the SDIO streaming-mode fault the recovery machinery was built around is still present. */
        ESP_LOGW(TAG, "wi-fi: the C6 runs esp-hosted %s", N.fw);
    } else {
        snprintf(N.fw, sizeof N.fw, "?");
        ESP_LOGW(TAG, "wi-fi: the C6 doesn't report its esp-hosted version (older than 1.x)");
    }
    ESP_LOGI(TAG, "wi-fi: mode %s, tune %s, start %s, country %s %.2s ch %d+%d, beacon window %us, tx %d/4 dBm, proto 0x%02x",
             esp_err_to_name(m), esp_err_to_name(t), esp_err_to_name(s), esp_err_to_name(c), cc.cc, cc.schan,
             cc.nchan, (unsigned)N.inactive_s, N.txpower, N.proto);
}

/* drop the network and join it again (the disconnect handler asks for the reconnect): for the dev console's
 * "rejoin", and for the two watchdogs */
void hal_wifi_rejoin(void)
{
    s_retry_step = 0; /* asked for on purpose: the first try is immediate */
    esp_wifi_disconnect();
}

void hal_wifi_join(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass);
    sta_tune(&wc.sta);
    esp_wifi_disconnect();
    /* stored in NVS by the driver: rejoins on the next boot. The disconnect above is asked of the C6 and
     * answered before it has happened, so the first set_config can land while the station is still
     * connecting and be refused with ESP_ERR_WIFI_STATE — which used to be dropped on the floor, and the
     * network the user typed in was simply not saved. Tried again as the state settles. */
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 6; i++) {
        e = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (e != ESP_ERR_WIFI_STATE) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (e != ESP_OK) ESP_LOGE(TAG, "wi-fi: couldn't save '%.32s': %s", ssid, esp_err_to_name(e));
    s_retry_step = 0;
    s_issued = esp_timer_get_time();
    esp_wifi_connect();
}

int hal_wifi_scan(hal_ap_t *out, int max)
{
    /* a station in the middle of connecting won't scan: stop it for the scan, and resume after */
    s_scanning = true;
    if (!N.up) esp_wifi_disconnect();
    /* an explicit active scan of every channel: NULL (the driver's defaults) crosses the SDIO link to the
     * C6 as a config it may not fill in */
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 80, .max = 200 } },
    };
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(300));
        e = esp_wifi_scan_start(&sc, true);
    }
    /* the results are read before the station reconnects: starting a connect empties the C6's list */
    uint16_t n = 0;
    wifi_ap_record_t *rec = NULL;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "wi-fi scan: %s", esp_err_to_name(e));
    } else {
        uint16_t found = 0;
        esp_wifi_scan_get_ap_num(&found);
        n = found < (uint16_t)max ? found : (uint16_t)max;
        if (n && (rec = calloc(n, sizeof *rec)) && esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) n = 0;
        if (!rec) n = 0;
    }
    s_scanning = false;
    /* asked for, not done here: a connect issued straight after a scan from the settings page's own thread
     * raced the poll's one and the C6 saw two */
    if (!N.up && wifi_has_network()) reconnect_soon(true);
    for (int i = 0; i < n; i++) {
        snprintf(out[i].ssid, sizeof out[i].ssid, "%s", (const char *)rec[i].ssid);
        out[i].rssi = rec[i].rssi;
        out[i].secure = rec[i].authmode != WIFI_AUTH_OPEN;
    }
    free(rec);
    return n;
}

/* ------------------------------------------------------------------ USB tether */

/* How long DHCP gets before the fallback address goes on. Systemcore's dnsmasq answers in well under a
 * second; a robot radio's DHCP can be slower, but a static address in the team's subnet works as well. */
#define TETHER_DHCP_WAIT_US (6 * 1000 * 1000)
/* esp_netif makes the up interface with the highest route_prio lwIP's default netif. Wi-Fi STA is 100;
 * the tether sits below it, so the default route (the PC, the internet, the Claude API) stays on Wi-Fi
 * and only the tether's own subnet goes out the cable. With Wi-Fi down, the tether becomes the default. */
#define TETHER_ROUTE_PRIO 20

/* One iot_eth instance per protocol. A device is claimed by at most one of them (tether_gate()). */
enum { TE_RNDIS, TE_ECM, TE_N };

typedef struct {
    const char *kind;
    iot_eth_driver_t *drv;
    iot_eth_handle_t eth;
    esp_netif_t *netif;
    iot_eth_netif_glue_handle_t glue;
    esp_timer_handle_t dhcp_timer;
    /* written from the event loop and the USB tasks, read by hal_tether() under TT.lock */
    bool link, up, fallback;
    esp_netif_ip_info_t ip;
    uint8_t mac[6];
    int mbps;
    uint64_t rx, tx;
} tether_if_t;

static tether_if_t TE[TE_N];

static struct {
    portMUX_TYPE lock;
    bool fallback_set;
    esp_netif_ip_info_t fallback;
    char other[16];     /* a USB network chip nothing here drives, by name */
    uint8_t other_addr; /* its USB address, to notice it leaving */
    bool started;
} TT = { .lock = portMUX_INITIALIZER_UNLOCKED };

ESP_EVENT_DEFINE_BASE(TETHER_EVENT);
enum { TETHER_EVENT_DHCP_TIMEOUT };

/* Composite gadgets (Systemcore's: ECM and RNDIS in one configuration) would otherwise be claimed by both
 * drivers and come up as two interfaces. The ECM driver keeps a pointer to this list rather than a copy,
 * and iot_usbh_cdc evaluates each driver's list just before calling that driver's callback, in reverse
 * order of registration — so tether_gate(), registered last, runs first and can close this list to a
 * device that also offers RNDIS. RNDIS wins on Systemcore because the addresses the rest of the firmware
 * tries (172.26.0.1, docs/catalyst-contract.md) are on its RNDIS side; its ECM side is 172.27.x.
 * Both behaviours are iot_usbh_cdc 3.1's (checked in its source; the manifest pins ~3.1.0).
 * UNVERIFIED on a real Systemcore: that its gadget carries both functions in configuration 1 (only
 * configuration 1 enumerates), and which subnet its RNDIS side hands out. */
static usb_device_match_id_t s_ecm_match[] = {
    { .match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS,
      .idVendor = USB_DEVICE_VENDOR_ANY, .bInterfaceClass = USB_CLASS_COMM, .bInterfaceSubClass = 0x06 /* ECM */ },
    { 0 },
};
#define ECM_MATCH_NONE 0xFFFF /* a vendor id no real device has: closes s_ecm_match */

static bool has_rndis(const usb_config_desc_t *cfg)
{
    /* the same test iot_usbh_rndis makes: an interface association of two interfaces, either the
     * "wireless controller / RF / RNDIS" triple or Microsoft's "misc / common / RNDIS" one */
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION,
                                                  &off))) {
        const usb_iad_desc_t *iad = (const usb_iad_desc_t *)d;
        if (iad->bInterfaceCount == 2 &&
            ((iad->bFunctionClass == 0xE0 && iad->bFunctionSubClass == 0x01 && iad->bFunctionProtocol == 0x03) ||
             (iad->bFunctionClass == 0xEF && iad->bFunctionSubClass == 0x04 && iad->bFunctionProtocol == 0x01)))
            return true;
    }
    return false;
}

static bool has_ecm(const usb_config_desc_t *cfg)
{
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off))) {
        const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
        if (i->bInterfaceClass == USB_CLASS_COMM && i->bInterfaceSubClass == 0x06) return true;
    }
    return false;
}

/* USB-Ethernet chips that need a vendor driver ESP-IDF doesn't have: named, so the UI can say why a
 * plugged-in adapter does nothing. */
static const char *vendor_chip(uint16_t vid, uint16_t pid)
{
    if (vid == 0x0B95) return pid == 0x1790 ? "ax88179" : "asix";
    if (vid == 0x0BDA) return pid == 0x8152 ? "rtl8152" : pid == 0x8153 ? "rtl8153" : pid == 0x8156 ? "rtl8156" : "realtek";
    return NULL;
}

static void tether_gate(usbh_cdc_device_event_t ev, usbh_cdc_device_event_data_t *d, void *user)
{
    (void)user;
    if (ev == CDC_HOST_DEVICE_EVENT_CONNECTED) {
        const usb_config_desc_t *cfg = d->new_dev.active_config_desc;
        const usb_device_desc_t *dev = d->new_dev.device_desc;
        bool rndis = has_rndis(cfg), ecm = has_ecm(cfg);
        s_ecm_match[0].idVendor = rndis ? ECM_MATCH_NONE : USB_DEVICE_VENDOR_ANY;
        ESP_LOGI(TAG, "USB-A: %04x:%04x, config %u:%s%s", dev->idVendor, dev->idProduct, cfg->bConfigurationValue,
                 rndis ? " rndis" : "", ecm ? " ecm" : "");
        if (!rndis && !ecm) {
            const char *chip = vendor_chip(dev->idVendor, dev->idProduct);
            taskENTER_CRITICAL(&TT.lock);
            snprintf(TT.other, sizeof TT.other, "%s", chip ? chip : "usb");
            TT.other_addr = d->new_dev.dev_addr;
            taskEXIT_CRITICAL(&TT.lock);
            ESP_LOGW(TAG, "USB-A: no CDC-ECM or RNDIS interface (%s): not a network adapter this firmware drives",
                     chip ? chip : "unknown");
        }
    } else if (ev == CDC_HOST_DEVICE_EVENT_DISCONNECTED) {
        taskENTER_CRITICAL(&TT.lock);
        if (TT.other_addr && TT.other_addr == d->dev_gone.dev_addr) {
            TT.other[0] = 0;
            TT.other_addr = 0;
        }
        taskEXIT_CRITICAL(&TT.lock);
    }
}

/* Many Realtek RTL815x adapters answer as a vendor-specific device in configuration 1 and as CDC-ECM in
 * configuration 2 (the one Linux's cdc_ether binds to without r8152). The USB host library enumerates
 * configuration 1 unless told otherwise. UNVERIFIED per adapter: some firmwares put ECM elsewhere or
 * nowhere. ASIX AX88179 has no class configuration at all. */
static bool usb_enum_filter(const usb_device_desc_t *dev, uint8_t *config)
{
    if (dev->idVendor == 0x0BDA && dev->bNumConfigurations >= 2) *config = 2;
    return true;
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static tether_if_t *tether_by_eth(iot_eth_handle_t eth)
{
    for (int i = 0; i < TE_N; i++)
        if (TE[i].eth && TE[i].eth == eth) return &TE[i];
    return NULL;
}

static tether_if_t *tether_by_netif(esp_netif_t *n)
{
    for (int i = 0; i < TE_N; i++)
        if (TE[i].netif && TE[i].netif == n) return &TE[i];
    return NULL;
}

/* Byte counters: the tether's frames pass through these two on their way between iot_eth and lwIP. */
static esp_err_t tether_rx(iot_eth_handle_t eth, uint8_t *data, size_t len, void *user)
{
    (void)eth;
    tether_if_t *t = user;
    taskENTER_CRITICAL(&TT.lock);
    t->rx += len;
    taskEXIT_CRITICAL(&TT.lock);
    return esp_netif_receive(t->netif, data, len, NULL);
}

static esp_err_t tether_tx(void *h, void *buffer, size_t len)
{
    tether_if_t *t = tether_by_eth(h);
    esp_err_t e = iot_eth_transmit(h, buffer, len);
    if (t && e == ESP_OK) {
        taskENTER_CRITICAL(&TT.lock);
        t->tx += len;
        taskEXIT_CRITICAL(&TT.lock);
    }
    return e;
}

static void rx_free(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

/* lwIP's ip4_route() walks netif_list from its head and sends a packet out of the first up interface whose
 * own subnet contains the destination; only when none matches does it use the default netif. When the
 * tether and Wi-Fi are on the same subnet — the robot radio's 10.TE.AM.0/24 over Wi-Fi and a dongle into
 * the robot's switch — the one nearer the head wins, and netif_add() puts each interface at the head when
 * it starts, so the answer would depend on which came up last. The robot link is the point of the
 * tether, so it is moved to the head whenever it gets an address (and again whenever Wi-Fi does).
 * UNVERIFIED with both links live on one subnet (docs/tab5-hardware.md, first-boot checklist). Runs in
 * lwIP's own thread through esp_netif_tcpip_exec(), the only place netif_list may be touched. */
static esp_err_t promote_in_lwip(void *ctx)
{
    struct netif *n = esp_netif_get_netif_impl(ctx);
    if (!n || netif_list == n) return ESP_OK;
    for (struct netif **p = &netif_list; *p; p = &(*p)->next) {
        if (*p == n) {
            *p = n->next;
            n->next = netif_list;
            netif_list = n;
            break;
        }
    }
    return ESP_OK;
}

static void tether_promote(tether_if_t *t)
{
    if (t->netif && t->up) esp_netif_tcpip_exec(promote_in_lwip, t->netif);
}

static void tether_promote_all(void)
{
    for (int i = 0; i < TE_N; i++) tether_promote(&TE[i]);
}

static void apply_fallback(tether_if_t *t)
{
    /* DHCP must be stopped before a static address can be set; setting it posts IP_EVENT_ETH_GOT_IP.
     * UNVERIFIED: the fallback and the re-arm on unplug, on a network with no DHCP server */
    esp_netif_dhcpc_stop(t->netif);
    esp_netif_ip_info_t ip = TT.fallback;
    t->fallback = true;
    esp_netif_set_ip_info(t->netif, &ip);
    ESP_LOGI(TAG, "tether %s: no DHCP answer, fallback " IPSTR "/" IPSTR, t->kind, IP2STR(&ip.ip), IP2STR(&ip.netmask));
}

static void dhcp_timeout(void *arg)
{
    /* esp_timer's task is small; the work happens on the event loop */
    int i = (int)(intptr_t)arg;
    esp_event_post(TETHER_EVENT, TETHER_EVENT_DHCP_TIMEOUT, &i, sizeof i, 0);
}

static void tether_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IOT_ETH_EVENT) {
        tether_if_t *t = tether_by_eth(*(iot_eth_handle_t *)data);
        if (!t) return;
        if (id == IOT_ETH_EVENT_CONNECTED) {
            uint8_t mac[6] = { 0 };
            iot_eth_get_addr(t->eth, mac);
            /* the USB bus rate: the adapters' own link speed isn't exposed by these drivers, and for a
             * gadget like Systemcore's the bus is the link */
            int mbps = 0;
            usbh_cdc_port_handle_t port = t == &TE[TE_ECM] ? usb_ecm_get_cdc_port_handle(t->drv)
                                                            : usb_rndis_get_cdc_port_handle(t->drv);
            usb_device_handle_t dev = NULL;
            usb_device_info_t info;
            if (port && usbh_cdc_get_dev_handle(port, &dev) == ESP_OK && usb_host_device_info(dev, &info) == ESP_OK)
                mbps = info.speed == USB_SPEED_HIGH ? 480 : info.speed == USB_SPEED_FULL ? 12 : 1;
            taskENTER_CRITICAL(&TT.lock);
            memcpy(t->mac, mac, 6);
            t->mbps = mbps;
            t->link = true;
            t->up = false;
            taskEXIT_CRITICAL(&TT.lock);
            /* the glue's own handler (registered before this one) has already started DHCP */
            esp_timer_stop(t->dhcp_timer);
            esp_timer_start_once(t->dhcp_timer, TETHER_DHCP_WAIT_US);
            ESP_LOGI(TAG, "tether %s: link up, %d Mbit/s bus, MAC %02x:%02x:%02x:%02x:%02x:%02x", t->kind, mbps,
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else if (id == IOT_ETH_EVENT_DISCONNECTED) {
            esp_timer_stop(t->dhcp_timer);
            mdns_netif_action(t->netif, MDNS_EVENT_DISABLE_IP4);
            if (t->fallback) {
                /* re-arm DHCP for the next cable: clear the static address, and a stopped client restarts
                 * from INIT when the link comes back (esp_netif_action_connected) */
                esp_netif_ip_info_t zero = { 0 };
                esp_netif_set_ip_info(t->netif, &zero);
                esp_netif_dhcpc_start(t->netif);
            }
            taskENTER_CRITICAL(&TT.lock);
            t->link = t->up = t->fallback = false;
            memset(&t->ip, 0, sizeof t->ip);
            taskEXIT_CRITICAL(&TT.lock);
            ESP_LOGI(TAG, "tether %s: link down", t->kind);
        }
    } else if (base == IP_EVENT) {
        ip_event_got_ip_t *e = data;
        tether_if_t *t = tether_by_netif(e->esp_netif);
        if (!t) return;
        if (id == IP_EVENT_ETH_GOT_IP) {
            esp_timer_stop(t->dhcp_timer);
            taskENTER_CRITICAL(&TT.lock);
            t->ip = e->ip_info;
            t->up = true;
            taskEXIT_CRITICAL(&TT.lock);
            tether_promote(t);
            /* answer and resolve .local names over the cable too (robot.local is Systemcore's) */
            mdns_netif_action(t->netif, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4);
            ESP_LOGI(TAG, "tether %s: " IPSTR "/" IPSTR " (%s)", t->kind, IP2STR(&e->ip_info.ip),
                     IP2STR(&e->ip_info.netmask), t->fallback ? "fallback" : "dhcp");
        } else if (id == IP_EVENT_ETH_LOST_IP) {
            taskENTER_CRITICAL(&TT.lock);
            t->up = false;
            taskEXIT_CRITICAL(&TT.lock);
        }
    } else if (base == TETHER_EVENT && id == TETHER_EVENT_DHCP_TIMEOUT) {
        tether_if_t *t = &TE[*(int *)data];
        if (t->link && !t->up && TT.fallback_set) apply_fallback(t);
    }
}

static bool tether_if_init(tether_if_t *t, int index, const char *kind, iot_eth_driver_t *drv)
{
    t->kind = kind;
    t->drv = drv;
    iot_eth_config_t ec = { .driver = drv };
    if (iot_eth_install(&ec, &t->eth) != ESP_OK) return false;

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = index == TE_ECM ? "USB_ECM" : "USB_RNDIS";
    base.if_desc = index == TE_ECM ? "usb-ecm" : "usb-rndis";
    base.route_prio = TETHER_ROUTE_PRIO;
    esp_netif_config_t nc = { .base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    t->netif = esp_netif_new(&nc);
    t->glue = iot_eth_new_netif_glue(t->eth);
    if (!t->netif || !t->glue || esp_netif_attach(t->netif, t->glue) != ESP_OK) return false;
    /* after the glue's post-attach hook: route frames through the byte counters */
    iot_eth_update_input_path(t->eth, tether_rx, t);
    esp_netif_driver_ifconfig_t dc = { .handle = t->eth, .transmit = tether_tx, .driver_free_rx_buffer = rx_free };
    esp_netif_set_driver_config(t->netif, &dc);

    esp_timer_create_args_t ta = { .callback = dhcp_timeout, .arg = (void *)(intptr_t)index, .name = "tether-dhcp" };
    esp_timer_create(&ta, &t->dhcp_timer);
    mdns_register_netif(t->netif);
    return iot_eth_start(t->eth) == ESP_OK;
}

static void tether_init(void)
{
    /* The USB host library, installed here rather than by iot_usbh_cdc so the enumeration filter can pick
     * a Realtek adapter's ECM configuration. Its default controller on the P4 is the high-speed one: the
     * USB-A port. VBUS (E2.P3) is switched on just before (hal_settle). UNVERIFIED: enumeration through the Tab5's USB-A port (the
     * BSP's USB HID example uses the same controller and switch). */
    usb_host_config_t hc = { .intr_flags = ESP_INTR_FLAG_LEVEL1, .enum_filter_cb = usb_enum_filter };
    if (usb_host_install(&hc) != ESP_OK) {
        ESP_LOGE(TAG, "USB host didn't install: no tether");
        return;
    }
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL, 0);
    usbh_cdc_driver_config_t cc = { .task_stack_size = 4096, .task_priority = 5, .task_coreid = 0,
                                    .skip_init_usb_host_driver = true };
    if (usbh_cdc_driver_install(&cc) != ESP_OK) {
        ESP_LOGE(TAG, "USB CDC host driver didn't install: no tether");
        return;
    }

    esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, tether_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, tether_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, tether_event, NULL);
    esp_event_handler_register(TETHER_EVENT, ESP_EVENT_ANY_ID, tether_event, NULL);

    iot_eth_driver_t *rndis = NULL, *ecm = NULL;
    iot_usbh_rndis_config_t rc = { .match_id_list = ESP_USB_DEVICE_MATCH_ID_ANY };
    iot_usbh_ecm_config_t ecc = { .match_id_list = s_ecm_match };
    bool ok = iot_eth_new_usb_rndis(&rc, &rndis) == ESP_OK && tether_if_init(&TE[TE_RNDIS], TE_RNDIS, "rndis", rndis);
    ok = iot_eth_new_usb_ecm(&ecc, &ecm) == ESP_OK && tether_if_init(&TE[TE_ECM], TE_ECM, "ecm", ecm) && ok;
    /* last, so it's called first (see s_ecm_match) */
    usbh_cdc_register_dev_event_cb(ESP_USB_DEVICE_MATCH_ID_ANY, tether_gate, NULL);
    TT.started = ok;
    ESP_LOGI(TAG, "USB tether %s (RNDIS + CDC-ECM on the USB-A port)", ok ? "ready" : "partly failed");
}

void hal_tether(hal_tether_t *o)
{
    memset(o, 0, sizeof *o);
    /* the interface with an address, else the one with a link, else a device nothing drives */
    const tether_if_t *pick = NULL;
    taskENTER_CRITICAL(&TT.lock);
    for (int i = 0; i < TE_N && !pick; i++)
        if (TE[i].up) pick = &TE[i];
    for (int i = 0; i < TE_N && !pick; i++)
        if (TE[i].link) pick = &TE[i];
    tether_if_t t = pick ? *pick : (tether_if_t){ 0 };
    char other[16];
    snprintf(other, sizeof other, "%s", TT.other);
    taskEXIT_CRITICAL(&TT.lock);

    if (!pick) {
        /* a CDC port that's open but hasn't reported a link yet (ECM waits up to 4 s for a notification) */
        if ((TE[TE_ECM].drv && usb_ecm_get_cdc_port_handle(TE[TE_ECM].drv)) ||
            (TE[TE_RNDIS].drv && usb_rndis_get_cdc_port_handle(TE[TE_RNDIS].drv))) {
            o->present = true;
            snprintf(o->kind, sizeof o->kind, "%s", usb_ecm_get_cdc_port_handle(TE[TE_ECM].drv) ? "ecm" : "rndis");
        } else if (other[0]) {
            o->present = true;
            snprintf(o->kind, sizeof o->kind, "%s", other);
        }
        return;
    }
    o->present = true;
    o->up = t.up;
    o->dhcp = t.up && !t.fallback;
    snprintf(o->kind, sizeof o->kind, "%s", t.kind);
    if (t.up) {
        snprintf(o->ip, sizeof o->ip, IPSTR, IP2STR(&t.ip.ip));
        snprintf(o->gw, sizeof o->gw, IPSTR, IP2STR(&t.ip.gw));
        snprintf(o->mask, sizeof o->mask, IPSTR, IP2STR(&t.ip.netmask));
    }
    memcpy(o->mac, t.mac, 6);
    o->rx_bytes = t.rx;
    o->tx_bytes = t.tx;
    o->mbps = t.mbps;
}

void hal_tether_fallback(const char *ip, const char *mask)
{
    esp_netif_ip_info_t f = { 0 };
    if (!ip || !mask || esp_netif_str_to_ip4(ip, &f.ip) != ESP_OK || esp_netif_str_to_ip4(mask, &f.netmask) != ESP_OK) {
        TT.fallback_set = false;
        return;
    }
    TT.fallback = f;
    TT.fallback_set = true;
    /* an interface already on the old fallback moves to the new one */
    for (int i = 0; i < TE_N; i++)
        if (TE[i].netif && TE[i].fallback && TE[i].link) apply_fallback(&TE[i]);
}

/* for the dev console's "net": the Wi-Fi interface's address, gateway, DNS servers, and which interface is
 * the default, plus a lookup of `host` */
void hal_net_report(char *out, size_t n, const char *host)
{
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_dns_info_t d0 = { 0 }, d1 = { 0 };
    if (N.sta) {
        esp_netif_get_ip_info(N.sta, &ip);
        esp_netif_get_dns_info(N.sta, ESP_NETIF_DNS_MAIN, &d0);
        esp_netif_get_dns_info(N.sta, ESP_NETIF_DNS_BACKUP, &d1);
    }
    esp_netif_t *def = esp_netif_get_default_netif();
    int r = -1;
    char resolved[20] = "-";
    if (host && host[0]) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
        r = getaddrinfo(host, "443", &hints, &res);
        if (r == 0 && res) {
            struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
            inet_ntoa_r(sa->sin_addr, resolved, sizeof resolved);
        }
        if (res) freeaddrinfo(res);
    }
    /* and a plain TCP connect to 1.1.1.1:443 and to the gateway's :80, 3 s each: does anything get out */
    int tcp[2] = { -1, -1 };
    const char *dst[2] = { "1.1.1.1", NULL };
    char gw[16];
    snprintf(gw, sizeof gw, IPSTR, IP2STR(&ip.gw));
    dst[1] = gw;
    for (int i = 0; i < 2; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(i ? 80 : 443) };
        inet_aton(dst[i], &sa.sin_addr);
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        connect(fd, (struct sockaddr *)&sa, sizeof sa);
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        struct timeval tv = { .tv_sec = 3 };
        int so = -1;
        socklen_t sl = sizeof so;
        if (select(fd + 1, NULL, &w, NULL, &tv) > 0) getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl);
        tcp[i] = so;
        close(fd);
    }
    snprintf(out, n, "tcp 1.1.1.1 %d gw %d; up %d ip " IPSTR " gw " IPSTR " dns " IPSTR " / " IPSTR " default %s; %s -> %s (%d)",
             tcp[0], tcp[1], N.up,
             IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR(&d0.ip.u_addr.ip4), IP2STR(&d1.ip.u_addr.ip4),
             def ? esp_netif_get_desc(def) : "none", host ? host : "", resolved, r);
}

/* The radio's own side of it, for the dev console's "net": which slave the C6 is running, what the station's
 * settings actually are after wifi_init, and how the last drop was explained. Everything here is read back
 * from the C6 at start-up rather than assumed, so a setting that didn't take shows as what it is. */
void hal_wifi_diag(char *out, size_t n)
{
    wifi_config_t wc = { 0 };
    bool have = esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK;
    wifi_ap_record_t ap = { 0 };
    bool joined = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    int a = hal_antenna_get();
    snprintf(out, n,
             "c6 esp-hosted %s; saved '%.32s' authmode>=%d scan %s sort %s retry %u pmf %d; "
             "beacon window %us tx %d/4dBm proto 0x%02x country %s ch %u+%u antenna %s; "
             "joined %d ch %u rssi %d bssid %02x:%02x:%02x:%02x:%02x:%02x; drops %u last reason %d (%s) at rssi %d",
             N.fw[0] ? N.fw : "?", have ? (const char *)wc.sta.ssid : "", have ? wc.sta.threshold.authmode : -1,
             have && wc.sta.scan_method == WIFI_ALL_CHANNEL_SCAN ? "all" : "fast",
             have && wc.sta.sort_method == WIFI_CONNECT_AP_BY_SIGNAL ? "rssi" : "security",
             have ? (unsigned)wc.sta.failure_retry_cnt : 0u, have ? wc.sta.pmf_cfg.capable : 0,
             (unsigned)N.inactive_s, N.txpower, N.proto, N.country[0] ? N.country : "?", (unsigned)N.cc_lo,
             (unsigned)N.cc_n, a < 0 ? "?" : a ? "external (MMCX)" : "internal", joined, (unsigned)ap.primary,
             joined ? ap.rssi : 0, ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5],
             (unsigned)N.drops, N.drop_reason, drop_name(N.drop_reason), N.drop_rssi);

    /* Uptime and the watchdog's own history, kept across the restarts it makes. A restart loop is
     * invisible in everything above -- every counter it prints is reset by the restart. */
    size_t k = strlen(out);
    if (k + 80 >= n) return;
    uint32_t wall = (uint32_t)time(NULL);
    int recent = 0;
    for (int i = 0; i < 3; i++)
        if (s_c6_restarts[i] && wall > s_c6_restarts[i] && wall - s_c6_restarts[i] < 3600) recent++;
    snprintf(out + k, n - k, "; up %us, %d watchdog restart%s in the last hour%s",
             (unsigned)(esp_timer_get_time() / 1000000), recent, recent == 1 ? "" : "s",
             s_filtered ? ", this network filters (TLS answered by the wrong certificate)" : "");
}

/* esp-hosted's OTA steps (rpc_wrap.c; not in its public header): the image goes over the SDIO link in
 * chunks and the C6 writes it to its other OTA slot, switching to it only when the whole image checks out */
int rpc_ota_begin(void);
int rpc_ota_write(uint8_t *ota_data, uint32_t ota_data_len);
int rpc_ota_end(void);

/* The C6's firmware updated from a file on the card (the dev console's "c6ota"). progress(done, total) is
 * called every ~64 KB. The P4 must restart after a success: the C6 restarts into the new firmware. */
bool hal_c6_ota(const char *path, void (*progress)(size_t done, size_t total), char *err, size_t errn)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errn, "can't open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *chunk = heap_caps_malloc(1400, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (total < 100 * 1024 || !chunk) {
        fclose(f);
        free(chunk);
        snprintf(err, errn, total < 100 * 1024 ? "image too small (%ld bytes)" : "no memory", total);
        return false;
    }
    if (rpc_ota_begin() != 0) {
        fclose(f);
        free(chunk);
        snprintf(err, errn, "the C6 refused to begin");
        return false;
    }
    size_t done = 0, next = 0;
    bool ok = true;
    for (;;) {
        size_t n = fread(chunk, 1, 1400, f);
        if (n == 0) break;
        if (rpc_ota_write(chunk, (uint32_t)n) != 0) {
            snprintf(err, errn, "the C6 refused a write at %u of %ld", (unsigned)done, total);
            ok = false;
            break;
        }
        done += n;
        if (progress && done >= next) {
            progress(done, (size_t)total);
            next = done + 64 * 1024;
        }
    }
    fclose(f);
    free(chunk);
    /* ended either way: a failed image leaves the C6 on the firmware it runs now */
    int e = rpc_ota_end();
    if (ok && e != 0) {
        snprintf(err, errn, "the C6 didn't accept the image (end %d)", e);
        ok = false;
    }
    if (ok && done != (size_t)total) {
        snprintf(err, errn, "read %u of %ld bytes", (unsigned)done, total);
        ok = false;
    }
    return ok;
}

void hal_net(hal_net_t *o)
{
    memset(o, 0, sizeof *o);
    bool usb = false;
    taskENTER_CRITICAL(&TT.lock);
    for (int i = 0; i < TE_N; i++) usb |= TE[i].up;
    taskEXIT_CRITICAL(&TT.lock);
    o->link = usb ? HAL_LINK_USB : HAL_LINK_WIFI;
    o->up = N.up;
    snprintf(o->ip, sizeof o->ip, "%s", N.up ? N.ip : "");
    if (N.up) {
        snprintf(o->ssid, sizeof o->ssid, "%s", N.ssid);
        o->rssi = N.rssi;
    }
}

/* ------------------------------------------------------------------ mDNS browse */

int hal_mdns_browse(const char *service, const char *proto, hal_service_t *out, int max, int timeout_ms)
{
    if (max <= 0 || !service || !proto) return 0;
    int64_t t0 = esp_timer_get_time();
    mdns_result_t *res = NULL;
    /* extra room: the same instance answers once per interface (Wi-Fi and the tether) */
    if (mdns_query_ptr(service, proto, (uint32_t)(timeout_ms > 0 ? timeout_ms : 1000), (size_t)max * 2, &res) != ESP_OK)
        return 0;
    int n = 0;
    for (mdns_result_t *r = res; r && n < max; r = r->next) {
        if (!r->instance_name) continue;
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) dup = !strncmp(out[i].name, r->instance_name, sizeof out[i].name - 1);
        if (dup) continue;
        hal_service_t *s = &out[n];
        memset(s, 0, sizeof *s);
        snprintf(s->name, sizeof s->name, "%s", r->instance_name);
        if (r->hostname) snprintf(s->host, sizeof s->host, "%s.local", r->hostname);
        s->port = r->port;
        for (mdns_ip_addr_t *a = r->addr; a && !s->ip[0]; a = a->next)
            if (a->addr.type == ESP_IPADDR_TYPE_V4) snprintf(s->ip, sizeof s->ip, IPSTR, IP2STR(&a->addr.u_addr.ip4));
        if (!s->ip[0] && r->hostname) {
            /* the answer carried no A record: ask for it, within what's left of the timeout */
            int left = timeout_ms - (int)((esp_timer_get_time() - t0) / 1000);
            esp_ip4_addr_t ip;
            if (left > 100 && mdns_query_a(r->hostname, (uint32_t)(left < 1000 ? left : 1000), &ip) == ESP_OK)
                snprintf(s->ip, sizeof s->ip, IPSTR, IP2STR(&ip));
        }
        if (!s->host[0]) snprintf(s->host, sizeof s->host, "%s", s->ip);
        n++;
    }
    mdns_query_results_free(res);
    return n;
}

/* ------------------------------------------------------------------ HTTP(S) */

struct hal_http {
    esp_http_client_handle_t c;
    int timeout_ms;
    bool done;
    bool read_any;                 /* a body byte has been handed over: only then does "complete" mean "read" */
    char last_modified[40];
};

/* response headers arrive one at a time through the client's events: keep the one a conditional GET needs */
static esp_err_t http_event(esp_http_client_event_t *e)
{
    hal_http_t *h = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && h && e->header_key && e->header_value &&
        !strcasecmp(e->header_key, "Last-Modified"))
        snprintf(h->last_modified, sizeof h->last_modified, "%s", e->header_value);
    return ESP_OK;
}

static void http_err(char *err, size_t n, const char *what, esp_err_t e)
{
    if (err && n) snprintf(err, n, "%s (%s)", what, esp_err_to_name(e));
}

static esp_http_client_method_t http_method(const char *m)
{
    if (!m || !strcasecmp(m, "GET")) return HTTP_METHOD_GET;
    if (!strcasecmp(m, "POST")) return HTTP_METHOD_POST;
    if (!strcasecmp(m, "PUT")) return HTTP_METHOD_PUT;
    if (!strcasecmp(m, "PATCH")) return HTTP_METHOD_PATCH;
    if (!strcasecmp(m, "DELETE")) return HTTP_METHOD_DELETE;
    if (!strcasecmp(m, "HEAD")) return HTTP_METHOD_HEAD;
    return HTTP_METHOD_GET;
}

/* "Name: value\r\n" lines into esp_http_client_set_header() calls */
static void http_headers(esp_http_client_handle_t c, const char *h)
{
    while (h && *h) {
        const char *eol = strpbrk(h, "\r\n");
        size_t len = eol ? (size_t)(eol - h) : strlen(h);
        const char *colon = memchr(h, ':', len);
        if (colon && colon > h) {
            char *line = malloc(len + 1);
            if (line) {
                memcpy(line, h, len);
                line[len] = 0;
                char *name = line, *value = line + (colon - h) + 1;
                line[colon - h] = 0;
                while (*value == ' ' || *value == '\t') value++;
                esp_http_client_set_header(c, name, value);
                free(line);
            }
        }
        h += len;
        while (*h == '\r' || *h == '\n') h++;
    }
}

/* The C6 sometimes goes on reporting "connected, with an address" while nothing gets through (no DNS, no TCP
 * even to the gateway; the PC can't ping the tablet). Joining the network again clears it: three requests
 * failing in a row with Wi-Fi "up" rejoin it, at most once a minute. */
static volatile int s_http_fails;

/* A request failed. Decide whether that says anything about the LINK -- and usually it does not.
 *
 * It used to end in c6_restart(), which restarts the tablet. On a filtered network that is a loop
 * with no exit: every request fails for a reason the radio cannot fix, so the tablet restarts, comes
 * back, fails again. Measured on `smchs-acc`, where api.openai.com resolves to a block page whose
 * certificate is not OpenAI's.
 *
 * So this can now do one thing at most: ask for a rejoin, and only when the link is implicated --
 * the gateway answered on this network before and has since gone quiet. Restarting the C6 belongs to
 * rssi_task, which watches the C6's own RPC and has evidence about the radio rather than about a
 * route to one host.
 *
 * `tls_failed` says the connection reached something and it was not who we asked for. That is proof
 * packets flow, not evidence they do not, so it is not counted at all. */
static void wifi_heal(bool tls_failed)
{
    static int64_t last;
    if (tls_failed) {
        /* Reached, and answered by the wrong certificate: a filter, a captive portal, or a proxy. */
        if (!s_filtered) ESP_LOGW(TAG, "wi-fi: something on this network answers for hosts we ask for "
                                       "(TLS did not match): treating it as filtered, not as broken");
        s_filtered = true;
        s_http_fails = 0;
        return;
    }
    /* both workers' requests land here (assist and the companion): counted atomically */
    if (!N.up || __atomic_add_fetch(&s_http_fails, 1, __ATOMIC_RELAXED) < 3) return;

    /* The guard rssi_task always had and this path never did. A gateway that has never answered a
     * probe on this network -- an enterprise controller that drops port 80 without a word -- is
     * silent whether the link works or not, so its silence is not evidence. */
    if (!s_gw_answered) {
        static int64_t said;
        int64_t now = esp_timer_get_time();
        if (now - said > 60 * 1000000LL) {
            ESP_LOGW(TAG, "wi-fi: requests are failing, but this network's gateway has never answered "
                          "a probe, so nothing here says the link is at fault: leaving it alone");
            said = now;
        }
        s_http_fails = 0;
        return;
    }
    uint32_t gw = 0;
    if (gw_alive(&gw)) {
        /* The link carries packets. Whatever failed, failed further out than this tablet. */
        s_http_fails = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (last && now - last < 120 * 1000000LL) return;  /* one rejoin every two minutes, and no more */
    last = now;
    s_http_fails = 0;
    ESP_LOGW(TAG, "wi-fi: the gateway used to answer and has gone quiet while requests fail: joining again");
    hal_wifi_rejoin(); /* the disconnect handler asks for the reconnect, at once for a rejoin we chose */
}

hal_http_t *hal_http_open(const hal_http_req_t *req, int *status, char *err, size_t errn)
{
    if (status) *status = -1;
    if (err && errn) err[0] = 0;
    if (!req || !req->url) {
        http_err(err, errn, "no url", ESP_ERR_INVALID_ARG);
        return NULL;
    }
    int timeout = req->timeout_ms > 0 ? req->timeout_ms : 10000;
    hal_http_t *h = calloc(1, sizeof *h);
    if (!h) {
        http_err(err, errn, "no memory", ESP_ERR_NO_MEM);
        return NULL;
    }
    esp_http_client_config_t cfg = {
        .url = req->url,
        .method = http_method(req->method),
        .timeout_ms = timeout,
        .crt_bundle_attach = esp_crt_bundle_attach, /* verified against the CA bundle in flash */
        .buffer_size = 4096,
        .buffer_size_tx = 2048,                     /* the request head, with an API key in it */
        .user_agent = "catalyst-tab",
        .disable_auto_redirect = true,
        .event_handler = http_event,
        .user_data = h,
    };
    h->timeout_ms = timeout;
    h->c = esp_http_client_init(&cfg);
    if (!h->c) {
        http_err(err, errn, "bad url", ESP_ERR_INVALID_ARG);
        free(h);
        return NULL;
    }
    http_headers(h->c, req->headers);
    size_t blen = req->body ? req->body_len : 0;
    /* The C6 now and then stops answering for a few seconds (its RPCs time out alongside): a request caught in
     * that fails to connect, to send, or to get headers. Nothing has been answered yet at any of those
     * points, so one more try a moment later is safe for any method. Only after a quick failure, though: one
     * that already took most of the timeout would double the wait of whoever asked. */
    for (int attempt = 0;; attempt++) {
        int64_t t0 = esp_timer_get_time();
        const char *what = NULL;
        esp_err_t why = ESP_FAIL;
        int tls = 0;
        esp_err_t e = esp_http_client_open(h->c, (int)blen);
        if (e != ESP_OK) {
            int flags = 0;
            esp_http_client_get_and_clear_last_tls_error(h->c, &tls, &flags);
            what = "can't connect";
            why = e;
        }
        for (size_t sent = 0; !what && sent < blen;) {
            int w = esp_http_client_write(h->c, req->body + sent, (int)(blen - sent));
            if (w <= 0) what = "send failed";
            else sent += (size_t)w;
        }
        if (!what && esp_http_client_fetch_headers(h->c) < 0) {
            what = "no response";
            why = ESP_ERR_HTTP_FETCH_HEADER;
        }
        if (!what) {
            s_http_fails = 0;
            break;
        }
        if (attempt == 0 && N.up && esp_timer_get_time() - t0 < (int64_t)timeout * 1000 / 3) {
            esp_http_client_close(h->c);
            vTaskDelay(pdMS_TO_TICKS(1200));
            continue;
        }
        if (tls && err && errn) snprintf(err, errn, "TLS failed (-0x%x)", -tls);
        else http_err(err, errn, what, why);
        hal_http_close(h);
        wifi_heal(tls != 0);
        return NULL;
    }
    if (status) *status = esp_http_client_get_status_code(h->c);
    return h;
}

int hal_http_read(hal_http_t *h, char *buf, int max)
{
    if (!h || max <= 0) return -1;
    if (h->done) return 0;
    /* all of the body already in (the last chunk came with the previous read): asking for one more byte would
     * wait out the whole timeout for nothing — the companion sat 20 s "speaking" after every spoken answer */
    /* (only with a length or chunks to go by: a body of unknown length, ended by the server closing, counts as
     * "complete" from the start) */
    /* …and only once a byte has actually come out of here. esp_http_client counts a body into
     * response->data_process inside http_on_body, which runs from esp_http_client_fetch_headers() whenever
     * the headers and the whole body land in one TCP segment. A short answer therefore reads as "complete"
     * before a single byte has been handed over, and this shortcut used to throw it away: the caller got
     * HTTP 200 and an empty body. It is exactly what killed speech-to-text — an OpenAI transcription is
     * forty-odd bytes and always arrives in one segment, so it was empty every single time, while the
     * kilobytes of a chat completion or a spoken answer span segments and came through fine. */
    if (h->read_any && (esp_http_client_get_content_length(h->c) > 0 || esp_http_client_is_chunked_response(h->c)) &&
        esp_http_client_is_complete_data_received(h->c)) {
        h->done = true;
        return 0;
    }
    /* esp_http_client_read() keeps reading until it has `len` body bytes, the body ends, or a read times
     * out — so asking for a buffer's worth would hold a Server-Sent Event back until more arrived behind
     * it. Instead: wait (with the request's timeout) for one byte, then take whatever else is already
     * here without waiting. Chunked framing is removed by the client's parser either way. UNVERIFIED
     * against the Claude API's stream on the unit (the logic is esp_http_client 5.5.1's, read in source). */
    esp_http_client_set_timeout_ms(h->c, h->timeout_ms);
    int r = esp_http_client_read(h->c, buf, 1);
    if (r == 0) {
        /* the end of the body, or the connection closing early */
        h->done = true;
        return esp_http_client_is_complete_data_received(h->c) ? 0 : -1;
    }
    if (r < 0) return -1; /* includes -ESP_ERR_HTTP_EAGAIN: nothing within the timeout */
    h->read_any = true;
    if (max > 1) {
        esp_http_client_set_timeout_ms(h->c, 0);
        int more = esp_http_client_read(h->c, buf + 1, max - 1);
        if (more > 0) r += more;
    }
    return r;
}

const char *hal_http_last_modified(const hal_http_t *h) { return h ? h->last_modified : ""; }

void hal_http_close(hal_http_t *h)
{
    if (!h) return;
    if (h->c) {
        esp_http_client_close(h->c);
        esp_http_client_cleanup(h->c);
    }
    free(h);
}

/* ------------------------------------------------------------------ worker threads */

typedef struct {
    void *(*fn)(void *);
    void *arg;
    bool psram;
} thread_start_t;

static void thread_main(void *p)
{
    thread_start_t s = *(thread_start_t *)p;
    free(p);
    s.fn(s.arg);
    if (s.psram) vTaskDeleteWithCaps(NULL);
    else vTaskDelete(NULL);
}

/* A thread whose stack must be internal RAM: it touches flash (a model partition mapped in, NVS), and while
 * flash is busy the cache is off and a PSRAM stack unreachable (a crash). false rather than PSRAM. */
bool hal_thread_internal(const char *name, void *(*fn)(void *), void *arg, int stack)
{
    if (stack < 4096) stack = 4096;
    thread_start_t *s = malloc(sizeof *s);
    if (!s) return false;
    *s = (thread_start_t){ fn, arg, false };
    if (xTaskCreatePinnedToCore(thread_main, name, (uint32_t)stack, s, 4, NULL, 0) == pdPASS) return true;
    free(s);
    ESP_LOGE(TAG, "thread %s: no internal RAM for its %d-byte stack", name, stack);
    return false;
}

bool hal_thread(const char *name, void *(*fn)(void *), void *arg, int stack)
{
    /* Core 0: core 1 renders. Priority 4, under the NetworkTables client (5), so a slow TLS handshake
     * never delays the robot's numbers.
     *
     * Stacks come from internal RAM. A worker may write NVS (settings, a key), and while flash is being
     * written the cache is off and PSRAM unreachable — a task whose stack is in PSRAM would fault. TLS
     * wants >= 12 KB of stack, but its record buffers are in PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC), so
     * a handful of 12–16 KB internal stacks fits beside everything else. Only when internal RAM can't
     * hold the stack does it go to PSRAM, with a warning: that thread must then leave NVS alone
     * (hal_kv_set() refuses to run on such a stack). */
    if (stack < 4096) stack = 4096;
    thread_start_t *s = malloc(sizeof *s);
    if (!s) return false;
    *s = (thread_start_t){ fn, arg, false };
    if (xTaskCreatePinnedToCore(thread_main, name, (uint32_t)stack, s, 4, NULL, 0) == pdPASS) return true;
    s->psram = true;
    if (xTaskCreatePinnedToCoreWithCaps(thread_main, name, (uint32_t)stack, s, 4, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        ESP_LOGW(TAG, "thread %s: %d-byte stack in PSRAM (internal RAM short); it must not write NVS", name, stack);
        return true;
    }
    free(s);
    return false;
}

/* ------------------------------------------------------------------ init */

static void sntp_sync_cb(struct timeval *tv)
{
    struct tm tm;
    localtime_r(&tv->tv_sec, &tm);
    hal_rtc_set(&tm);
    ESP_LOGI(TAG, "time from the network: %04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min);
}

void hal_net_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    if (mdns_init() == ESP_OK) mdns_hostname_set("catalyst-tab");
    wifi_init();
    /* network time whenever Wi-Fi reaches the internet; the RTC is set from it (sntp_sync_cb) */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
}

void hal_net_tether_init(void)
{
    /* after Wi-Fi, once the UI is up (hal_settle): which of the two interfaces lwIP lists first depends
     * on start order, and tether_promote() settles it */
    tether_init();
}
