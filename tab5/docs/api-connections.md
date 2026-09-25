# Catalyst Tab — every API connection, in one place

Audience: whoever maintains the FrcCatalyst robot-side library, or anyone auditing what this tablet
talks to and why. `docs/catalyst-integration.md` is the exhaustive NT4/HTTP-vs-FrcCatalyst
cross-reference for the robot side specifically — read that first for the library gap analysis. This
page is the map of *every* connection the firmware makes or serves: NetworkTables (reads **and**
writes), the Systemcore agent, Catalyst Link, the cloud APIs, and the tablet's own local interfaces
(SD card, dev console). Every claim below cites `file:line` in this checkout
(`tab5/` at `C:\Users\yu_th\dev\_worktrees\Bezel-tab5-apidoc\tab5`, branch `tab5-apidoc`). Anything not
confirmed by reading the source is marked `TODO(verify)`.

---

## 1. NetworkTables 4 (robot ↔ tablet)

The tablet's NT4 client subscribes to a fixed set of prefixes at boot — nothing outside them ever
reaches the tablet:

```c
static const char *const prefixes[] = { "/Catalyst/", "/FMSInfo/", "/Auto Selector/", "/SmartDashboard/",
                                         "/limelight", "/PathPlanner/", NULL };
nt4_config_t ncfg = { .client_name = "catalyst-tab", .period_s = 0.05, .prefixes = prefixes };
```
`main/main.c:217-219`. Client name `catalyst-tab`, 50 ms period (20 Hz nominal update rate).

### 1.1 Reads

`docs/catalyst-integration.md` §1–16 is the authoritative, already-verified table of every topic the
tablet reads (identity, mode/FMS, battery/power, loop time, CAN, alerts/health, mechanisms, pose/swerve,
Systemcore, tunables, auto chooser, preflight, controls manifest, states, motor history, recorder) with
its PUBLISHED/MISMATCH/MISSING classification against FrcCatalyst alpha-7/alpha-6/v1.1.0. This
document does not repeat that table. Re-verifying a sample of its highest-stakes claims against the
Catalyst docs MCP (bundled docs: `2.0.0-alpha.2`) found no `BatteryMonitor`, `TunablesManifest`,
`ControlsManifest` or a standalone `Swerve/HeadingDeg` publisher anywhere in the documentation search
results either — consistent with catalyst-integration.md's MISSING findings. **Caveat:** the bundled
docs (`alpha.2`) are older than the alpha-7/beta.2 checkouts catalyst-integration.md read directly, so
this is corroboration, not an independent re-derivation; where the two disagree, trust
catalyst-integration.md's direct source reads.

Reading source directly confirms catalyst-integration.md's scope claim: outside `cat_model.c` and
`ui_apps_sc.c`, none of `cat_batt.c`, `cat_can.c`, `cat_preflight.c` read NT4 topics directly (they
operate on data `cat_model.c` already fetched) — confirmed by grep, zero NT4 topic references in those
three files. `cat_logs.c` reads `.wpilog`/`.dslog` files from the SD card, not NetworkTables
(`cat_logs.c:239-241` only *matches* `/Catalyst/.../State` strings **inside** a parsed log file, it
never subscribes to NT4).

### 1.2 Writes — not covered by catalyst-integration.md

The tablet is not purely read-only. Three write paths exist, all narrow and all guarded:

| Topic | Type | Written from | Guard | UI call site |
|---|---|---|---|---|
| `<tunable key>` (e.g. `/Catalyst/Tuning/<Name>/kP`, or whatever key the manifest/fallback discovery reported) | double or bool | `cat_set_tunable()`, `cat_model.c:694-705` | absolute path only; refuses any key starting `/FMSInfo` or `/.schema` (`cat_model.c:699`); value clamped to the tunable's `[min,max]` when known (`cat_model.c:700-701`) | `ui_apps_robot.c:237,254,272` (the tune screen's slider/toggle) |
| `<auto base>/selected` (e.g. `/Auto Selector/selected`) | string | `cat_select_auto()`, `cat_model.c:707-714` | only when `r->have_autos` is true | `ui_apps_robot.c:398` (auto screen, picking an autonomous routine) |
| `/<limelight name>/ledMode` | double (`2` = on, `0` = off) | `cat_blink_limelight()`, `cat_model.c:716-721` | none beyond a valid camera name | `ui_apps_robot.c:668,676` (camera "blink to identify" button, press/release) |

No other NT4 write exists in `components/catalyst/src` — confirmed by grep for `nt4_set_` across
`cat_model.c`, `cat_batt.c`, `cat_can.c`, `cat_logs.c`, `cat_preflight.c`, `cat_sc.c`, `cat_sc_io.c`:
only `cat_model.c:702,703,712,720` call it. This matches the invariant both `catalyst-contract.md:10-13`
and `README.md:50-52` state: the tablet only ever writes a value a human explicitly set (a tunable
slider, an auto pick, a camera LED toggle) — never a command, mode change, or anything that starts or
stops the robot.

---

## 2. Systemcore / catalyst-agent HTTP (port 9010)

Two independent HTTP clients on the tablet talk to the same agent, at the same port, confirmed
identical:

- `components/catalyst/include/cat_sc.h:48`: `#define CAT_AGENT_PORT 9010`
- `components/assist/src/as_tools.c:24`: `#define AGENT_PORT 9010`

### 2.1 `cat_sc_io.c` — the pulse/systemcore screens' poller

| Route | Method | Caller | Cadence / timeout | Buffer | Notes |
|---|---|---|---|---|---|
| `/api/system` | GET | `poll_system()`, `cat_sc_io.c:158-187`, via `cat_agent_url(...,"/api/system")` at `cat_sc_io.c:161` | polled by the agent thread loop; 2500 ms timeout (`cat_sc_io.c:164`) | 256 KiB (`AG_BUF`, `cat_sc_io.c:70`) | 3 misses (`CAT_AGENT_GIVE_UP`, `cat_sc.h:51`) → state `CAT_AG_ABSENT`, treated as "not installed," not an error (`cat_sc_io.c:176-184`) |
| `/api/motor-history` | GET | `fetch_mh_agent()`, `cat_sc_io.c:201-221`, via `cat_agent_url(...,"/api/motor-history")` at `cat_sc_io.c:204` | 4000 ms timeout (`cat_sc_io.c:208`) | 768 KiB (`MH_BUF`, `cat_sc_io.c:71`) | falls back to parsing `/Catalyst/MotorHistory/Rows` off NT4 (totals only) when the agent doesn't answer (`fetch_mh_nt()`, `cat_sc_io.c:224-255`) |

The agent's host is derived from the live NT4 connection's own address, with the NT4 port stripped
(`cat_agent_url()`, `cat_sc_io.c:57-66`): `http://<nt4-host>:9010<path>`. No TLS — plain HTTP on the
robot's own network (`cat_sc_io.c:37` comment).

### 2.2 `as_tools.c` — the assistant's own tools

| Route | Method | Caller | Buffer | Used by |
|---|---|---|---|---|
| `/api/system` | GET | `agent_get()`, `as_tools.c:882-925`, called with `"/api/system"` at `as_tools.c:939-941` inside `t_systemcore()` | 256 KiB | the assistant's `systemcore_health` tool |
| `/api/motor-history` | GET | same `agent_get()` helper, called with `"/api/motor-history"` at `as_tools.c:1087` | (per call site) | the assistant's `motor_history` tool |

`agent_get()` resolves the address from the live NT4 connection the same way `cat_sc_io.c` does
(`as_tools.c:892-901`), 8000 ms timeout (`as_tools.c:903`). A non-200 or unreachable agent produces a
structured `{error, url, note}` JSON the assistant can read and explain, distinguishing "catalyst-agent
isn't running" from "this looks like a roboRIO, catalyst-agent doesn't run there" (`as_tools.c:910-923`).

Both clients hit exactly the same two routes at exactly the same port — no drift between the "systemcore
screen" path and the "ask the assistant" path.

### 2.3 The assistant's desk (`get_matches`, `get_batteries`) — not a network route, but an internal one

Two more read-only tools are declared in `components/assist/src/as_tooldefs.c:57-66` (`get_matches`,
`get_batteries`, both `AS_T_READ`) and both implemented by one function, `t_desk()`
(`components/assist/src/as_tools.c:718-737`), dispatched at `as_tools.c:1688-1689`. Neither one makes a
network call itself; both just hand back the latest text the UI already posted to "the desk"
(`components/assist/src/as_desk.c`), a small one-writer-many-readers text cache keyed by `as_desk_t`
(`AS_DESK_MATCHES`, `AS_DESK_BATTERIES`, `AS_DESK_NOW`, `assist.h:145`): `assist_desk_post()` copies a
string in under a lock, `assist_desk_get()` copies it back out (`as_desk.c:15-37`).

- **`get_matches`** reads whatever `desk_matches()` last posted (`components/ui/src/ui_match.c:701-788`):
  the event name/place/dates, rank and record, the next unplayed match spelled out, then every one of the
  team's matches with its time, alliance and partners, and whether reminders are on. It's posted whenever
  TBA's data generation changes (`ma_tick()`, `ui_match.c:654-660`), i.e. after a successful poll — see
  §4.1's cadence.
- **`get_batteries`** reads whatever `desk_post()` last posted (`components/ui/src/ui_batt.c:552-564`):
  `cat_batt_summary()`'s text for the whole fleet. It's posted whenever the battery manager's generation
  (`BM.gen`) changes, and otherwise at most once a minute (`ui_batt.c:556-558`), from `bm_tick()`
  (`ui_batt.c:566-576`, the battery manager's own per-refresh tick).
- **`AS_DESK_NOW`** is a third, shorter slot neither tool reads directly: `desk_now()`
  (`ui_match.c:790-808`) posts one line — the next match, and the battery pick's summary if there is one —
  every 30 s (`ui_match.c:661-664`, `MA.next_desk`), and immediately whenever `desk_matches()` runs.
  `voice.c`'s `context_note()` fetches it with `assist_desk_get(AS_DESK_NOW)` (`voice.c:565-567`) and folds
  it into the bracketed `[tablet: ...]` note every question carries (`answer()` prepends this note to the
  question text at `voice.c:748-753`) — so the companion always has the next match and current battery
  pick in view even when it never calls a tool.

---

## 3. Catalyst Link — the PC companion's HTTP API

`link/catalyst_link/server.py` is the source of truth; `docs/link-api.md` is the existing prose
contract and is already thorough (pairing, auth, error shapes, the Messages-API bridge in both `api`
and `claude-code` backends, patches, inbox, files, Claude Code session hooks, media). This section adds
exact route line numbers and confirms the two documents agree.

- **Port:** `8765` (`Config.port`, `server.py:42`), matching `docs/link-api.md:10` and every CLI default
  (`cli.py:319,379,388,393`).
- **mDNS:** service `_catalyst-link._tcp.local.` (`link/catalyst_link/mdns.py:9`), matching
  `docs/link-api.md:10`.
- **Auth:** every non-pairing, non-status, non-`/admin` route requires `X-Link-Token`
  (`server.py:313-334`); a missing/wrong token is `401 {"ok":false,"error":"token"}` (`server.py:334`).

### 3.1 Public routes (`self.ROUTES`, `server.py:441-461`)

| Method | Path | Handler |
|---|---|---|
| GET | `/code/tree` | `code_tree`, `server.py:476-477` |
| GET | `/code/read` | `code_read`, `server.py:479-480` |
| GET | `/code/search` | `code_search`, `server.py:482-483` |
| GET | `/code/patches` | `code_patches`, `server.py:485-486` |
| POST | `/code/patch` | `code_patch`, `server.py:488-492` |
| GET | `/inbox` | `inbox_list`, `server.py:496-497` |
| POST | `/inbox` | `inbox_create`, `server.py:499-507` |
| GET | `/inbox/<id>` | `inbox_get`, `server.py:509-510` |
| POST | `/inbox/<id>/status` | `inbox_status`, `server.py:512-519` |
| GET | `/files` | `files_list` |
| POST | `/files` | `files_upload` |
| POST | `/v1/messages` | `messages` |
| GET | `/v1/claude/sessions` | `cc_sessions` |
| POST | `/v1/claude/events` | `cc_event` |
| POST | `/v1/claude/sessions/ack` | `cc_ack_all` |
| POST | `/v1/claude/sessions/<id>/ack` | `cc_ack` |
| GET | `/media/now` | `media_now` |
| GET | `/media/art` | `media_art` |
| POST | `/media/control` | `media_control` |

Plus the two token-free pairing routes (`server.py:322-325,354`: `/link/pair`, `/link/pair/confirm`),
`/link/status` (`server.py:320-321`), and the admin-only routes below. **19 tablet-facing routes total**
(18 in `ROUTES` + `/link/status`), matching `docs/link-api.md`'s coverage exactly — no drift found.

### 3.2 Admin routes (`self.ADMIN_ROUTES`, `server.py:360-369`) — loopback + main token only

| Method | Path | Handler |
|---|---|---|
| GET | `/admin/overview` | `adm_overview` |
| GET | `/admin/pairing` | `adm_pairing` |
| POST | `/admin/pairing/cancel` | `adm_pairing_cancel` |
| GET | `/admin/devices` | `adm_devices` |
| POST | `/admin/devices/<id>/forget` | `adm_forget` |
| POST | `/admin/inbox/<id>/status` | `adm_inbox_status` |
| GET | `/admin/claude-hooks` | `adm_hooks` |
| POST | `/admin/claude-hooks` | `adm_hooks_set` |

Restricted to a loopback client with no `Origin` header, carrying the main token
(`_admin()`, `server.py:371-379`) — a paired tablet gets 403/401 even with its own valid token.

### 3.3 What `docs/link-api.md` already covers in full (not re-derived here)

Pairing flow and rate limits (`link-api.md:25-57`), the `/v1/messages` bridge's two backends
(`link-api.md:95-155`), patch semantics (`link-api.md:176-213`), the inbox (`link-api.md:215-244`),
Claude Code session state machine and ETA model (`link-api.md:254-355`), and media transport
(`link-api.md:357-392`) — all cross-checked against `server.py`'s route table above with no
discrepancy found. Read `docs/link-api.md` for the request/response shapes; this section exists only to
pin exact route handlers to lines.

The tablet side of Link is `components/assist/src/link.c` (assistant/Messages bridge, `link.c:716`
constructs the `/v1/messages` URL) and `components/home/src/home_pc.c` (media polling, per
`link-api.md:389-391`).

---

## 4. Cloud APIs called directly by the firmware

Every one of these is a client the *tablet itself* opens over Wi-Fi — none go through Catalyst Link.

### 4.1 The Blue Alliance, read API v3

Source: `components/home/src/home_tba.c` (verified in full this session).

- Base: `https://www.thebluealliance.com/api/v3` (`home_tba.c:18`).
- Auth: `X-TBA-Auth-Key: <key>` header (`home_tba.c:214`), key from `TBA_API_KEY` in `KEYS.ENV`
  (`docs/keys.md:25,42`).
- Conditional GET: `If-Modified-Since` sent when a prior `Last-Modified` is held; a `304` keeps the
  cached body (`home_tba.c:214-216,252-253`).
- Four endpoints, walked in order (`home_tba.c:26` `enum { EP_EVENTS, EP_MATCHES, EP_STATUS, EP_RANKS }`):
  1. `GET /team/frc<team>/events/<year>/simple` — the team's events this season (`home_tba.c:552`)
  2. `GET /team/frc<team>/event/<key>/matches/simple` — matches at the picked event (`home_tba.c:563`)
  3. `GET /team/frc<team>/event/<key>/status` — the team's status at that event (`home_tba.c:564`)
  4. `GET /event/<key>/rankings` — the event's rankings (`home_tba.c:565`)
- Poll cadence: events every 600 s (`EVENTS_S`, `home_tba.c:21`), the other three every 60 s normally,
  20 s once the picked event is running (`POLL_S`/`POLL_LIVE_S`, `home_tba.c:19-20`); 20 s retry after
  a failure (`RETRY_S`, `home_tba.c:22`).
- Body cap 768 KiB (`BODY_MAX`, `home_tba.c:24`).
- Offline cache: every successful body is written to `<sd>/CATOS/DATA/tba/<path-with-/-as-_>.json`
  (`card_save()`, `home_tba.c:147-169`; path built at `home_tba.c:134-145`), and read back when no
  network body exists yet (`card_load()`, `home_tba.c:171-192`) — so the event screen works offline
  from the last successful poll.
- A `401` is surfaced as "the blue alliance refused the key" (`say()`, `home_tba.c:510-515`) and short-
  circuits further per-event fetches (`home_tba.c:561,568`).
- TBA's data also reaches the assistant: `ui_match.c`'s `desk_matches()`/`desk_now()` post it to the
  "desk" (`AS_DESK_MATCHES`/`AS_DESK_NOW`) whenever a poll here changes `tba_gen()` — see §2.3.

### 4.2 Open-Meteo (weather + geocoding)

Source: `components/home/src/home_weather.c` (verified in full this session).

- Geocoding: `GET https://geocoding-api.open-meteo.com/v1/search?name=<q>&count=1&language=en&format=json`
  (`home_weather.c:194`) — used when the configured place isn't already a `"lat, lon"` pair
  (`parse_ll()`, `home_weather.c:165-180`, tried first at `home_weather.c:242`).
- Forecast: `GET https://api.open-meteo.com/v1/forecast?latitude=<lat>&longitude=<lon>&current=…&hourly=…
  &daily=…&forecast_days=7&timezone=auto&temperature_unit=<celsius|fahrenheit>&wind_speed_unit=<kmh|mph>`
  (`home_weather.c:250-257`) — the exact `current`/`hourly`/`daily` parameter lists are in the source at
  those lines.
- No API key: both are Open-Meteo's free, keyless endpoints.
- 10 s timeout (`home_weather.c:184`), 20 s retry on failure (`RETRY_S`, not shown above but same file).

### 4.3 OpenAI (Chat Completions) — one of the assistant's three routes

Source: `components/assist/src/assist.c`, `as_oai.c`, `as_oai.h` (verified this session).

- Base: `https://api.openai.com` (`AS_OAI_DEFAULT_BASE`, `as_oai.h:22`), overridable
  (`A.oai_base`, `assist.c:370`; `assist_set_base_url()`, `assist.c:1235`).
- Endpoint: `POST <base>/v1/chat/completions` (`assist.c:377`).
- Auth: `Authorization: Bearer <key>` (`as_oai_headers()`, `as_oai.c:155-158`), key from
  `OPENAI_API_KEY` in `KEYS.ENV` (`docs/keys.md:23,40`) or typed in settings.
- Default model: `gpt-4o-mini` (`AS_OAI_DEFAULT_MODEL`, `as_oai.h:21`), overridable per
  `A.cfg.oai_model` (`assist.c:679,1117-1220`).
- The tablet's own Anthropic-shaped Messages requests are translated to OpenAI's Chat Completions shape
  by `as_oai.c` (message roles, tool_result → `role:"tool"` messages, `join_text()`/`result_text()`
  helpers, `as_oai.c:13-45`) — this is the "one contract, two shapes" adapter for the assistant.
- This is chosen only when `AS_ROUTE_OPENAI` is the active route (`as_route_t`, `assist.h:33`); the
  assistant otherwise prefers `AS_ROUTE_DIRECT` (an Anthropic key on the tablet) or `AS_ROUTE_LINK`
  (Catalyst Link, §3) — chosen by which key is present (`assist.c:1124`).

### 4.4 Anthropic — direct route (no PC involved)

- Base: `https://api.anthropic.com` (`assist.c:371`), overridable via `A.base`.
- Endpoint: `POST <base>/v1/messages` (`assist.c:386`).
- Auth headers: `x-api-key: <key>`, `anthropic-version: 2023-06-01`,
  `anthropic-beta: server-side-fallback-2026-07-01` (`AS_BETA`, `as_conv.h:12`), `accept:
  text/event-stream` (`as_conv_headers()`, `as_conv.c:148-153`).
- Default model: `claude-opus-5` (`AS_DEFAULT_MODEL`, `as_conv.h:11`), key from `ANTHROPIC_API_KEY` in
  `KEYS.ENV` (`docs/keys.md:24,41`, kv name `ai_key`, `assist.c:908`).
- This is `AS_ROUTE_DIRECT` — the tablet talks straight to `api.anthropic.com`, over the pit Wi-Fi, with
  a key stored on the tablet itself; no PC, no Catalyst Link. Distinct from §3's Link-mediated
  `/v1/messages`, which uses the **same wire format** but never puts a key on the tablet.
- `components/assist/src/voice.c` (re-read in full this pass; every citation below re-derived, since the
  file has grown since the previous pass and old line numbers no longer line up) is always OpenAI — it
  has no Anthropic or Catalyst Link path at all. Its single `post()` helper (`voice.c:286-331`) builds
  every request against `V.base`, which `load_config()` sets from the `oai_base` kv or else
  `AS_OAI_DEFAULT_BASE` (`voice.c:200-207`) — the same OpenAI base §4.3 uses, never `api.anthropic.com`.
  The three calls it makes are all OpenAI REST endpoints: `POST <base>/v1/audio/transcriptions` for
  speech-to-text (`voice.c:490`), `POST <base>/v1/chat/completions` for the answer (`voice.c:775`), and
  `POST <base>/v1/audio/speech` for text-to-speech (built at `voice.c:899`, opened at `voice.c:906`).
  `voice.c` does `#include "link.h"` but calls nothing from it — the only cross-module calls it makes
  outside `as_oai.c`/`as_tools.c` are to `ccwatch.h`'s `ccw_list()`/`ccw_available()` (`voice.c:555-563`,
  inside `context_note()`), which only read Claude Code session state for the context note, not a network
  route. So: the voice feature always uses OpenAI's Chat Completions/Whisper/TTS endpoints directly from
  the tablet; it cannot route through Catalyst Link or a direct Anthropic key the way the text assistant
  (§4.4) can.
- **Text-to-speech asks for MP3 when there's a decoder, PCM otherwise.** `speak()` opens a decoder with
  `hal_mp3_open()` (`voice.c:882`) and picks the request's `response_format` from whether that succeeded
  (`voice.c:883`: `"mp3"` when `mp3` is non-NULL, `"pcm"` otherwise). On real hardware `hal_mp3_open()`
  (`components/tab_hal/src/hal_tab5_audio.c:508-527`) opens `esp_audio_codec`'s simple MP3 decoder and
  normally succeeds; the Linux simulator's `hal_mp3_open()` (`sim/hal_sim.c:192`) always returns `NULL` —
  "no decoder here: the companion fetches PCM" — so the simulator always requests `"pcm"`. MP3 is worth a
  tenth of PCM's bytes over the C6 (the comment at `voice.c:859-861` ties this directly to the Wi-Fi
  co-processor hangs under sustained 24 kHz PCM load — see the new §5.6).
- **A stream that won't decode falls back to PCM for good.** `V.tts_pcm` (declared `voice.c:104`) is
  checked before every attempt to open the decoder (`voice.c:880-882`: `want_pcm` skips `hal_mp3_open()`
  entirely once set) and is set to `true` the first time an MP3 stream comes back undecodable, the sink
  fails to start, or nothing ever plays (`voice.c:965-969`). Nothing in `voice.c` ever clears it, so once
  one TTS reply fails to decode, every later reply in that boot asks for `"pcm"` — the same permanent
  downgrade `V.stt_fallback`/`V.tts_fallback` (`voice.c:103`) apply to the transcription and speech
  *models* when the configured one is refused.
- **Speech is skipped below 2% volume.** `turn()` gates whether an answer is spoken at all on
  `V.enabled && hal_volume() > 0.02f` (`voice.c:1039`, `can_speak`); below that, `rep->spoken` stays
  false and the answer is only shown on screen (`voice.c:1041-1042`) — `speak()` (and so the
  `/v1/audio/speech` request) is never called (`voice.c:1047` only calls it `if (rep->spoken)`).
  `hal_volume()` returns the speaker's last-set level, 0..1 (`hal.h:83`; real implementation
  `hal_tab5.c:1766`, simulator `sim/hal_sim.c:187`).

### 4.5 Home Assistant — REST API

Source: `components/home/src/home_ha.c` (verified this session).

- Base: `HA_URL` from `KEYS.ENV` (`docs/keys.md:26,43`), trailing `/` dropped.
- Auth: `Authorization: Bearer <HA_TOKEN>` (`home_ha.c:327`, and again at `home_ha.c:601` for the raw
  states fetch).
- Connectivity check (no tiles configured): `GET /api/` (`home_ha.c:501`).
- Per-tile read, template path (needs an admin token): `POST /api/template` with a Jinja loop over the
  configured entity ids, extracting state/unit/area/brightness/color-mode/climate attributes in one
  round trip (`poll_template()`, `home_ha.c:394-417`; the exact template string is
  `home_ha.c:400-407`).
- Per-tile read, fallback path (non-admin token, used when the template call answers 400/403/405 —
  `home_ha.c:493`): `GET /api/states/<entity_id>` one at a time (`poll_each()`, `home_ha.c:450-475`,
  path built at `home_ha.c:455`).
- Bulk read (used elsewhere, e.g. picking entities in settings): `GET /api/states`
  (`home_ha.c:600-601`).
- Writes (device control): `POST /api/services/light/turn_on` / `turn_off` with
  `{"entity_id","brightness_pct"}` for a dimmer tile (`home_ha.c:528-532`); `POST
  /api/services/climate/set_temperature` with `{"entity_id","temperature"}` (`home_ha.c:533-535`); a
  generic `POST /api/services/<domain>/<service>` with `{"entity_id"}` for a plain tap-to-toggle tile
  (`home_ha.c:536-540`, service resolved by `tap_service()`).
- Poll cadence: `POLL_S` = 5.0 s on success, `RETRY_S` = 15.0 s on failure (`home_ha.c:17-18`, applied at
  `home_ha.c:518`).

### 4.6 FRC Events (FIRST) and frc.nexus — scaffolding only, no live call

`docs/keys.md:45-47` documents `NEXUS_API_KEY`, `FRC_EVENTS_USER`, `FRC_EVENTS_TOKEN` as "kept for
later." Confirmed by source: `assist.h:132` lists them among the KEYS.ENV names; `assist.c:912,978`
store them into kv slots `nexus_key`, `frc_ev_user`, `frc_ev_token`. **A grep of every `.c`/`.h` file in
`components/` and `main/` for `nexus_key`, `frc_ev_user`, `frc_ev_token`, or any HTTP call to
`frc.nexus` or `frc-events.firstinspires.org` found no code that ever reads these stored values back or
opens a connection to either service.** These are inert settings today — **MISSING (by design, not a
bug)**: nothing in the firmware calls FRC Events or Nexus.

---

## 5. Tablet-local interfaces

### 5.1 SD card layout

Fixed, upper-case 8.3-safe layout under `<sd>/CATOS/` (`ui_storage.h:1-11`, `cs_dir_t`/`CS_DIR_NAME`
enum at `ui_storage.h:18-19`):

| Folder | Purpose | Source |
|---|---|---|
| `CATOS/DOCS` | notes, `.TXT`/`.MD` to read; assistant transcripts saved as `MMDDHHMM.MD` | `ui_storage.h:3`, `ui_apps_tools.c:872` |
| `CATOS/PHOTOS` | pictures (the lens app's own snapshots live in `<sd>/lens` instead; the photos app shows both) | `ui_storage.h:4` |
| `CATOS/AUDIO` | music player's MP3/WAV files | `ui_storage.h:5`, `home_player.c:65` |
| `CATOS/DATA` | app data: `batteries.json` (battery fleet roster/history, `ui_batt.c:37` `FILE_NAME`), `EVENTS.TXT` (calendar), `tba/*.json` (TBA offline cache, §4.1) | `ui_storage.h:6`, `ui_batt.c`, `home_tba.c:134-169` |
| `CATOS/LOGS` | the OS's own logs | `ui_storage.h:7` |
| `CATOS/KEYS.ENV` (or root `KEYS.ENV`) | the dotenv key import file, zeroed and deleted (or renamed `.USED`) after import | `docs/keys.md:9-11`, `assist.c` key-import path |

`cstore_*` (`ui_storage.h:22-53`) is the shared file API every app above uses: `cstore_path`,
`cstore_layout`, `cstore_list`, `cstore_read`, `cstore_write` (atomic via a `.TMP` file), `cstore_usage`.

`batteries.json` schema, confirmed from `cat_batt_to_json()`/`cat_batt_from_json()`
(`components/catalyst/src/cat_batt.c:920-978,987-1051`, called from `ui_batt.c:193,251`):

```json
{
  "version": 1, "updated": 1234567890, "next_uid": 13,
  "seen": [3820147, ...],
  "batteries": [
    { "uid": 1, "label": "Big Red", "status": "good", "auto_watch": true, "year": 2025,
      "notes": "", "charged": 1234567890, "base_mohm": 14.2, "uses_total": 37,
      "uses": [
        { "t": 1234567890, "match": "2026casj_qm34", "label": "Q34", "charge": "fresh",
          "charged": 1234567890, "src": "pl", "v_rest": 12.90, "v_min": 11.80, "mohm": 16.0,
          "wh": 13.5, "amps": 35.0, "peak_a": 180, "dur_s": 150, "brownouts": 0, "log": "Q34.wpilog" }
      ] }
  ]
}
```
`status` is one of `CAT_BATT_STATUS` (`good`/`watch`/`bad`/`retired`, `cat_batt.c:15`); a use's `charge`
is one of `CAT_BATT_CHARGE` (`""`/`fresh`/`rested`/`used`, `cat_batt.c:16`); `src` is a subset of the
letters `p` (picked by hand), `l` (from a `.wpilog`/`.dslog`), `n` (folded in live from the robot,
`ui_batt.c:451-477`), written in that order (`cat_batt.c:954`). Every numeric field the value isn't known
for is simply omitted (`jb_num()`, `cat_batt.c:912-918`), not written as `null` or `0`.

This is the full on-card shape — roster **and** every recorded use. `ui_batt.c:5`'s "the roster without
histories" refers to the separate, smaller shape kept in the `batteries` kv slot (NVS) when there is no
SD card: `save_kv()` (`ui_batt.c:200-214`) copies the fleet, zeroes `nseen` and every battery's `nuse`,
then serializes with the same `cat_batt_to_json()` — so the kv copy is this same schema with `seen: []`
and every `uses: []`, capped at `KV_MAX` = 3900 bytes (`ui_batt.c:46,210`).

The TBA cache's file-naming rule: the URL's path after `/api/v3/` with every `/` replaced by `_`, plus
`.json` (`card_path()`, `home_tba.c:134-145`) — e.g.
`CATOS/DATA/tba/team_frc5805_events_2026_simple.json`.

### 5.2 Battery manager (BMS)

`components/ui/src/ui_batt.c` (read in full this session) is the tablet's battery fleet manager: the
roster, each battery's history, the checklist's "which battery goes in" picker, and (§5.1) the
`batteries.json`/`batteries` kv persistence. It is not itself an NT4 client — the live robot numbers it
folds in come from `cat_model.c`'s already-fetched `cat_robot_t` fields, the same struct §1.1 covers, not
from a topic this file subscribes to directly.

- **Live folding, while a battery is picked in and the robot runs.** `live_tick()`
  (`ui_batt.c:485-512`) runs every UI refresh: if the most recent pick is under `PICK_LIVE_S` = 4 h old
  and not already sourced from a log, and the robot reports `R->connected && R->have_battery &&
  R->battery_v > 3`, it accumulates that pick's voltage/current into `BM.lv.acc`
  (`cat_batt_acc_add()`/`cat_batt_acc_brown()`, `ui_batt.c:504-505`). `R->battery_v`, `R->total_current`
  (falling back to `R->pd_total`), `R->have_mode`/`R->enabled`, and `R->browned_out` are the
  `cat_robot_t` fields `cat_model.c`'s `read_power()` fills from `/Catalyst/Status/BatteryVolts` (or the
  `/Catalyst/Brownout/MeasuredVoltage` / `/Catalyst/Systemcore/BatteryVolts` fallbacks),
  `/Catalyst/Brownout/TotalCurrent`, `/SmartDashboard/<pdh>/TotalCurrent`, and
  `/Catalyst/Systemcore/BrownedOut` respectively (`cat_model.c:160-177`) — the exact topic list and its
  PUBLISHED/MISSING status against a given FrcCatalyst version is `docs/catalyst-integration.md`'s to
  give, not repeated here. `live_end()`/`live_commit()` (`ui_batt.c:451-483`) close the accumulator out
  into that use's `v_min`/`v_rest`/`amps`/`wh`/`peak_a`/`dur_s`/`brownouts` fields once the robot disables
  for `LIVE_END_S` = 15 s (`ui_batt.c:45`), tagging the use `CU_LIVE` (written as `"n"` in
  `batteries.json`'s `src`, §5.1).
- **Also reads `R->battery_model`** (`ui_batt.c:857`, from `/Catalyst/Robot/Power/Battery` per
  `cat_model.c:181`) to show the team's declared battery type/model on the roster screen — display only,
  nothing is written back.
- **Log scan, independent of NT4.** `scan_job()` (`ui_batt.c:333-373`), run on the assistant's worker
  thread, reads `.wpilog`/`.dslog` files directly off the SD card root and `logs/` (not through NT4) and
  attributes each one's numbers to a pick (`cat_fleet_attribute()`, `ui_batt.c:388`); this is the same
  `.wpilog` parsing `docs/api-connections.md` §1.1 notes for `cat_logs.c`, but battery numbers are pulled
  by `cat_batt_log()` (`components/catalyst/src/cat_batt.c`), a different reader.
- **No NT4 writes.** Nothing in `ui_batt.c` calls `nt4_set_*` — §1.2's write-path grep already covers the
  whole of `components/catalyst/src`, and `ui_batt.c` lives in `components/ui`, confirmed separately by
  grep here: zero `nt4_set_` references in this file. The battery manager only ever writes to
  `batteries.json` / the `batteries` kv slot (local storage, §5.1) and to the fleet's own in-memory state.
- **The assistant tie-in.** `ask_gpt()` (`ui_batt.c:538-547`) posts the fleet's summary
  (`cat_batt_summary()`) to the assistant's analysis pipeline (`analyze_fleet_start()`), the same
  OpenAI/Anthropic/Link routing §4.3–4.4 describe for the rest of the assistant — not a separate
  connection.

### 5.3 KEYS.ENV format

Full contract already documented in `docs/keys.md` (dotenv, comments, quoting, `export` prefix, CRLF
tolerance, unknown/empty values ignored) — not re-derived here. The name→kv-slot table there
(`docs/keys.md:38-50`) matches `assist.c:903-915`'s `ENV_KEYS[]` table exactly (both list the same 10
names): `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `TBA_API_KEY`, `HA_URL`, `HA_TOKEN`, `NEXUS_API_KEY`,
`FRC_EVENTS_USER`, `FRC_EVENTS_TOKEN`, `TEAM`, and the Wi-Fi pair (`WIFI_SSID`/`WIFI_PASS`, handled
separately by the Wi-Fi driver, `assist.c:901-902` comment).

### 5.4 USB dev console

`components/tab_hal/src/hal_tab5_dev.c` implements a line-based command console over the USB-C serial
port (115200 baud, DTR/RTS held low so opening the port doesn't reset the tablet — `tools/tab5_dev.py:24-26`
comment). Full command set, confirmed by grepping every `strcmp(line,...)`/`strncmp(line,...)`/`sscanf(line,...)`
dispatch in `hal_tab5_dev.c`'s `run()` (line numbers below are into that function, re-checked this pass —
the file has grown since the table was last written and every line number had shifted):

| Command | Effect | Source |
|---|---|---|
| `shot` / `pshot` | capture the current frame (RLE RGB565 over serial), `pshot` is the portrait/no-flip variant | `hal_tab5_dev.c:167,169` |
| `tap x y` | synthetic press+release at `(x,y)` | `hal_tab5_dev.c:171` |
| `swipe x0 y0 x1 y1 ms` | synthetic press-move-release | `hal_tab5_dev.c:174` |
| `down x y` / `move x y` | hold (or move) a synthetic finger without releasing it | `hal_tab5_dev.c:177` |
| `up` | release any held synthetic touch | `hal_tab5_dev.c:183` |
| `settime <epoch> [tz]` | set the tablet's clock and POSIX TZ | `hal_tab5_dev.c:186` |
| `bat` | battery status line | `hal_tab5_dev.c:201` |
| `net [host]` | Wi-Fi/network status (`net` alone, or a reachability check against `host`) | `hal_tab5_dev.c:208` |
| `putbegin <path> <len>` / `puthex <off> <hex>` / `putend <crc32>` | push a file onto the SD card in 512-byte hex-coded chunks, checked by CRC-32 | `hal_tab5_dev.c:215,234,256` |
| `c6ota <path>` | OTA the companion MCU (C6) from a file already on the card, then a planned restart | `hal_tab5_dev.c:265` |
| `wdhold [0]` | hold the C6 watchdog's restart for inspection (bare) or let it resume (`wdhold 0`) | `hal_tab5_dev.c:279` |
| `c6dbg` | esp-hosted's own SDIO debug line (`esp_hosted_sdio_debug()`) | `hal_tab5_dev.c:283` |
| `wdtest` | force the watchdog's restart path on demand, without a hung C6 | `hal_tab5_dev.c:290`, `hal_tab5_net.c:141-147` |
| `rejoin` | rejoin Wi-Fi | `hal_tab5_dev.c:294` |
| `mem` | heap/task memory report | `hal_tab5_dev.c:297` |
| `scan` | Wi-Fi AP scan, results as `AP ...` lines | `hal_tab5_dev.c:331` |
| `keybegin` / `keyhex <hex>` / `keyend` | push a `KEYS.ENV`-shaped file over serial (hex-coded, never printed); the tablet restarts and imports it | `hal_tab5_dev.c:341,349,356`, `tools/tab5_dev.py:135-144` |
| `ant 0\|1` | select the internal or external Wi-Fi antenna | `hal_tab5_dev.c:386` |
| `flip` | toggle screen orientation | `hal_tab5_dev.c:390` |

`put <local> <remote>` and `keys <file>` are `tools/tab5_dev.py`-side conveniences, not device commands:
`put` wraps `putbegin`/`puthex`/`putend` for one local file (`tools/tab5_dev.py:104-130`), and `keys`
wraps `keybegin`/`keyhex`/`keyend` for a `KEYS.ENV`-shaped file (`tools/tab5_dev.py:135-144`) — read only
to confirm this table, never run, and never opened as a `KEYS.ENV`/`keys.env` file itself. Any other
command (including `wdtest`) goes over the wire exactly as typed (`tools/tab5_dev.py:146-152`).

Commands not recognized by any of the above fall through to `s_handler`, a hook the UI installs
(`hal_dev_set_handler()`, `hal_tab5_dev.c:145-146,393-395`) so a handful of dev commands can reach the
UI thread instead of `tab_hal`. `components/ui/src/ui_shell.c`'s `dev_handler()`
(`ui_shell.c:743-753`, registered at `ui_shell.c:1933`) accepts these and queues them for `dev_run()`
(`ui_shell.c:760` on) to execute on the next UI frame:

| Command | Effect | Source |
|---|---|---|
| `open <app>` | open an app by name, as its launcher icon would | `ui_shell.c:763-765` |
| `page <n>` | close any open app and go to page `n` | `ui_shell.c:766-768` |
| `close` | close the current app | `ui_shell.c:769-770` |
| `alarm test [n] [match\|chime]` | fire a made-up match's queue/match reminder (or schedule-change chime) in `n` s (default 5) | `ui_shell.c:771-777` |
| `match fake [n]` | track a made-up match of ours `n` minutes out (default 30) as if real; `match fake 0` clears it | `ui_shell.c:778-782` |
| `say <text>` | post `text` from the island, as any notification would | `ui_shell.c:783-785` |
| `accent <name\|n>` | switch the accent colour, by name or index | `ui_shell.c:786-792` |
| `settings <page>` | open the settings app to a specific page | `ui_shell.c:793-794` |
| `bms [args]` | the battery manager's own sub-console (bare ranks the fleet; `scan`, `pick <label>`, `demo`, `reset`, `gpt`, `json`) | `ui_shell.c:795-797` |
| `inv` | trace the next 20 LVGL invalidations | `ui_shell.c:798-799` |
| `ask <text>` | a typed question to the companion, as its keyboard would send it | `ui_shell.c:800-802` |
| `home` | enter or leave Home mode | `ui_shell.c:803-805` |
| `redraw` | invalidate and redraw the whole screen | `ui_shell.c:806-809` |
| `perf` | print per-frame timing (hooks/LVGL/refresh/render/present) since the last `perf` | `ui_shell.c:810-822` |

`tools/tab5_dev.py` is the PC-side driver for all of these (`python tools/tab5_dev.py COM9 <cmd...>`,
commands chainable with `;`).

### 5.5 NFC / Grove hardware hooks

A hook exists — an earlier pass's grep terms missed it. `components/tab_hal/src/hal_tab5_nfc.c` drives an
NFC reader on **Grove Port A**: an M5Stack Unit RFID 2 (a WS1850S speaking the MFRC522 register set over
I2C at address `0x28`, `hal_tab5_nfc.c:1-18,33`), on its own I2C controller (`I2C_NUM_0`, SDA 53/SCL 54,
`hal_tab5_nfc.c:5,34-35`) separate from the system bus. The Tab5 has no NFC reader of its own — this only
works with the M5Stack unit plugged into Port A.

- `hal_nfc_init()` (`hal_tab5_nfc.c:318-326`, called once from `hal_settle()` per `hal_tab5.c:2700`)
  starts a low-priority task that waits `PROBE_DELAY_MS` = 4000 ms, switches Port A's 5 V on, and probes
  `0x28`; nothing there and the 5 V goes back off and the task exits (`hal_tab5_nfc.c:36,273-289`).
- Found, it polls for an ISO 14443A card every `POLL_MS` = 300 ms (WUPA + the anticollision/select
  cascade for a 4/7/10-byte UID, `hal_tab5_nfc.c:37,193-238,291-305`).
- `hal_nfc_present()` and `hal_nfc_card(uid, n)` (`hal_tab5_nfc.c:335,337-345`) are the public read: a
  present flag and the last UID as hex, guarded by a mutex the UI thread also takes.
- `hal_nfc_release()` (`hal_tab5_nfc.c:328-333`) stops the task and frees the bus for good — `hal_can_start()`
  calls it because the CAN tap reuses the same two pins (`hal_tab5_nfc.c:13`, `hal_tab5.c:2390`): NFC and
  the CAN tap are mutually exclusive, first one to start wins for that boot.
- Consumer: `components/ui/src/ui_home_mode.c`'s `tag_tick()` (`ui_home_mode.c:1034-1071`) polls
  `hal_nfc_present()`/`hal_nfc_card()` at 10 Hz and uses a paired tag's UID to trigger/leave Home mode
  (`UI_HOME_BY_TAG`); pairing UI is `ui_home_settings.c:131` and `hm_tag_pair_begin()`/`hm_tag_pair_state()`
  (`ui_home_mode.c:1073-1090`). The simulator stubs it with the `SIM_NFC_TAG` env var (`sim/hal_sim.c:421-423`).
- Nothing here calls this hardware "Grove" in the API sense — no other Grove-port peripheral or protocol
  exists in this checkout; "Grove" is only ever the physical connector name for Port A.

### 5.6 Wi-Fi co-processor (ESP32-C6 over esp-hosted SDIO)

Why this belongs here: every cloud API in §4 and the NT4 client in §1 ride over this link, and it drops
for a real, if usually brief, stretch and recovers on its own — worth knowing before reading a gap in
telemetry or a failed request as a bug. All of this is `components/tab_hal/src/hal_tab5_net.c`, read in
full this session.

**The hang this machinery was built for is fixed at its cause** (2026-09-25): the host reads one packet per
fixed 1.5 KB SDIO transfer (`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_MAX_SIZE`), but the C6 slave had been built
in stream mode, so one read could carry several packets and all but the first were lost; within minutes of
steady traffic the C6 stopped sending. The slave is now built in packet mode (`firmware/c6/`, with its
settings and how to install it over the air); stress runs went from a hang every few minutes to 30/30 mixed
and 60/60 companion rounds with no Wi-Fi event at all. The recovery below stays as the backstop for anything
else (a router that goes away, a real C6 fault).

- **The C6 sometimes stops answering its own RPCs.** `rssi_task()` (`hal_tab5_net.c:234-287`) polls
  `esp_wifi_sta_get_ap_info()` every 3 s; two consecutive misses while the link is otherwise "up" call
  `c6_restart("the C6 stopped answering")` (`hal_tab5_net.c:284-285`).
- **The C6 sometimes reports "connected" while nothing actually gets through.** The same task probes the
  gateway with a 2 s TCP connect to port 80 (`gw_alive()`, `hal_tab5_net.c:205-232`) — every 15 s
  normally, every 3 s once the gateway has gone quiet (`hal_tab5_net.c:260-261`,
  `if (++cycle >= 5 || dead)`). It only acts on a gateway that has answered before (`gw &&
  gw == answered`, `hal_tab5_net.c:267`) — one that has never answered is left alone, since silence from
  it is not news. Two quiet probes in a row rejoin the network (`esp_wifi_disconnect()`,
  `hal_tab5_net.c:270-273`, rate-limited to once a minute), four quiet probes in a row call
  `c6_restart("connected, but nothing gets through even after joining again")`
  (`hal_tab5_net.c:274-275`).
- **A separate watchdog catches it from the HTTP side.** `wifi_heal()` (`hal_tab5_net.c:1030-1045`) is
  called from `hal_http_open()`'s own failure path (`hal_tab5_net.c:1119`) — so both the assistant's and
  the companion's request failures count towards it. Three failed requests while Wi-Fi reports "up"
  trigger a rejoin (rate-limited to once every 2 minutes, `hal_tab5_net.c:1036`); if a fourth failure
  lands within 2 minutes of the last rejoin, it calls `c6_restart()` directly
  (`hal_tab5_net.c:1038`).
- **A restart to reset the C6 is spaced at least 45 s from the last one, always.** `c6_restart()`
  (`hal_tab5_net.c:172-201`) refuses to restart again inside 45 s of the last one it made
  (`hal_tab5_net.c:187`, tracked in `RTC_NOINIT_ATTR` state that survives the restart,
  `hal_tab5_net.c:116`) and never gives up permanently — the comment at `hal_tab5_net.c:165-168` notes
  this replaced an older backoff (10 minutes, then summed across boots) that could leave Wi-Fi dead for
  good until someone power-cycled the tablet. The restart itself is quick — "~6 s and comes back where
  it was" (`hal_tab5_net.c:185` comment) — which is why 45 s, not longer, is the floor. The dev console's
  `wdhold`/`wdtest` commands (§5.4) can hold this off for inspection or trigger it on demand.
- **esp-hosted can restart the host itself.** `esp_hosted_host_restarting()` (`hal_tab5_net.c:133-138`) is
  a weak hook esp-hosted's own `os_wrapper.c` calls when it decides the link is unrecoverable — most
  likely the C6 reset on its own (its own watchdog after a hang). Once boot has settled
  (`hal_boot_settled()`, `hal_tab5_net.c:135`) this is treated as the same kind of recovery as a
  `c6_restart()`: it notes where the UI was and marks the coming restart planned
  (`hal_restart_mark_planned()`, `hal_tab5_net.c:136-137`), rather than counting as a crash. Before boot
  has settled, it's left alone — so a C6 that never comes up ends in safe mode instead of restarting
  forever.
- **A planned restart resumes to the same page or app.** `ui_shell.c` registers `note_where()` as the
  restart hook (`hal_restart_hook(note_where)`, `ui_shell.c:1926`), which calls `hal_resume_note()` with
  the current page and open app (`ui_shell.c:414`, backed by `RTC_NOINIT_ATTR` state,
  `hal_tab5_net.c:119-124`); on the next boot `ui_shell.c:422` calls `hal_resume_take()` once to read it
  back and reopen the same place.
- **What this means for an integration:** a Wi-Fi-dependent connection to this tablet (NT4, §1; the
  Systemcore agent, §2; any cloud API, §4) can drop for anywhere from a few seconds (a quick RPC-miss
  restart) up to roughly a minute (a gateway-silence sequence: up to 15 s to notice, a rejoin, more quiet
  probes, then the restart), and won't retry a restart more often than every 45 s even under sustained
  failure. None of this needs the robot side to do anything — the tablet reconnects and resumes on its
  own — but a robot-side integration polling this tablet's own HTTP/NT4 surface (there is none today,
  per §6) should expect exactly this kind of gap, not treat it as a hang.

---

## 6. What to add to Catalyst

Re-derived from sections 1–5 above, cross-checked against `docs/catalyst-integration.md`'s own
priority list (which remains the authoritative source for items 1–3, 5 below — this list adds nothing
new for NT4 reads, only reflects what this pass's independent verification confirms still holds, plus
one item catalyst-integration.md doesn't cover: NT4 writes).

1. **`/Catalyst/Status/BatteryVolts` — zero-config battery voltage (P0).** Confirmed still absent from
   the bundled Catalyst docs search (§1.1) and already the top finding in
   `docs/catalyst-integration.md` (its §"Library changes to make," item 1). Every dashboard, not just
   this tablet, has no battery reading without a `BrownoutMonitor` or `HealthMonitor.systemCoreChecks()`
   opt-in.
   ```java
   // frc.lib.catalyst.util.BatteryMonitor — call once per loop
   public static void update() { CatalystLog.log("Status/BatteryVolts", RobotController.getBatteryVoltage()); }
   ```

2. **`/Catalyst/Tunables/.manifest` — a manifest publisher (P1).** No publisher found in the docs search
   either (§1.1); `cat_model.c:528` (per catalyst-integration.md §10) is ready to read it the moment one
   exists. Every `TunableGains`-backed gain on a 2.x robot shows unbounded and ungrouped on the tune
   screen without it.

3. **`/Catalyst/Controls/.manifest` — a controls manifest publisher (P1, already tracked in
   `systemcore.md:117-123`).** The controls screen (`ui_apps_sc.c:2005`, `cat_sc.h:223`) is functionally
   empty on every stock robot; only Catalyst X1's own robot code publishes this today.

4. **`/Catalyst/Swerve/HeadingDeg` — a standalone heading scalar (P1).** Confirmed still missing; the
   motion screen's swerve-arrow view has no heading source without a `PhysicsCore` pose.

5. **Confirm read-vs-write parity before adding anything new.** This session's independent read of
   `cat_model.c`'s three NT4 write call sites (`cat_set_tunable`, `cat_select_auto`,
   `cat_blink_limelight` — §1.2) confirms the tablet write surface is exactly: a tunable value (clamped,
   guarded against `/FMSInfo`/`/.schema`), the selected auto's name, and a Limelight's `ledMode`. A
   `TunablesManifest`/`ControlsManifest` publisher (items 2–3) must keep these three writable paths'
   shapes exactly as `cat_model.c:694-721` already expects them — a manifest that changes the *value*
   topic's path or type (as opposed to adding metadata alongside it) would silently break the existing
   write path with no compile-time signal on either side.

Everything else worth doing on the library side — the struct-vs-array pose/module-state question, the
dead `/Catalyst/Status/CanUtilization` and `/Catalyst/Match/TimeLeft` fallbacks, `AutonomyBoard`'s
unread fields — is already scoped in `docs/catalyst-integration.md`'s checklist and is not repeated
here.

## Open TODOs from this pass

All four `TODO(verify)` markers left by the previous pass were resolved by reading source in an earlier
session: `voice.c`'s route (§4.4, always OpenAI, no Anthropic/Link path), Home Assistant's poll/retry
seconds (§4.5, 5.0 s / 15.0 s), `batteries.json`'s full field list (§5.1), and the NFC/Grove hook (§5.5 —
one exists, at `components/tab_hal/src/hal_tab5_nfc.c`, missed by an earlier grep).

This pass re-read `voice.c` end to end and corrected every citation into that file (the previous pass's
line numbers had drifted as the file grew — see §4.4), added the MP3/PCM text-to-speech behaviour, the
permanent `V.tts_pcm` fallback, and the sub-2%-volume speech gate, all new since the previous pass. It
also added §2.3 (the assistant's "desk" and the two tools it feeds, `get_matches`/`get_batteries`), the
missing dev-console commands in §5.4 (both `hal_tab5_dev.c`'s own set and the UI-forwarded set in
`ui_shell.c`, with every line number in that table re-derived for the same reason as `voice.c`), and
§5.6 (the Wi-Fi co-processor's recovery behaviour: the RPC and gateway watchdogs, the 45 s restart floor,
and the planned-restart/resume mechanism).

Nothing new is marked `TODO(verify)` this pass — every number and function name added above was read
directly from the cited source line. Two things worth a human's attention rather than a marker:

- §5.6's "anywhere from a few seconds up to roughly a minute" downtime estimate is composed from several
  independently-cited numbers (3 s poll, 15 s/3 s gateway probe, ~6 s restart per the `hal_tab5_net.c:185`
  comment, 45 s floor) rather than a single constant in source; treat it as a derived estimate, not a
  guaranteed bound.
- §6 is left unchanged this pass: the desk mechanism (§2.3) turned out to be entirely tablet-local (The
  Blue Alliance and the on-card battery roster, neither reaching NT4), so nothing about it suggests a
  concrete new library-side publisher or consumer beyond what §6 already lists.
