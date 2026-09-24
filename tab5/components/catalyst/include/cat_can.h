/* cat_can — reading a robot's CAN bus directly, without the robot program.
 *
 * FRC devices use 29-bit arbitration ids laid out as device type [28:24], manufacturer [23:16],
 * API class [15:10], API index [9:6] and device number [5:0] (the FRC CAN specification). The CAN tap
 * hears every frame on the bus through a listen-only transceiver and builds a live roster from them:
 * who is talking, how often, and who has gone quiet. It works when the robot code is dead, the radio
 * is off or NetworkTables is down — which is when a pit tech most needs it. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t type, mfr, api_class, api_index, device;
} cat_frc_id_t;

cat_frc_id_t cat_can_decode(uint32_t id);
const char *cat_can_type_name(int type);
const char *cat_can_mfr_name(int mfr);
/* A friendlier guess for common devices: "Talon FX", "CANcoder", "Pigeon 2", "SPARK", "PDH"... */
const char *cat_can_device_name(int type, int mfr);

#define CAT_CAN_MAX_NODES 96

typedef struct {
    uint8_t type, mfr, device;
    uint32_t frames;
    double first_s, last_s;
    float rate_hz;          /* frames per second, smoothed */
    uint32_t window;        /* frames in the current second */
    bool quiet;             /* heard before, not for a second */
} cat_can_node_t;

typedef struct {
    int n;
    cat_can_node_t nodes[CAT_CAN_MAX_NODES];
    uint32_t frames, bits_window;
    double window_start;
    float load;             /* estimated bus load, 0..1 of the bitrate */
    int bitrate;
    bool heartbeat;         /* the robot controller's heartbeat is on the bus */
    double heartbeat_s;
} cat_can_bus_t;

void cat_can_reset(cat_can_bus_t *b, int bitrate);
/* One received frame. `now` in seconds. */
void cat_can_feed(cat_can_bus_t *b, uint32_t id, bool ext, int len, double now);
/* Rolls rates and quiet flags; call a few times a second. */
void cat_can_tick(cat_can_bus_t *b, double now);
