/* Catalyst Tab on a laptop: the firmware's UI, renderer and NT client with the simulated HAL.
 *
 *   catalyst_tab_sim [--robot HOST] [--team N] [--script FILE] [--out DIR] [--light] [--calm]
 *
 * Script commands, one per line (# comments):
 *   wait S                    let S seconds of real time pass, rendering at 60 Hz
 *   tap X Y                   press and release
 *   drag X1 Y1 X2 Y2 S        a finger moving over S seconds, then lifting
 *   fling X1 Y1 X2 Y2 S       the same, lifting while still moving
 *   shot NAME                 save the composed frame as OUT/NAME.png
 *   mode dark|light           tone
 *   calm on|off               reduced motion + solid glass
 *   tilt X Y                  lean the simulated IMU (radians)
 *   ask TEXT                  put a question to the assistant, as if typed
 *   stats                     print compositor timing
 *   trace NAME | trace end    cost every frame between the two for the ESP32-P4 (see bz_ui.c's model)
 *                             and print the distribution; frames over 16.7 ms miss 60 Hz
 * Without a script it renders for 3 s and saves OUT/frame.png. */
#include "bz_theme.h"
#include "bz_ui.h"
#include "hal.h"
#include "nt4.h"
#include "ui.h"
#include "cat_model.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void sim_touch(bool pressed, int x, int y);
void sim_tilt(float x, float y);
uint16_t *sim_frame(void);
int png_write_rgb565(const char *path, const uint16_t *px, int w, int h);

static const char *g_out = "shots";
static double g_next;

static struct {
    bool on;
    char name[64];
    float ms[4096];
    bz_ui_perf_t worst;
    int n;
    uint32_t last_frame;
} T;

static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void trace_end(void)
{
    if (!T.on) return;
    T.on = false;
    if (!T.n) { printf("trace %s: no frames\n", T.name); return; }
    float sum = 0;
    int over = 0;
    for (int i = 0; i < T.n; i++) { sum += T.ms[i]; over += T.ms[i] > 16.7f; }
    qsort(T.ms, (size_t)T.n, sizeof T.ms[0], cmp_f);
    const bz_comp_stats_t *c = &T.worst.comp;
    printf("trace %-14s %3d frames  P4 model: avg %5.1f ms  p95 %5.1f  max %5.1f  over 16.7: %d\n", T.name, T.n,
           sum / T.n, T.ms[(int)(T.n * 0.95f)], T.ms[T.n - 1], over);
    printf("    worst: lvgl %u px, composed %u, direct %u, glass %u (fast %u), lut %u/%u ring, blur %u, base-cpu %u, "
           "mix %u, ink %u, backdrop %u, cells %u\n",
           T.worst.lvgl_px, c->composed_px, c->direct_px, c->glass_px, c->glass_fast_px, c->lut_px, c->lut_ring_px,
           c->blur_src_px, c->base_cpu_px, c->mix_px, c->ink_px, c->backdrop_build_px, c->cells);
}

static void frame(void)
{
    double now = hal_seconds();
    if (g_next == 0) g_next = now;
    g_next += 1.0 / 60;
    bz_ui_frame(now);
    if (T.on) {
        bz_ui_perf_t p;
        bz_ui_perf(&p);
        if (p.frames != T.last_frame && T.n < 4096) {
            T.last_frame = p.frames;
            if (!T.n || p.model_ms > T.worst.model_ms) T.worst = p;
            T.ms[T.n++] = p.model_ms;
            if (getenv("SIM_TRACE_FRAMES")) {
                const bz_comp_stats_t *c = &p.comp;
                printf("  %5.1f ms  lvgl %7u  comp %7u dir %7u glass %7u (flat %6u edge %6u shadow %6u) lut %6u/%6u "
                       "blur %7u mix %6u ink %6u cells %u\n",
                       p.model_ms, p.lvgl_px, c->composed_px, c->direct_px, c->glass_px, c->glass_flat_px, c->glass_edge_px,
                       c->glass_shadow_px, c->lut_px, c->lut_ring_px, c->blur_src_px, c->mix_px, c->ink_px, c->cells);
            }
        }
    }
    double sleep = g_next - hal_seconds();
    if (sleep > 0) usleep((useconds_t)(sleep * 1e6));
    else g_next = hal_seconds();
}

static void run_for(double s)
{
    double end = hal_seconds() + s;
    while (hal_seconds() < end) frame();
}

static void finger(int x1, int y1, int x2, int y2, double s, bool fling)
{
    int steps = (int)(s * 60);
    if (steps < 2) steps = 2;
    for (int i = 0; i <= steps; i++) {
        float k = (float)i / steps;
        if (!fling) k = k * k * (3 - 2 * k); /* a drag slows into place before lifting */
        sim_touch(true, (int)(x1 + (x2 - x1) * k), (int)(y1 + (y2 - y1) * k));
        frame();
    }
    sim_touch(false, x2, y2);
    frame();
}

static void shot(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", g_out, name);
    png_write_rgb565(path, sim_frame(), HAL_W, HAL_H);
    printf("shot %s\n", path);
}

static void *nt_thread(void *arg)
{
    nt4_run(arg);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *robot = "127.0.0.1", *script = NULL;
    int team = 5805;
    bool light = false, calm = false, probe = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--robot") && i + 1 < argc) robot = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) g_out = argv[++i];
        else if (!strcmp(argv[i], "--team") && i + 1 < argc) team = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--light")) light = true;
        else if (!strcmp(argv[i], "--calm")) calm = true;
        else if (!strcmp(argv[i], "--probe")) probe = true;
    }
    mkdir(g_out, 0755);

    lv_init();
    hal_init();
    hal_display_t d;
    hal_display(&d);
    bz_ui_config_t cfg = {
        .w = HAL_W, .h = HAL_H, .content = d.content, .ink = d.ink, .out = d.out,
        .present = hal_present, .read_touch = hal_touch, .ops = d.ops,
    };
    bz_ui_init(&cfg);
    bz_theme_init();
    bz_ui_set_mode(!light, calm);

    static const char *const prefixes[] = { "/Catalyst/", "/FMSInfo/", "/Auto Selector/", "/SmartDashboard/",
                                             "/limelight", "/PathPlanner/", NULL };
    nt4_config_t ncfg = { .client_name = "catalyst-tab", .period_s = 0.05, .prefixes = prefixes };
    nt4_client_t *nt = nt4_create(&ncfg);
    ui_config_t ucfg = { .nt = nt, .team = team, .sim_address = robot };
    ui_init(&ucfg);
    pthread_t th;
    pthread_create(&th, NULL, nt_thread, nt);

    if (probe) {
        /* connect, read the model, print what the tablet would show */
        static cat_robot_t r;
        cat_model_init(nt);
        char addrs[8][64];
        const char *ptrs[8];
        int n = cat_addresses(team, robot, addrs, 1);
        for (int i = 0; i < n; i++) ptrs[i] = addrs[i];
        nt4_set_addresses(nt, ptrs, n);
        run_for(2.5);
        cat_model_update(&r);
        printf("connected %d to %s, rtt %.1f ms, %u topics\n", r.connected, r.address, r.rtt_ms, r.topics);
        printf("%s (%d) catalyst %s, %s, mode %s, battery %.2f V, loop %.1f ms\n", r.name, r.team, r.catalyst_version,
               r.line2 ? "2.x" : "1.x", cat_mode_name(&r), r.battery_v, r.loop_avg_ms);
        printf("devices %d, roster %d/%d, alerts %d (E%d W%d I%d), mechs %d, tunables %d, autos %d sel '%s'\n",
               r.ndevices, r.motors_connected, r.motors_expected, r.nalerts, r.n_errors, r.n_warnings, r.n_infos,
               r.nmechs, r.ntunables, r.nautos, r.auto_selected);
        printf("pose %d (%.2f, %.2f, %.2f) modules %d, cams %d, channels %d live %d, findings %d, can %d [%.2f]\n",
               r.have_pose, r.pose_x, r.pose_y, r.pose_rad, r.nmodules, r.ncameras, r.nchannels, r.have_pd_live,
               r.nfindings, r.ncan_util, r.can_util[0]);
        for (int i = 0; i < r.nmechs; i++) printf("  mech %s kind %d pos %.2f sp %.2f %s\n", r.mechs[i].name, r.mechs[i].kind, r.mechs[i].pos, r.mechs[i].setpoint, r.mechs[i].state);
        for (int i = 0; i < r.nalerts; i++) printf("  alert %d [%s] %s\n", r.alerts[i].sev, r.alerts[i].source, r.alerts[i].text);
        cat_set_tunable(&r, 5, 70);
        run_for(0.5);
        cat_model_update(&r);
        printf("after write: shooter target %.1f\n", r.tunables[5].value);
        nt4_stop(nt);
        return 0;
    }
    if (!script) {
        run_for(3);
        shot("frame");
        return 0;
    }
    FILE *f = fopen(script, "r");
    if (!f) {
        perror(script);
        return 1;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char cmd[32] = "", arg[200] = "";
        double a, b, c, e, s;
        if (line[0] == '#' || sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "wait") && sscanf(line, "%*s %lf", &a) == 1) run_for(a);
        else if (!strcmp(cmd, "tap") && sscanf(line, "%*s %lf %lf", &a, &b) == 2) {
            sim_touch(true, (int)a, (int)b);
            frame(); frame(); frame();
            sim_touch(false, (int)a, (int)b);
            frame();
        } else if ((!strcmp(cmd, "drag") || !strcmp(cmd, "fling")) &&
                   sscanf(line, "%*s %lf %lf %lf %lf %lf", &a, &b, &c, &e, &s) == 5) {
            finger((int)a, (int)b, (int)c, (int)e, s, cmd[0] == 'f');
        } else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %199s", arg) == 1) shot(arg);
        else if (!strcmp(cmd, "mode") && sscanf(line, "%*s %199s", arg) == 1) bz_ui_set_mode(strcmp(arg, "light") != 0, bz_ui_calm());
        else if (!strcmp(cmd, "calm") && sscanf(line, "%*s %199s", arg) == 1) bz_ui_set_mode(bz_ui_dark(), !strcmp(arg, "on"));
        else if (!strcmp(cmd, "tilt") && sscanf(line, "%*s %lf %lf", &a, &b) == 2) sim_tilt((float)a, (float)b);
        else if (!strcmp(cmd, "trace") && sscanf(line, "%*s %63s", arg) == 1) {
            if (!strcmp(arg, "end")) trace_end();
            else {
                bz_ui_perf_t p;
                bz_ui_perf(&p);
                memset(&T, 0, sizeof T);
                T.on = true;
                T.last_frame = p.frames;
                snprintf(T.name, sizeof T.name, "%s", arg);
            }
        } else if (!strcmp(cmd, "dump") && sscanf(line, "%*s %199s", arg) == 1) {
            const uint16_t *ui_debug_strip(int *stride, int *w, int *h);
            int st, w, h;
            const uint16_t *px = ui_debug_strip(&st, &w, &h);
            char path[512];
            snprintf(path, sizeof path, "%s/%s.png", g_out, arg);
            if (!strcmp(arg, "base")) {
                void bz_comp_debug_base(bz_comp_t *c, uint16_t *dst);
                static uint16_t buf[HAL_W * HAL_H];
                bz_comp_debug_base(bz_ui_comp(), buf);
                png_write_rgb565(path, buf, HAL_W, HAL_H);
            } else if (!strcmp(arg, "glass")) {
                void bz_comp_debug_dump(bz_comp_t *c, const char *dir);
                bz_comp_debug_dump(bz_ui_comp(), g_out);
            } else if (px) png_write_rgb565(path, px, w, h);
        } else if (!strcmp(cmd, "ask")) {
            bool assist_send(const char *text);
            const char *q = line + 3;
            while (*q == ' ') q++;
            char buf[512];
            snprintf(buf, sizeof buf, "%s", q);
            buf[strcspn(buf, "\n")] = 0;
            if (!assist_send(buf)) fprintf(stderr, "script: the assistant is busy or not set up\n");
        } else if (!strcmp(cmd, "stats")) {
            bz_comp_stats_t st;
            bz_comp_stats(bz_ui_comp(), &st);
            printf("compose %u us, %u px, glass %u px, blur rebuilds %u, lut rebuilds %u\n", st.compose_us,
                   st.composed_px, st.glass_px, st.blur_rebuilds, st.lut_rebuilds);
        } else fprintf(stderr, "script: can't read: %s", line);
    }
    fclose(f);
    nt4_stop(nt);
    return 0;
}
