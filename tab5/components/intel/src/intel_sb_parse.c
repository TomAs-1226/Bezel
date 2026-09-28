/* Statbotics v3's documents: a team's season, its event, and its matches there. Pure: host-tested.
 *
 * v3's OpenAPI names no response schema, so the fields are read by the names the site and the statbotics
 * Python package use (epa.total_points.mean, epa.ranks.total.{rank,percentile,team_count}, record.{wins,
 * losses,ties}, record.qual.{...}; a match's alliances.{red,blue}.team_keys, pred.red_win_prob, result.
 * {red_score,blue_score}), with the older flat names tried after them. Absent is NAN or 0, never a guess. */
#include "intel.h"
#include "intel_priv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int path(const ix_json_t *j, int obj, const char *p)
{
    return obj < 0 ? -1 : jl_path(&j->d, obj, p);
}

static double pnum(const ix_json_t *j, int obj, const char *p, double fb)
{
    int t = path(j, obj, p);
    if (t < 0 || j->d.t[t].type == JL_NULL) return fb;
    return ix_tok_num(j, t, fb);
}

/* the first path that holds a number */
static double pnum_any(const ix_json_t *j, int obj, const char *const *ps, double fb)
{
    for (; *ps; ps++) {
        double v = pnum(j, obj, *ps, NAN);
        if (!isnan(v)) return v;
    }
    return fb;
}

static const char *const P_EPA[] = { "epa.total_points.mean", "epa.total_points", "epa.mean", "epa_end", "epa", NULL };
static const char *const P_SD[] = { "epa.total_points.sd", "epa.sd", NULL };
static const char *const P_RANK[] = { "epa.ranks.total.rank", "epa_rank", "total_epa_rank", NULL };
static const char *const P_PCT[] = { "epa.ranks.total.percentile", "epa_percentile", "total_epa_percentile", NULL };
static const char *const P_COUNT[] = { "epa.ranks.total.team_count", "total_team_count", NULL };
static const char *const P_SRANK[] = { "epa.ranks.state.rank", "state_epa_rank", NULL };
static const char *const P_SCOUNT[] = { "epa.ranks.state.team_count", "state_team_count", NULL };
static const char *const P_W[] = { "record.wins", "record.season.wins", "record.total.wins", "wins", NULL };
static const char *const P_L[] = { "record.losses", "record.season.losses", "record.total.losses", "losses", NULL };
static const char *const P_T[] = { "record.ties", "record.season.ties", "record.total.ties", "ties", NULL };

bool sb_parse_team_year(const char *js, size_t len, sb_state_t *out)
{
    ix_json_t j;
    if (!ix_parse(&j, js, len)) return false;
    bool ok = j.d.t[0].type == JL_OBJ && (ix_get(&j, 0, "epa") >= 0 || ix_get(&j, 0, "team") >= 0);
    if (ok) {
        out->epa = pnum_any(&j, 0, P_EPA, NAN);
        out->epa_sd = pnum_any(&j, 0, P_SD, NAN);
        out->rank = (int)pnum_any(&j, 0, P_RANK, 0);
        out->percentile = pnum_any(&j, 0, P_PCT, NAN);
        out->team_count = (int)pnum_any(&j, 0, P_COUNT, 0);
        out->state_rank = (int)pnum_any(&j, 0, P_SRANK, 0);
        out->state_count = (int)pnum_any(&j, 0, P_SCOUNT, 0);
        out->wins = (int)pnum_any(&j, 0, P_W, 0);
        out->losses = (int)pnum_any(&j, 0, P_L, 0);
        out->ties = (int)pnum_any(&j, 0, P_T, 0);
        out->have_year = !isnan(out->epa);
    }
    ix_free(&j);
    return ok;
}

static const char *const P_EW[] = { "record.qual.wins", "record.wins", "record.total.wins", "wins", NULL };
static const char *const P_EL[] = { "record.qual.losses", "record.losses", "record.total.losses", "losses", NULL };
static const char *const P_ET[] = { "record.qual.ties", "record.ties", "record.total.ties", "ties", NULL };
static const char *const P_ER[] = { "record.qual.rank", "rank", "qual_rank", NULL };
static const char *const P_EN[] = { "record.qual.num_teams", "num_teams", NULL };

bool sb_parse_team_event(const char *js, size_t len, sb_state_t *out)
{
    ix_json_t j;
    if (!ix_parse(&j, js, len)) return false;
    bool ok = j.d.t[0].type == JL_OBJ && (ix_get(&j, 0, "epa") >= 0 || ix_get(&j, 0, "event") >= 0);
    if (ok) {
        out->event_epa = pnum_any(&j, 0, P_EPA, NAN);
        out->ev_wins = (int)pnum_any(&j, 0, P_EW, 0);
        out->ev_losses = (int)pnum_any(&j, 0, P_EL, 0);
        out->ev_ties = (int)pnum_any(&j, 0, P_ET, 0);
        out->event_rank = (int)pnum_any(&j, 0, P_ER, 0);
        out->event_teams = (int)pnum_any(&j, 0, P_EN, 0);
        char ev[20];
        if (ix_str(&j, 0, "event", ev, sizeof ev)) snprintf(out->event_key, sizeof out->event_key, "%s", ev);
        out->have_event = !isnan(out->event_epa);
    }
    ix_free(&j);
    return ok;
}

static void label_of(const ix_json_t *j, int m, char *out, size_t n)
{
    char cl[8] = "", key[40];
    ix_str(j, m, "comp_level", cl, sizeof cl);
    int set = (int)ix_num(j, m, "set_number", 1), num = (int)ix_num(j, m, "match_number", 0);
    if (!cl[0] && ix_str(j, m, "key", key, sizeof key)) {
        /* "2026casj_qm34", "2026casj_sf5m1", "2026casj_f1m2" */
        const char *u = strchr(key, '_');
        if (u) {
            u++;
            int k = 0;
            while (u[k] >= 'a' && u[k] <= 'z' && k < 3) cl[k] = u[k], k++;
            cl[k] = 0;
            if (!strcmp(cl, "qm")) sscanf(u + k, "%d", &num);
            else if (sscanf(u + k, "%dm%d", &set, &num) != 2) num = 0;
        }
    }
    out[0] = 0;
    if (!strcmp(cl, "qm")) snprintf(out, n, "Q%d", num);
    else if (!strcmp(cl, "f")) snprintf(out, n, "F%d", num);
    else if (!strcmp(cl, "sf")) {
        if (num <= 1) snprintf(out, n, "SF%d", set);
        else snprintf(out, n, "SF%d-%d", set, num);
    } else if (!strcmp(cl, "qf")) {
        if (num <= 1) snprintf(out, n, "QF%d", set);
        else snprintf(out, n, "QF%d-%d", set, num);
    } else if (!strcmp(cl, "ef")) snprintf(out, n, "E%d", set);
}

static int side_of(const ix_json_t *j, int m, const char *side, int team)
{
    int al = ix_get(j, m, "alliances");
    int s = al >= 0 ? ix_get(j, al, side) : -1;
    int keys = s >= 0 ? ix_get(j, s, "team_keys") : -1;
    if (keys < 0 && s >= 0 && j->d.t[s].type == JL_ARR) keys = s;
    if (keys < 0) {
        char k[16];
        snprintf(k, sizeof k, "%s_teams", side);
        keys = ix_get(j, m, k);
    }
    if (keys < 0 || j->d.t[keys].type != JL_ARR) return 0;
    for (int i = 0; i < j->d.t[keys].size; i++)
        if (ix_team(j, jl_at(&j->d, keys, i)) == team) return 1;
    return 0;
}

bool sb_parse_matches(const char *js, size_t len, int team, sb_state_t *out)
{
    ix_json_t j;
    if (!ix_parse(&j, js, len)) return false;
    if (j.d.t[0].type != JL_ARR) {
        ix_free(&j);
        return false;
    }
    int n = 0;
    for (int i = 0; i < j.d.t[0].size && n < SB_MATCHES; i++) {
        int m = jl_at(&j.d, 0, i);
        if (m < 0 || j.d.t[m].type != JL_OBJ) continue;
        int ours = side_of(&j, m, "red", team) ? 1 : side_of(&j, m, "blue", team) ? 2 : 0;
        if (!ours) continue;
        sb_match_t *x = &out->matches[n];
        memset(x, 0, sizeof *x);
        label_of(&j, m, x->label, sizeof x->label);
        if (!x->label[0]) continue;
        n++;
        x->ours = ours;
        time_t t = ix_time(&j, ix_get(&j, m, "predicted_time"));
        x->time = t ? t : ix_time(&j, ix_get(&j, m, "time"));
        double rw = pnum(&j, m, "pred.red_win_prob", NAN);
        if (isnan(rw)) rw = pnum(&j, m, "red_win_prob", NAN);
        x->win = isnan(rw) ? NAN : ours == 1 ? rw : 1 - rw;
        x->red_pred = pnum(&j, m, "pred.red_score", NAN);
        x->blue_pred = pnum(&j, m, "pred.blue_score", NAN);
        double rs = pnum(&j, m, "result.red_score", NAN), bs = pnum(&j, m, "result.blue_score", NAN);
        if (isnan(rs)) rs = pnum(&j, m, "red_score", NAN);
        if (isnan(bs)) bs = pnum(&j, m, "blue_score", NAN);
        char status[16] = "";
        ix_str(&j, m, "status", status, sizeof status);
        x->played = (!isnan(rs) && !isnan(bs) && rs >= 0 && bs >= 0) || ix_has(status, "complete");
        x->red_score = isnan(rs) ? -1 : (int)rs;
        x->blue_score = isnan(bs) ? -1 : (int)bs;
    }
    out->nmatches = n;
    ix_free(&j);
    return true;
}
