/* nt4 — a NetworkTables 4 client for a handheld dashboard.
 *
 * Mirrors Catalyst Console's src-tauri/src/nt4.rs: connect to ws://<robot>:5810/nt/<name> offering
 * "v4.1.networktables.first.wpi.edu" then "networktables.first.wpi.edu", subscribe with a prefix, turn
 * announcements into an id → name/type table, decode msgpack value frames, run the timestamp handshake
 * once a second (keepalive + RTT + server clock offset), and publish on demand with a per-session pubuid.
 * Struct topics wpimath publishes (Pose2d, SwerveModuleState, ...) decode to runs of doubles exactly as
 * Console does, and 2027's struct:ControlWord maps onto the 2026 FMSControlData bits.
 *
 * One task owns the socket (nt4_run); every other task reads through the thread-safe getters, which copy
 * out under a mutex. Values carry a sequence number so a UI can redraw only what changed. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NT4_BOOLEAN = 0, NT4_DOUBLE = 1, NT4_INT = 2, NT4_FLOAT = 3, NT4_STRING = 4, NT4_RAW = 5,
    NT4_BOOLEAN_ARRAY = 16, NT4_DOUBLE_ARRAY = 17, NT4_INT_ARRAY = 18, NT4_FLOAT_ARRAY = 19,
    NT4_STRING_ARRAY = 20,
} nt4_type_t;

typedef enum { NT4_IDLE, NT4_CONNECTING, NT4_CONNECTED } nt4_state_t;

typedef struct {
    nt4_state_t state;
    char address[64];       /* the address currently connected (or being tried) */
    char protocol[48];      /* the subprotocol the server chose */
    int64_t rtt_us;         /* last timestamp-handshake round trip */
    int64_t offset_us;      /* server clock minus ours */
    uint32_t topics;        /* announced topics */
    uint32_t updates;       /* values received since connect */
    uint64_t rx_bytes, tx_bytes;
    uint32_t reconnects;
    int64_t connected_since_us;
    char last_error[96];
} nt4_status_t;

typedef struct {
    const char *client_name;    /* appears in the server's client list: /nt/<client_name> */
    double period_s;            /* subscription periodic rate; Console uses 0.02, a handheld 0.05 */
    const char *const *prefixes;/* subscription prefixes, NULL-terminated; NULL means {"/"} */
    int max_topics;             /* table capacity; 0 → 2048 */
} nt4_config_t;

typedef struct nt4_client nt4_client_t;

nt4_client_t *nt4_create(const nt4_config_t *cfg);
/* Candidate addresses, tried in order each round (a team's 10.TE.AM.2, the mDNS name, USB 172.22.11.2,
 * the simulator's localhost...). Safe to call from any task; takes effect on the next connect. */
void nt4_set_addresses(nt4_client_t *c, const char *const *addrs, int n);
/* Blocks, owning the connection, until nt4_stop(). Run it on its own task. */
void nt4_run(nt4_client_t *c);
void nt4_stop(nt4_client_t *c);
/* Drops the current connection so the next round starts over (address change, link change). */
void nt4_reconnect(nt4_client_t *c);

void nt4_status(nt4_client_t *c, nt4_status_t *out);
/* Microseconds on the server's clock, if the handshake has run; our monotonic clock otherwise. */
int64_t nt4_server_time_us(nt4_client_t *c);
/* Monotonic microseconds, the clock every nt4 timestamp is measured against locally. */
int64_t nt4_now_us(void);
/* Bumps whenever any value or announcement changes: a cheap "anything new?" for a render loop. */
uint32_t nt4_generation(nt4_client_t *c);

/* ---- reading (copies out under the lock) ---- */

/* Any scalar: boolean, int, float, double, or a single-double struct (Rotation2d, ControlWord). */
bool nt4_get_number(nt4_client_t *c, const char *name, double *out);
bool nt4_get_bool(nt4_client_t *c, const char *name, bool *out);
/* Strings and JSON strings. Returns false if absent or not a string. */
bool nt4_get_string(nt4_client_t *c, const char *name, char *buf, size_t n);
/* Numeric arrays and decoded structs (Pose2d → x,y,rad; SwerveModuleState[] → speed,rad pairs...).
 * Boolean arrays read as 0/1. Returns the element count copied (≤ max), or -1 if absent. */
int nt4_get_numbers(nt4_client_t *c, const char *name, double *out, int max);
/* String arrays, packed into `buf` with `items` pointing into it. Returns the count, or -1. */
int nt4_get_strings(nt4_client_t *c, const char *name, char *buf, size_t buflen, const char **items, int max);

/* Per-topic metadata: the NT type string ("double", "struct:Pose2d", "json"...), the value's
 * sequence number (0 if never received) and its age on our clock. */
bool nt4_info(nt4_client_t *c, const char *name, char *type, size_t type_len, uint32_t *seq, int64_t *age_us);

/* Visits every announced topic whose name starts with `prefix`, under the lock: keep the callback
 * short and do not call back into nt4. Returns the number visited. */
typedef void (*nt4_visit_fn)(const char *name, const char *type, void *user);
int nt4_list(nt4_client_t *c, const char *prefix, nt4_visit_fn fn, void *user);

/* ---- writing (queued; the network task publishes and sends) ---- */

void nt4_set_double(nt4_client_t *c, const char *name, double v);
void nt4_set_int(nt4_client_t *c, const char *name, int64_t v);
void nt4_set_bool(nt4_client_t *c, const char *name, bool v);
void nt4_set_string(nt4_client_t *c, const char *name, const char *v);

#ifdef __cplusplus
}
#endif
