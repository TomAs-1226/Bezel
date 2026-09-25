/* The weather from Open-Meteo (no key, no account): the temperature now, today's high and low, the WMO code,
 * and the next 24 hours and 7 days for the weather app, in one request every 20 minutes while home mode (or
 * the weather app) shows it. A place typed as a name is resolved once through
 * Open-Meteo's geocoder; "lat,lon" is used as it is. */
#include "home_priv.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVERY_S (20 * 60)
#define RETRY_S 90
#define RESP_MAX 16384      /* the forecast's JSON is ~4 KB */

typedef struct {
    char where[64];
    bool fahrenheit;
    unsigned cfg_gen, done_gen;
    bool have_ll;
    double lat, lon;
    home_weather_t st;
    home_forecast_t fc;
    double next;
} wx_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static wx_t *X;

void home_weather_config(const char *where, bool fahrenheit)
{
    pthread_mutex_lock(&g_lock);
    if (!X) X = calloc(1, sizeof *X);
    if (X) {
        char w[64];
        snprintf(w, sizeof w, "%s", where ? where : "");
        if (strcmp(w, X->where) != 0 || fahrenheit != X->fahrenheit) {
            if (strcmp(w, X->where) != 0) X->have_ll = false;
            snprintf(X->where, sizeof X->where, "%s", w);
            X->fahrenheit = fahrenheit;
            X->cfg_gen++;
            X->next = 0;
            X->st.configured = w[0] != 0;
            X->st.ok = false;
            X->st.err[0] = 0;
            X->st.gen++;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void home_weather_get(home_weather_t *out)
{
    pthread_mutex_lock(&g_lock);
    if (X) *out = X->st;
    else memset(out, 0, sizeof *out);
    pthread_mutex_unlock(&g_lock);
}

void home_weather_forecast(home_forecast_t *out)
{
    pthread_mutex_lock(&g_lock);
    if (X) {
        *out = X->fc;
        out->gen = X->st.gen;
    } else {
        memset(out, 0, sizeof *out);
    }
    pthread_mutex_unlock(&g_lock);
}

void home_weather_refresh(void)
{
    pthread_mutex_lock(&g_lock);
    if (X) X->next = 0;
    pthread_mutex_unlock(&g_lock);
    home_kick();
}

/* "2026-09-24T07:02" -> "7:02" */
static void hhmm(const char *iso, char *out, size_t n)
{
    const char *t = strchr(iso, 'T');
    out[0] = 0;
    if (!t || strlen(t) < 6) return;
    int h = atoi(t + 1), m = atoi(t + 4);
    snprintf(out, n, "%d:%02d", h, m);
}

/* the day of the week of "2026-09-24" (0 sunday), -1 if it isn't a date */
static int weekday(const char *ymd, int *mday)
{
    int y, m, d;
    if (sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) return -1;
    *mday = d;
    static const int T[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7;
}

static int arr_at(const home_json_t *j, int obj, const char *key, int i)
{
    int a = obj < 0 ? -1 : jl_get(&j->d, obj, key);
    return a < 0 ? -1 : jl_at(&j->d, a, i);
}

static void read_forecast(const home_json_t *j, home_forecast_t *fc)
{
    memset(fc, 0, sizeof *fc);
    int cur = jl_get(&j->d, 0, "current"), hourly = jl_get(&j->d, 0, "hourly"), daily = jl_get(&j->d, 0, "daily");
    fc->feels = (float)home_json_num(j, cur, "apparent_temperature", NAN);
    fc->humidity = (int)home_json_num(j, cur, "relative_humidity_2m", -1);
    fc->wind = (float)home_json_num(j, cur, "wind_speed_10m", -1);
    for (int i = 0; hourly >= 0 && i < HOME_WX_HOURS; i++) {
        int t = arr_at(j, hourly, "time", i);
        if (t < 0) break;
        char iso[24];
        jl_str(&j->d, t, iso, sizeof iso);
        const char *tp = strchr(iso, 'T');
        fc->hour[i].hour = tp ? atoi(tp + 1) : 0;
        fc->hour[i].temp = (float)jl_num(&j->d, arr_at(j, hourly, "temperature_2m", i), 0);
        fc->hour[i].code = (int)jl_num(&j->d, arr_at(j, hourly, "weather_code", i), -1);
        fc->hour[i].pop = (int)jl_num(&j->d, arr_at(j, hourly, "precipitation_probability", i), -1);
        fc->hour[i].day = jl_num(&j->d, arr_at(j, hourly, "is_day", i), 1) != 0;
        fc->nhours = i + 1;
    }
    for (int i = 0; daily >= 0 && i < HOME_WX_DAYS; i++) {
        int t = arr_at(j, daily, "time", i);
        if (t < 0) break;
        char ymd[16];
        jl_str(&j->d, t, ymd, sizeof ymd);
        fc->day[i].wday = weekday(ymd, &fc->day[i].mday);
        fc->day[i].hi = (float)jl_num(&j->d, arr_at(j, daily, "temperature_2m_max", i), 0);
        fc->day[i].lo = (float)jl_num(&j->d, arr_at(j, daily, "temperature_2m_min", i), 0);
        fc->day[i].code = (int)jl_num(&j->d, arr_at(j, daily, "weather_code", i), -1);
        fc->day[i].pop = (int)jl_num(&j->d, arr_at(j, daily, "precipitation_probability_max", i), -1);
        fc->ndays = i + 1;
    }
    char iso[24];
    int t = arr_at(j, daily, "sunrise", 0);
    if (t >= 0 && jl_str(&j->d, t, iso, sizeof iso)) hhmm(iso, fc->sunrise, sizeof fc->sunrise);
    t = arr_at(j, daily, "sunset", 0);
    if (t >= 0 && jl_str(&j->d, t, iso, sizeof iso)) hhmm(iso, fc->sunset, sizeof fc->sunset);
    fc->ok = fc->nhours > 0 || fc->ndays > 0;
}

const char *home_weather_text(int c)
{
    if (c == 0) return "clear";
    if (c == 1) return "mostly clear";
    if (c == 2) return "partly cloudy";
    if (c == 3) return "overcast";
    if (c == 45 || c == 48) return "fog";
    if (c >= 51 && c <= 57) return "drizzle";
    if (c >= 61 && c <= 67) return "rain";
    if (c >= 71 && c <= 77) return "snow";
    if (c >= 80 && c <= 82) return "showers";
    if (c == 85 || c == 86) return "snow showers";
    if (c >= 95) return "thunderstorm";
    return "weather";
}

/* "47.61, -122.33" → true with the numbers */
static bool parse_ll(const char *s, double *lat, double *lon)
{
    char *e;
    double a = strtod(s, &e);
    if (e == s) return false;
    while (*e == ' ' || *e == ',') e++;
    char *e2;
    double b = strtod(e, &e2);
    if (e2 == e) return false;
    while (*e2 == ' ') e2++;
    if (*e2) return false;
    if (a < -90 || a > 90 || b < -180 || b > 180) return false;
    *lat = a;
    *lon = b;
    return true;
}

static int get(const char *url, char *out)
{
    hal_http_req_t rq = { .url = url, .headers = "Accept: application/json\r\n", .timeout_ms = 10000 };
    int len;
    return hal_http_fetch(&rq, out, RESP_MAX, &len);
}

static bool geocode(const char *where, double *lat, double *lon, char *place, size_t pn, char *err, size_t en,
                    char *buf)
{
    char q[160], url[320];
    home_urlenc(where, q, sizeof q);
    snprintf(url, sizeof url, "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json", q);
    int status = get(url, buf);
    if (status != 200) {
        snprintf(err, en, status < 0 ? "no internet for the weather" : "the place search answered %d", status);
        return false;
    }
    home_json_t j;
    bool ok = false;
    if (home_json_parse(&j, buf, strlen(buf))) {
        int r = jl_get(&j.d, 0, "results");
        int first = r >= 0 ? jl_at(&j.d, r, 0) : -1;
        if (first >= 0) {
            *lat = home_json_num(&j, first, "latitude", 0);
            *lon = home_json_num(&j, first, "longitude", 0);
            char name[40], cc[8];
            home_json_str(&j, first, "name", name, sizeof name);
            home_json_str(&j, first, "country_code", cc, sizeof cc);
            snprintf(place, pn, "%s%s%s", name, cc[0] ? ", " : "", cc);
            ok = true;
        }
        home_json_free(&j);
    }
    if (!ok) snprintf(err, en, "no place called \"%.30s\"", where);
    return ok;
}

void home_wx_work(double now, bool want)
{
    static char *buf;
    pthread_mutex_lock(&g_lock);
    if (!X || !X->where[0] || !want || now < X->next) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    char where[64];
    snprintf(where, sizeof where, "%s", X->where);
    bool f = X->fahrenheit, have_ll = X->have_ll;
    double lat = X->lat, lon = X->lon;
    unsigned gen = X->cfg_gen;
    X->next = now + RETRY_S; /* whatever happens, not again straight away */
    pthread_mutex_unlock(&g_lock);
    if (!buf) buf = malloc(RESP_MAX);
    if (!buf) return;

    home_weather_t st = { .configured = true, .fahrenheit = f };
    home_forecast_t *fc = calloc(1, sizeof *fc); /* ~800 bytes: off the worker's stack */
    char place[48] = "";
    if (!have_ll) {
        if (parse_ll(where, &lat, &lon)) {
            snprintf(place, sizeof place, "%.2f, %.2f", lat, lon);
            have_ll = true;
        } else {
            have_ll = geocode(where, &lat, &lon, place, sizeof place, st.err, sizeof st.err, buf);
        }
    }
    if (have_ll) {
        char url[400];
        snprintf(url, sizeof url,
                 "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,weather_code,is_day,apparent_temperature,relative_humidity_2m,wind_speed_10m"
                 "&hourly=temperature_2m,weather_code,precipitation_probability,is_day&forecast_hours=24"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
                 "&forecast_days=7&timezone=auto&temperature_unit=%s&wind_speed_unit=%s",
                 lat, lon, f ? "fahrenheit" : "celsius", f ? "mph" : "kmh");
        int status = get(url, buf);
        home_json_t j;
        if (status == 200 && home_json_parse(&j, buf, strlen(buf))) {
            int cur = jl_get(&j.d, 0, "current"), daily = jl_get(&j.d, 0, "daily");
            st.temp = (float)home_json_num(&j, cur, "temperature_2m", 0);
            st.code = (int)home_json_num(&j, cur, "weather_code", -1);
            st.day = home_json_num(&j, cur, "is_day", 1) != 0;
            int hi = daily >= 0 ? jl_get(&j.d, daily, "temperature_2m_max") : -1;
            int lo = daily >= 0 ? jl_get(&j.d, daily, "temperature_2m_min") : -1;
            st.hi = (float)jl_num(&j.d, hi >= 0 ? jl_at(&j.d, hi, 0) : -1, st.temp);
            st.lo = (float)jl_num(&j.d, lo >= 0 ? jl_at(&j.d, lo, 0) : -1, st.temp);
            st.ok = cur >= 0;
            if (fc) read_forecast(&j, fc);
            home_json_free(&j);
        }
        if (!st.ok) snprintf(st.err, sizeof st.err, status < 0 ? "no internet for the weather" : "the weather answered %d", status);
    }

    pthread_mutex_lock(&g_lock);
    if (gen == X->cfg_gen) {
        if (have_ll && !X->have_ll) {
            X->have_ll = true;
            X->lat = lat;
            X->lon = lon;
            snprintf(X->st.place, sizeof X->st.place, "%s", place);
        }
        snprintf(st.place, sizeof st.place, "%s", X->st.place);
        st.gen = X->st.gen + 1;
        X->st = st;
        if (fc && fc->ok) X->fc = *fc;
        else if (!st.ok) X->fc.ok = false;
        X->next = now + (st.ok ? EVERY_S : RETRY_S);
    }
    pthread_mutex_unlock(&g_lock);
    free(fc);
}
