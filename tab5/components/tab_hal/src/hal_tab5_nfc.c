/* NFC on Grove Port A: M5Stack's Unit RFID 2 (a WS1850S, which speaks the MFRC522's register set over I2C at
 * 0x28). The Tab5 has no NFC reader of its own (docs/tab5-hardware.md): nothing on the system I2C bus reads
 * a tag, so the tag that came in the box needs this unit plugged into Port A.
 *
 * Port A is its own I2C controller (I2C_NUM_0; the system bus is I2C_NUM_1) on SDA 53 / SCL 54, with the
 * port's 5 V switched by expander E1. hal_nfc_init() (from hal_settle) starts a small task that waits a few
 * seconds, switches the 5 V on and probes 0x28: nothing there, and the 5 V goes off again, the bus is
 * deleted and the task ends — the tablet without the unit pays nothing. Something there, and the task looks
 * for an ISO 14443A card every POLL_MS: the antenna on, WUPA, the anticollision/select cascade for the UID
 * (4, 7 or 10 bytes), the antenna off again (the field is on ~20 ms in every 300, and a card left on the
 * reader simply answers the next WUPA).
 *
 * The CAN tap uses the same two pins as TWAI: hal_can_start() calls hal_nfc_release(), which stops the task
 * and frees the bus for good (until the next start-up).
 *
 * UNVERIFIED on hardware: written from the MFRC522 datasheet and M5Stack's MFRC522_I2C library (register
 * access is <reg> then data, FIFO reads and writes repeat the one address); the WS1850S's version register
 * reads 0x15 or 0x92 depending on the batch, so any answer but 0x00/0xFF counts. */
#include "hal.h"
#include "hal_tab5_priv.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "nfc";

#define NFC_ADDR 0x28
#define NFC_SDA 53
#define NFC_SCL 54
#define PROBE_DELAY_MS 4000   /* after hal_settle: the rails are up and the UI is running */
#define POLL_MS 300
#define LOST_POLLS 10         /* the unit stops answering this many polls in a row: unplugged, stop */

/* MFRC522 registers and commands */
#define R_COMMAND 0x01
#define R_COMIRQ 0x04
#define R_DIVIRQ 0x05
#define R_ERROR 0x06
#define R_FIFODATA 0x09
#define R_FIFOLEVEL 0x0A
#define R_CONTROL 0x0C
#define R_BITFRAMING 0x0D
#define R_COLL 0x0E
#define R_MODE 0x11
#define R_TXCONTROL 0x14
#define R_TXASK 0x15
#define R_CRC_H 0x21
#define R_CRC_L 0x22
#define R_TMODE 0x2A
#define R_TPRESCALER 0x2B
#define R_TRELOAD_H 0x2C
#define R_TRELOAD_L 0x2D
#define R_VERSION 0x37
#define CMD_IDLE 0x00
#define CMD_CALCCRC 0x03
#define CMD_TRANSCEIVE 0x0C
#define CMD_SOFTRESET 0x0F

static struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t lock;       /* the card state only, held for a copy: the UI thread reads it */
    volatile bool present, stop, running;
    bool card;
    char uid[24];
} N;

/* ---- register access ---- */

static bool wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(N.dev, b, 2, 20) == ESP_OK;
}

static int rd(uint8_t reg)
{
    uint8_t v;
    return i2c_master_transmit_receive(N.dev, &reg, 1, &v, 1, 20) == ESP_OK ? v : -1;
}

static void set_bits(uint8_t reg, uint8_t mask)
{
    int v = rd(reg);
    if (v >= 0) wr(reg, (uint8_t)v | mask);
}

static void clear_bits(uint8_t reg, uint8_t mask)
{
    int v = rd(reg);
    if (v >= 0) wr(reg, (uint8_t)v & (uint8_t)~mask);
}

static bool fifo_write(const uint8_t *data, int n)
{
    uint8_t b[16];
    if (n > 15) return false;
    b[0] = R_FIFODATA;
    memcpy(b + 1, data, (size_t)n);
    return i2c_master_transmit(N.dev, b, (size_t)n + 1, 20) == ESP_OK;
}

static bool fifo_read(uint8_t *out, int n)
{
    uint8_t reg = R_FIFODATA;
    return n <= 0 || i2c_master_transmit_receive(N.dev, &reg, 1, out, (size_t)n, 20) == ESP_OK;
}

/* ---- the MFRC522 ---- */

static bool chip_init(void)
{
    wr(R_COMMAND, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < 10 && (rd(R_COMMAND) & 0x10); i++) vTaskDelay(pdMS_TO_TICKS(5)); /* PowerDown clears */
    int ver = rd(R_VERSION);
    if (ver <= 0 || ver == 0xFF) return false;
    /* the timer: 40 kHz, 1000 ticks = 25 ms, started at the end of each transmission (TAuto) */
    wr(R_TMODE, 0x80);
    wr(R_TPRESCALER, 0xA9);
    wr(R_TRELOAD_H, 0x03);
    wr(R_TRELOAD_L, 0xE8);
    wr(R_TXASK, 0x40);   /* 100 % ASK */
    wr(R_MODE, 0x3D);    /* CRC preset 0x6363 (ISO 14443-3) */
    clear_bits(R_TXCONTROL, 0x03);
    ESP_LOGI(TAG, "Unit RFID 2 on port A (version 0x%02X)", ver);
    return true;
}

typedef enum { TX_OK, TX_TIMEOUT, TX_ERROR, TX_COLLISION } tx_t;

/* Sends `n` bytes (the last one `last_bits` long, 0 = 8) and reads the answer into `back`. */
static tx_t transceive(const uint8_t *send, int n, uint8_t last_bits, uint8_t *back, int *back_n, int *valid_bits)
{
    wr(R_COMMAND, CMD_IDLE);
    wr(R_COMIRQ, 0x7F);
    wr(R_FIFOLEVEL, 0x80);
    if (!fifo_write(send, n)) return TX_ERROR;
    wr(R_BITFRAMING, last_bits);
    wr(R_COMMAND, CMD_TRANSCEIVE);
    set_bits(R_BITFRAMING, 0x80); /* StartSend */
    int irq = 0;
    for (int i = 0; i < 20; i++) { /* the chip's timer ends it at 25 ms; this is only a backstop */
        irq = rd(R_COMIRQ);
        if (irq < 0) return TX_ERROR;
        if (irq & 0x30) break;           /* RxIRq, IdleIRq */
        if (irq & 0x01) return TX_TIMEOUT; /* TimerIRq: no card answered */
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    clear_bits(R_BITFRAMING, 0x80);
    if (!(irq & 0x30)) return TX_TIMEOUT;
    int err = rd(R_ERROR);
    if (err < 0 || (err & 0x13)) return TX_ERROR; /* buffer overflow, parity, protocol */
    int got = rd(R_FIFOLEVEL);
    if (got < 0 || got > *back_n) return TX_ERROR;
    if (!fifo_read(back, got)) return TX_ERROR;
    *back_n = got;
    int ctl = rd(R_CONTROL);
    *valid_bits = ctl < 0 ? 0 : (ctl & 0x07);
    if (err & 0x08) return TX_COLLISION;
    return TX_OK;
}

static bool crc_a(const uint8_t *data, int n, uint8_t out[2])
{
    wr(R_COMMAND, CMD_IDLE);
    wr(R_DIVIRQ, 0x04);
    wr(R_FIFOLEVEL, 0x80);
    if (!fifo_write(data, n)) return false;
    wr(R_COMMAND, CMD_CALCCRC);
    for (int i = 0; i < 20; i++) {
        int d = rd(R_DIVIRQ);
        if (d < 0) return false;
        if (d & 0x04) {
            wr(R_COMMAND, CMD_IDLE);
            int lo = rd(R_CRC_L), hi = rd(R_CRC_H);
            if (lo < 0 || hi < 0) return false;
            out[0] = (uint8_t)lo;
            out[1] = (uint8_t)hi;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

/* WUPA, then the cascade: the card's UID into `uid` as hex. false with no card, or two at once. */
static bool read_card(char *uid, size_t un, bool *alive)
{
    *alive = true;
    set_bits(R_TXCONTROL, 0x03); /* the field on */
    vTaskDelay(pdMS_TO_TICKS(5)); /* a card needs a few ms of field to power up */
    bool ok = false;
    uint8_t atqa[2], wupa = 0x52, b[8];
    int n = sizeof atqa, vb = 0;
    clear_bits(R_COLL, 0x80);
    tx_t r = transceive(&wupa, 1, 7, atqa, &n, &vb);
    if (r == TX_ERROR && rd(R_VERSION) < 0) *alive = false;
    if (r == TX_OK && n == 2 && vb == 0) {
        uint8_t id[10];
        int idn = 0;
        static const uint8_t SEL[3] = { 0x93, 0x95, 0x97 };
        for (int level = 0; level < 3; level++) {
            uint8_t ac[2] = { SEL[level], 0x20 };
            n = 5;
            wr(R_BITFRAMING, 0);
            if (transceive(ac, 2, 0, b, &n, &vb) != TX_OK || n != 5) break;
            if ((b[0] ^ b[1] ^ b[2] ^ b[3]) != b[4]) break; /* BCC */
            uint8_t sel[9] = { SEL[level], 0x70, b[0], b[1], b[2], b[3], b[4] };
            if (!crc_a(sel, 7, &sel[7])) break;
            uint8_t sak[3];
            n = 3;
            if (transceive(sel, 9, 0, sak, &n, &vb) != TX_OK || n != 3) break;
            bool more = (sak[0] & 0x04) != 0; /* the UID goes on at the next level */
            if (more && b[0] == 0x88) {         /* the cascade tag: three bytes of UID here */
                memcpy(id + idn, b + 1, 3);
                idn += 3;
                continue;
            }
            memcpy(id + idn, b, 4);
            idn += 4;
            ok = !more;
            break;
        }
        if (ok) {
            size_t o = 0;
            for (int i = 0; i < idn && o + 3 <= un; i++) o += (size_t)snprintf(uid + o, un - o, "%02X", id[i]);
        }
    }
    clear_bits(R_TXCONTROL, 0x03); /* the field off until the next look */
    return ok;
}

/* ---- the task ---- */

static void teardown(void)
{
    if (N.dev) i2c_master_bus_rm_device(N.dev);
    if (N.bus) i2c_del_master_bus(N.bus);
    N.dev = NULL;
    N.bus = NULL;
}

static bool probe(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = NFC_SDA,
        .scl_io_num = NFC_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &N.bus) != ESP_OK) {
        N.bus = NULL;
        return false;
    }
    if (i2c_master_probe(N.bus, NFC_ADDR, 50) != ESP_OK) return false;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = NFC_ADDR, .scl_speed_hz = 100000 };
    if (i2c_master_bus_add_device(N.bus, &dc, &N.dev) != ESP_OK) {
        N.dev = NULL;
        return false;
    }
    return chip_init();
}

static void nfc_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(PROBE_DELAY_MS));
    /* only this task ever touches the bus (hal_nfc_release() asks it to stop and waits), so the bus needs no
     * lock; the lock guards the card state the UI copies out */
    bool found = false;
    if (!N.stop) {
        hal_tab5_ext5v(true); /* the unit is powered from the port's 5 V */
        vTaskDelay(pdMS_TO_TICKS(60));
        found = probe();
        if (!found) {
            teardown();
            hal_tab5_ext5v(false);
        }
    }
    N.present = found;
    int lost = 0;
    while (found && !N.stop) {
        char uid[24] = "";
        bool alive = true;
        bool card = read_card(uid, sizeof uid, &alive);
        xSemaphoreTake(N.lock, portMAX_DELAY);
        N.card = card;
        snprintf(N.uid, sizeof N.uid, "%s", card ? uid : "");
        xSemaphoreGive(N.lock);
        lost = alive ? 0 : lost + 1;
        if (lost >= LOST_POLLS) {
            ESP_LOGW(TAG, "the reader stopped answering: unplugged?");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    N.present = false;
    if (found) {
        teardown();
        if (!N.stop) hal_tab5_ext5v(false); /* the CAN tap, taking over, powers the port itself */
    }
    xSemaphoreTake(N.lock, portMAX_DELAY);
    N.card = false;
    xSemaphoreGive(N.lock);
    N.running = false;
    vTaskDelete(NULL);
}

void hal_nfc_init(void)
{
    if (N.lock) return;
    N.lock = xSemaphoreCreateMutex();
    if (!N.lock) return;
    N.running = true;
    /* low priority on core 0; I2C only, so a small internal stack */
    if (xTaskCreatePinnedToCore(nfc_task, "nfc", 3072, NULL, 2, NULL, 0) != pdPASS) N.running = false;
}

void hal_nfc_release(void)
{
    if (!N.lock) return;
    N.stop = true;
    for (int i = 0; i < 100 && N.running; i++) vTaskDelay(pdMS_TO_TICKS(10)); /* a poll takes ~30 ms */
}

bool hal_nfc_present(void) { return N.present; }

bool hal_nfc_card(char *uid, size_t n)
{
    if (!N.lock || !N.present) return false;
    xSemaphoreTake(N.lock, portMAX_DELAY);
    bool card = N.card;
    if (card) snprintf(uid, n, "%s", N.uid);
    xSemaphoreGive(N.lock);
    return card;
}
