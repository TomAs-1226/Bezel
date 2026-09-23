/* assist_e2e — the assistant end to end, without the UI: the simulator's HAL, NetworkTables to a robot
 * (tools/fake_robot.py), and the Messages API at tools/fake_claude.py (or the real one).
 *
 *   assist_e2e [--robot HOST] [--claude URL] [--key KEY] [--model ID] [--link URL [--link-token T]] [--route direct|link]
 *              [--wait-outbox S] "question" ...
 *
 * It feeds the robot model at 10 Hz as the UI would, asks each question in turn, answers confirmation
 * cards (approve; a question starting with "!decline " declines them), and prints the transcript, the
 * snapshots and the usage. Run from tab5/build: the simulated microSD is ./sim_sd. */
#include "as_snap.h"
#include "assist.h"
#include "cat_model.h"
#include "hal.h"
#include "link.h"
#include "nt4.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static cat_robot_t R;

static void *nt_thread(void *arg)
{
    nt4_run(arg);
    return NULL;
}

static void tick(void)
{
    cat_model_update(&R);
    assist_feed(&R);
    usleep(100000);
}

static const char *kind_name(as_entry_kind_t k)
{
    static const char *n[] = { "you", "claude", "thinking", "tool", "note", "error" };
    return k <= AS_E_ERROR ? n[k] : "?";
}

static const char *state_name(as_tool_state_t s)
{
    static const char *n[] = { "running", "ok", "failed", "declined", "waiting" };
    return s <= AS_TOOL_WAITING ? n[s] : "?";
}

static int print_from(int from)
{
    assist_lock();
    int n = assist_count();
    for (int i = from; i < n; i++) {
        const as_entry_t *e = assist_entry(i);
        if (e->kind == AS_E_TOOL) printf("  [tool %s · %s] %s\n", e->tool, state_name(e->tool_state), e->text);
        else printf("  [%s] %s\n", kind_name(e->kind), e->text);
    }
    assist_unlock();
    return n;
}

int main(int argc, char **argv)
{
    const char *robot = "127.0.0.1", *claude = "http://127.0.0.1:8787", *key = "test-key", *model = "";
    const char *link = NULL, *token = "test-token", *route = "direct";
    double wait_outbox = 0;
    const char *questions[16];
    int nq = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--robot") && i + 1 < argc) robot = argv[++i];
        else if (!strcmp(argv[i], "--claude") && i + 1 < argc) claude = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--link") && i + 1 < argc) link = argv[++i];
        else if (!strcmp(argv[i], "--route") && i + 1 < argc) route = argv[++i];
        else if (!strcmp(argv[i], "--link-token") && i + 1 < argc) token = argv[++i];
        else if (!strcmp(argv[i], "--wait-outbox") && i + 1 < argc) wait_outbox = atof(argv[++i]);
        else if (nq < 16) questions[nq++] = argv[i];
    }

    hal_init();
    static const char *const prefixes[] = { "/Catalyst/", "/FMSInfo/", "/Auto Selector/", "/SmartDashboard/",
                                             "/limelight", "/PathPlanner/", NULL };
    nt4_config_t ncfg = { .client_name = "catalyst-tab", .period_s = 0.05, .prefixes = prefixes };
    nt4_client_t *nt = nt4_create(&ncfg);
    cat_model_init(nt);
    const char *addrs[] = { robot };
    nt4_set_addresses(nt, addrs, 1);
    pthread_t th;
    pthread_create(&th, NULL, nt_thread, nt);

    assist_init();
    assist_config_t cfg = { .route = !strcmp(route, "link") ? AS_ROUTE_LINK : AS_ROUTE_DIRECT };
    snprintf(cfg.api_key, sizeof cfg.api_key, "%s", key);
    snprintf(cfg.model, sizeof cfg.model, "%s", model);
    assist_configure(&cfg);
    assist_set_base_url(claude);
    if (link) link_configure(link, token);

    for (int i = 0; i < 50 && !R.connected; i++) tick();
    for (int i = 0; i < 10; i++) tick(); /* let the announce burst and values land */
    printf("robot: connected %d at %s · %s · %d tunables · battery %.2f V\n", R.connected, R.address, cat_mode_name(&R),
           R.ntunables, R.battery_v);
    const char *sugg[5];
    int ns = assist_suggestions(sugg, 5);
    printf("suggestions:");
    for (int i = 0; i < ns; i++) printf(" \"%s\"", sugg[i]);
    printf("\n");
    char why[160];
    bool ready = assist_ready(why, sizeof why);
    printf("ready: %s%s%s\n", ready ? "yes" : "no", ready ? "" : " — ", ready ? "" : why);

    int shown = 0, failures = 0;
    for (int q = 0; q < nq; q++) {
        bool decline = !strncmp(questions[q], "!decline ", 9);
        const char *text = decline ? questions[q] + 9 : questions[q];
        printf("\n> %s%s\n", text, decline ? "   (cards will be declined)" : "");
        if (!assist_send(text)) {
            printf("  send refused\n");
            failures++;
            continue;
        }
        double end = hal_seconds() + 180, last_card = -1;
        while (hal_seconds() < end) {
            tick();
            as_confirm_t c;
            if (assist_confirm_pending(&c) && c.deadline != last_card) {
                last_card = c.deadline;
                printf("  ┌ card: %s (%s)\n", c.title, c.kind);
                for (char *line = strtok(c.detail, "\n"); line; line = strtok(NULL, "\n")) printf("  │ %s\n", line);
                printf("  └ %s\n", decline ? "declined" : "approved");
                assist_confirm(!decline);
            }
            as_phase_t p = assist_phase();
            if (p == AS_PHASE_IDLE || p == AS_PHASE_ERROR) break;
        }
        if (assist_phase() == AS_PHASE_ERROR) failures++;
        shown = print_from(shown);
    }

    snap_info_t snaps[SNAP_MAX];
    int n = snap_list(snaps, SNAP_MAX);
    printf("\nsnapshots: %d\n", n);
    for (int i = 0; i < n && i < 6; i++) {
        char diff[512];
        int d = snap_diff(&R, snaps[i].id, diff, sizeof diff);
        printf("  #%d %s %s (%d tunables) · differs now: %d%s%s\n", snaps[i].id, snaps[i].when, snaps[i].reason,
               snaps[i].count, d, d > 0 ? " · " : "", d > 0 ? diff : "");
    }
    for (int i = 0; i < R.ntunables; i++)
        if (strstr(R.tunables[i].key, "Shooter")) printf("robot now: %s = %g\n", R.tunables[i].key, R.tunables[i].value);

    if (wait_outbox > 0) {
        double end = hal_seconds() + wait_outbox;
        int last = -1;
        while (hal_seconds() < end) {
            int k = link_outbox_count();
            if (k != last) printf("outbox: %d waiting\n", k);
            last = k;
            if (k == 0) break;
            usleep(200000);
        }
    }
    as_usage_t u;
    assist_usage(&u);
    printf("\nusage: %d in, %d out, model %s%s\n", u.input_tokens, u.output_tokens, u.model, u.fell_back ? ", fell back" : "");
    nt4_stop(nt);
    return failures ? 1 : 0;
}
