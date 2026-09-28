/* hal_tab5_ble — the BLE presence watch (hal.h, "BLE presence watch").
 *
 * All this does is scan for one advertisement and report whether it is in range. It never connects,
 * never reads a characteristic and never sends anything: the PC's Catalyst Link is found over BLE and
 * then talked to over HTTP on the LAN, which is the transport that already works and already has the
 * outbox, the token and the store-and-forward behind it. BLE is the doorbell; the LAN is the door.
 *
 * Why a scanner and not a beacon of our own: the tablet is the one that wants to know, and a scanner
 * costs the PC nothing. The PC is mains-powered and stationary, so it is the right side to advertise
 * from, and Windows can advertise manufacturer data without any driver work (link/catalyst_link/
 * presence.py).
 *
 * The radio is the C6's. There is no BLE controller in the ESP32-P4 at all, so NimBLE runs here as a
 * host with its controller at the far end of the SDIO bus: HCI frames are multiplexed onto the same
 * link that carries Wi-Fi (esp-hosted's ESP_HCI_IF, PRIO_Q_BT). That costs no extra pins, which is
 * just as well because there are none spare between the two chips. It does mean the C6's esp-hosted
 * image has to have been built with CONFIG_BT_ENABLED; images that were not simply never answer, so
 * `ready` below stays false and every caller is expected to carry on without presence information.
 *
 * Compiled out entirely without a NimBLE host stack, which is the default: see the note by
 * CONFIG_BT_NIMBLE_ENABLED in sdkconfig.defaults. The stubs at the bottom keep every caller building. */
#include "hal.h"

#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "ble";

#if CONFIG_BT_NIMBLE_ENABLED

#include <pthread.h>

#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

/* Scan duty cycle. The window is what costs: the C6 is listening on one antenna that Wi-Fi also wants,
 * and software coexistence resolves that by taking airtime away from one of them. 30 ms in every 300 ms
 * is a tenth of the radio, which against a PC advertising a few times a second finds it within a couple
 * of seconds and is small enough not to show up in the HTTP polling. Both are in the HCI's 0.625 ms
 * units. Passive: there is nothing to ask the advertiser for, and a scan request would only add noise. */
#define SCAN_ITVL_MS 300
#define SCAN_WIN_MS 30
#define UNITS(ms) ((uint16_t)((ms) * 8 / 5))
#define TICK_US 1000000 /* how often the hold window is re-examined */

#define ID_MAX 16

static struct {
    pthread_mutex_t lock;
    uint8_t id[ID_MAX];
    size_t idn;
    double hold_s;
    void (*on_change)(bool present, int rssi);
    hal_ble_t st;
    bool inited;     /* nimble_port_init() done: only ever once, it cannot be undone cheaply */
    bool want_scan;
    uint8_t own_addr_type;
    esp_timer_handle_t tick;
} B = { .lock = PTHREAD_MUTEX_INITIALIZER, .hold_s = 12.0 };

/* present is derived, never stored by the sighting path: one place decides what the hold window means,
 * and a caller that asks between ticks gets the same answer the tick would give. */
static bool present_locked(void)
{
    return B.st.last > 0 && hal_seconds() - B.st.last < B.hold_s;
}

/* Under the lock, works out whether present flipped and hands back the callback to run outside it --
 * on_change belongs to link.c and must not be able to deadlock this by asking hal_ble() back. */
static void (*settle_locked(bool *now, int *rssi))(bool, int)
{
    bool p = present_locked();
    if (p == B.st.present) return NULL;
    B.st.present = p;
    *now = p;
    *rssi = B.st.rssi;
    return B.on_change;
}

static void settle(void)
{
    bool now = false;
    int rssi = 0;
    pthread_mutex_lock(&B.lock);
    void (*cb)(bool, int) = settle_locked(&now, &rssi);
    pthread_mutex_unlock(&B.lock);
    if (cb) {
        if (now) ESP_LOGI(TAG, "the pc's beacon is in range, %d dBm", rssi);
        else ESP_LOGI(TAG, "the pc's beacon went quiet");
        cb(now, rssi);
    }
}

static void tick(void *arg)
{
    (void)arg;
    settle();
}

/* One advertisement. `mfg_data` from NimBLE keeps the company id in its first two bytes, so the id the
 * caller gave is matched against the whole field: link.c's id starts with the company id on purpose,
 * which keeps this function free of any opinion about the beacon's shape. */
static void saw_adv(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return;
    if (!f.mfg_data || f.mfg_data_len < 2) return;
    pthread_mutex_lock(&B.lock);
    bool match = B.idn && f.mfg_data_len >= B.idn && !memcmp(f.mfg_data, B.id, B.idn);
    if (match) {
        B.st.last = hal_seconds();
        B.st.rssi = d->rssi;
        B.st.seen++;
    }
    pthread_mutex_unlock(&B.lock);
    /* Only a match can turn present on, and the tick is what turns it off, so there is nothing to
     * settle for an advertisement that was not ours. */
    if (match) settle();
}

static int gap_event(struct ble_gap_event *e, void *arg);

static void scan_start(void)
{
    struct ble_gap_disc_params p = { 0 };
    p.itvl = UNITS(SCAN_ITVL_MS);
    p.window = UNITS(SCAN_WIN_MS);
    p.passive = 1;
    /* Duplicates are the point: the controller filtering them would report the PC once and then never
     * again, and "when did we last hear it" is the entire signal. */
    p.filter_duplicates = 0;
    p.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
    int rc = ble_gap_disc(B.own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc == BLE_HS_EALREADY) return;
    if (rc != 0) ESP_LOGW(TAG, "scan didn't start: %d", rc);
    pthread_mutex_lock(&B.lock);
    B.st.watching = rc == 0;
    pthread_mutex_unlock(&B.lock);
}

static int gap_event(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    switch (e->type) {
    case BLE_GAP_EVENT_DISC:
        saw_adv(&e->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE: {
        /* BLE_HS_FOREVER should mean this never arrives; if the controller ends the scan anyway, start
         * it again rather than going quietly deaf. */
        pthread_mutex_lock(&B.lock);
        bool again = B.want_scan;
        B.st.watching = false;
        pthread_mutex_unlock(&B.lock);
        if (again) scan_start();
        break;
    }
    default:
        break;
    }
    return 0;
}

/* The controller answered and the host is usable. With a C6 whose esp-hosted image has Bluetooth
 * compiled out this is never reached: the HCI frames go out over SDIO and nothing comes back, so
 * `ready` stays false for the life of the boot and every caller falls back to what it did before. */
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) ESP_LOGW(TAG, "no usable address: %d", rc);
    rc = ble_hs_id_infer_auto(0, &B.own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "the c6's controller has no address to scan from: %d", rc);
        return;
    }
    pthread_mutex_lock(&B.lock);
    B.st.ready = true;
    bool want = B.want_scan;
    pthread_mutex_unlock(&B.lock);
    ESP_LOGI(TAG, "the c6's ble controller answered: presence watch is live");
    if (want) scan_start();
}

static void on_reset(int reason)
{
    /* The C6 restarted under us (the Wi-Fi watchdog's territory) or the link dropped a frame the
     * controller minded. NimBLE re-syncs by itself; drop `ready` so nothing trusts the radio meanwhile. */
    ESP_LOGW(TAG, "the controller reset: %d", reason);
    pthread_mutex_lock(&B.lock);
    B.st.ready = B.st.watching = false;
    pthread_mutex_unlock(&B.lock);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run(); /* returns when nimble_port_stop() is called, which nothing here does */
    nimble_port_freertos_deinit();
}

/* Brings the stack up once, on whichever worker thread asks first. It has to be after the network is
 * up, because the HCI transport is esp-hosted's SDIO link: there is nothing to send an HCI Reset down
 * before then. link.c calls it from the poller, which is late enough. */
/* Its own lock, not B.lock: nimble_port_init() and nimble_port_freertos_init() can have on_sync running
 * before they return, and on_sync takes B.lock. Only one caller does this today, but a double
 * nimble_port_init() is the kind of thing that fails once on a Tuesday rather than in a test. */
static pthread_mutex_t g_init = PTHREAD_MUTEX_INITIALIZER;

static bool ensure_stack(void)
{
    pthread_mutex_lock(&g_init);
    if (B.inited) {
        pthread_mutex_unlock(&g_init);
        return true;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        pthread_mutex_unlock(&g_init);
        ESP_LOGW(TAG, "the ble host wouldn't start: %s", esp_err_to_name(err));
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    const esp_timer_create_args_t ta = { .callback = tick, .name = "ble-presence" };
    if (esp_timer_create(&ta, &B.tick) == ESP_OK) esp_timer_start_periodic(B.tick, TICK_US);
    B.inited = true;
    pthread_mutex_unlock(&g_init);
    pthread_mutex_lock(&B.lock);
    B.st.built = true;
    pthread_mutex_unlock(&B.lock);
    return true;
}

bool hal_ble_watch(const uint8_t *id, size_t n, double hold_s, void (*on_change)(bool present, int rssi))
{
    if (!id || !n || n > ID_MAX) return false;
    if (!ensure_stack()) return false;
    pthread_mutex_lock(&B.lock);
    bool changed = n != B.idn || memcmp(B.id, id, n) != 0;
    memcpy(B.id, id, n);
    B.idn = n;
    if (hold_s > 0) B.hold_s = hold_s;
    B.on_change = on_change;
    B.want_scan = true;
    if (changed) { /* a different PC: what we heard from the old one says nothing about this one */
        B.st.last = 0;
        B.st.rssi = 0;
        B.st.present = false;
    }
    bool ready = B.st.ready, watching = B.st.watching;
    pthread_mutex_unlock(&B.lock);
    if (ready && !watching) scan_start();
    return true;
}

void hal_ble_stop(void)
{
    pthread_mutex_lock(&B.lock);
    B.want_scan = false;
    B.idn = 0;
    B.st.present = false;
    B.st.last = 0;
    pthread_mutex_unlock(&B.lock);
    /* The host stack stays up: nimble_port_init() is not worth undoing for a watch that the owner may
     * switch back on, and a stopped scan is already the whole of the radio cost. */
    if (B.inited) ble_gap_disc_cancel();
    pthread_mutex_lock(&B.lock);
    B.st.watching = false;
    pthread_mutex_unlock(&B.lock);
}

void hal_ble(hal_ble_t *out)
{
    pthread_mutex_lock(&B.lock);
    *out = B.st;
    out->present = present_locked();
    pthread_mutex_unlock(&B.lock);
}

#else /* no BLE host stack in this build */

/* Note the gap_event/BLE_HS_FOREVER machinery above is not merely unused here, it is uncompilable:
 * without CONFIG_BT_NIMBLE_ENABLED the nimble headers are not on the include path at all. */

bool hal_ble_watch(const uint8_t *id, size_t n, double hold_s, void (*on_change)(bool present, int rssi))
{
    (void)id;
    (void)n;
    (void)hold_s;
    (void)on_change;
    static bool said;
    if (!said) {
        said = true;
        ESP_LOGD(TAG, "no ble in this build: presence stays unknown");
    }
    return false;
}

void hal_ble_stop(void) {}

void hal_ble(hal_ble_t *out) { memset(out, 0, sizeof *out); }

#endif
