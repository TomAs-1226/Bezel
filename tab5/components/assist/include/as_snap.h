/* as_snap — tunable snapshots. Every write the tablet makes to a tunable (from tune, or from the
 * assistant) is preceded by a snapshot of every declared tunable's value, kept on microSD, so any
 * change can be put back exactly as it was. */
#pragma once
#include <stdbool.h>

#include "cat_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNAP_MAX 32

typedef struct {
    int id;
    double t;              /* hal_seconds() */
    char when[20];         /* "14:12:05" from the RTC */
    char reason[96];       /* "before Elevator/kP 0.8 → 1.1 (assist)" */
    int count;             /* tunables in it */
} snap_info_t;

void snap_init(cat_robot_t *robot);
int snap_take(const char *reason);           /* returns its id, or -1 with nothing to snapshot */
bool snap_revert(int id);                    /* writes every value in it back; false if the robot's gone */
int snap_list(snap_info_t *out, int max);    /* newest first */
/* The differences between a snapshot and the robot now, one "key  old → now" per line. */
int snap_diff(int id, char *out, int max);

#ifdef __cplusplus
}
#endif
