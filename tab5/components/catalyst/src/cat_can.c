#include "cat_can.h"
#include <string.h>

cat_frc_id_t cat_can_decode(uint32_t id)
{
    cat_frc_id_t f = {
        .type = (uint8_t)((id >> 24) & 0x1f),
        .mfr = (uint8_t)((id >> 16) & 0xff),
        .api_class = (uint8_t)((id >> 10) & 0x3f),
        .api_index = (uint8_t)((id >> 6) & 0x0f),
        .device = (uint8_t)(id & 0x3f),
    };
    return f;
}

const char *cat_can_type_name(int t)
{
    static const char *const T[] = {
        "broadcast", "robot controller", "motor controller", "relay controller", "gyro", "accelerometer",
        "ultrasonic", "gear tooth sensor", "power distribution", "pneumatics", "miscellaneous", "io breakout",
        "servo controller",
    };
    if (t >= 0 && t < (int)(sizeof T / sizeof T[0])) return T[t];
    if (t == 31) return "firmware update";
    return "reserved";
}

const char *cat_can_mfr_name(int m)
{
    static const char *const M[] = {
        "broadcast", "NI", "Luminary", "DEKA", "CTRE", "REV", "Grapple", "MindSensors", "team use",
        "Kauai Labs", "Copperforge", "Playing With Fusion", "Studica", "Thrifty Bot", "Redux", "AndyMark",
        "Vivid Hosting",
    };
    if (m >= 0 && m < (int)(sizeof M / sizeof M[0])) return M[m];
    return "unknown";
}

const char *cat_can_device_name(int type, int mfr)
{
    if (mfr == 4) { /* CTRE */
        switch (type) {
        case 2: return "Talon FX";
        case 4: return "Pigeon 2";
        case 7: return "CANcoder";
        case 8: return "PDP";
        case 9: return "PCM";
        case 10: return "CANdle";
        }
    }
    if (mfr == 5) { /* REV */
        switch (type) {
        case 2: return "SPARK";
        case 8: return "PDH";
        case 9: return "PH";
        }
    }
    if (mfr == 1 && type == 1) return "robot controller";
    if (mfr == 14) return type == 2 ? "Redux motor" : "Canandcoder";
    if (mfr == 9 && type == 4) return "navX";
    if (mfr == 11) return "PWF sensor";
    if (mfr == 13) return "Thrifty";
    return cat_can_type_name(type);
}

void cat_can_reset(cat_can_bus_t *b, int bitrate)
{
    memset(b, 0, sizeof *b);
    b->bitrate = bitrate > 0 ? bitrate : 1000000;
}

/* Bits on the wire for one frame: arbitration, control, data, CRC, ACK, EOF and IFS, plus worst-ish
 * case stuffing (~20 %). 8-byte extended frames come out near Catalyst App's planning figure of 135. */
static uint32_t frame_bits(bool ext, int len)
{
    uint32_t base = ext ? 67 : 47;
    uint32_t raw = base + 8u * (uint32_t)len;
    return raw + raw / 5;
}

void cat_can_feed(cat_can_bus_t *b, uint32_t id, bool ext, int len, double now)
{
    b->frames++;
    b->bits_window += frame_bits(ext, len);
    if (!b->window_start) b->window_start = now;
    if (!ext) return; /* FRC devices are all extended ids */
    if (id == 0x01011840) {
        /* the robot controller's heartbeat: the rest of the bus follows its enable */
        b->heartbeat = true;
        b->heartbeat_s = now;
    }
    cat_frc_id_t f = cat_can_decode(id);
    if (f.type == 0 && f.mfr == 0) return; /* broadcast */
    for (int i = 0; i < b->n; i++) {
        cat_can_node_t *n = &b->nodes[i];
        if (n->type == f.type && n->mfr == f.mfr && n->device == f.device) {
            n->frames++;
            n->window++;
            n->last_s = now;
            n->quiet = false;
            return;
        }
    }
    if (b->n >= CAT_CAN_MAX_NODES) return;
    cat_can_node_t *n = &b->nodes[b->n++];
    memset(n, 0, sizeof *n);
    n->type = f.type;
    n->mfr = f.mfr;
    n->device = f.device;
    n->frames = n->window = 1;
    n->first_s = n->last_s = now;
}

void cat_can_tick(cat_can_bus_t *b, double now)
{
    double span = now - b->window_start;
    if (b->window_start && span >= 0.5) {
        float load = (float)(b->bits_window / span / b->bitrate);
        b->load = b->load ? b->load + (load - b->load) * 0.5f : load;
        for (int i = 0; i < b->n; i++) {
            cat_can_node_t *n = &b->nodes[i];
            float rate = (float)(n->window / span);
            n->rate_hz = n->rate_hz ? n->rate_hz + (rate - n->rate_hz) * 0.5f : rate;
            n->window = 0;
        }
        b->bits_window = 0;
        b->window_start = now;
    }
    for (int i = 0; i < b->n; i++) {
        cat_can_node_t *n = &b->nodes[i];
        n->quiet = now - n->last_s > 1.0;
        if (n->quiet) n->rate_hz = 0;
    }
    if (b->heartbeat && now - b->heartbeat_s > 1.0) b->heartbeat = false;
}
