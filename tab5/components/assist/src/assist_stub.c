/* Placeholder until the assist core lands: the contract in assist.h, link.h and as_snap.h, inert. */
#include "as_snap.h"
#include "assist.h"
#include "link.h"

#include <stdio.h>
#include <string.h>

void assist_init(cat_robot_t *robot) { (void)robot; }
void assist_configure(const assist_config_t *c) { (void)c; }
bool assist_ready(char *why, size_t n) { snprintf(why, n, "not built yet"); return false; }
bool assist_send(const char *text) { (void)text; return false; }
void assist_stop(void) {}
void assist_reset(void) {}
as_phase_t assist_phase(void) { return AS_PHASE_IDLE; }
uint32_t assist_rev(void) { return 0; }
void assist_lock(void) {}
int assist_count(void) { return 0; }
const as_entry_t *assist_entry(int i) { (void)i; return NULL; }
void assist_unlock(void) {}
bool assist_confirm_pending(as_confirm_t *out) { memset(out, 0, sizeof *out); return false; }
void assist_confirm(bool approve) { (void)approve; }
void assist_usage(as_usage_t *out) { memset(out, 0, sizeof *out); }
int assist_suggestions(const char **out, int max) { (void)out; (void)max; return 0; }

void link_init(void) {}
void link_configure(const char *url, const char *token) { (void)url; (void)token; }
void link_status(link_status_t *out) { memset(out, 0, sizeof *out); }
int link_outbox_count(void) { return 0; }
int link_inbox(link_item_t *out, int max) { (void)out; (void)max; return 0; }
int link_patches(link_item_t *out, int max) { (void)out; (void)max; return 0; }
int link_get(const char *path, char *out, int max) { (void)path; if (max) out[0] = 0; return -1; }
int link_post(const char *path, const char *json, char *out, int max) { (void)path; (void)json; if (max) out[0] = 0; return -1; }
bool link_post_queued(const char *path, const char *json) { (void)path; (void)json; return false; }
bool link_upload(const char *sd_path, const char *name) { (void)sd_path; (void)name; return false; }
bool link_messages_endpoint(char *url, size_t n, char *headers, size_t hn) { (void)url; (void)n; (void)headers; (void)hn; return false; }

void snap_init(cat_robot_t *robot) { (void)robot; }
int snap_take(const char *reason) { (void)reason; return -1; }
bool snap_revert(int id) { (void)id; return false; }
int snap_list(snap_info_t *out, int max) { (void)out; (void)max; return 0; }
int snap_diff(int id, char *out, int max) { (void)id; if (max) out[0] = 0; return 0; }
