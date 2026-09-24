/* The weather from Open-Meteo (no key, no account): the temperature now, today's high and low, and the WMO
 * code, every 20 minutes while home mode shows it. A place typed as a name is resolved once through
 * Open-Meteo's geocoder; "lat,lon" is used as it is. */
#include "home_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVERY_S (20 * 60)
#define RETRY_S 90
#define RESP_MAX 4096

typedef struct {
    char where[64];
    bool fahrenheit;
    unsigned cfg_gen, done_gen;
    bool have_ll;
    double lat, lon;
    home_weather_t st;
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
                 "&current=temperature_2m,weather_code,is_day&daily=temperature_2m_max,temperature_2m_min"
                 "&forecast_days=1&timezone=auto&temperature_unit=%s",
                 lat, lon, f ? "fahrenheit" : "celsius");
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
        X->next = now + (st.ok ? EVERY_S : RETRY_S);
    }
    pthread_mutex_unlock(&g_lock);
}
