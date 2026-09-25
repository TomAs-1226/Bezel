# Keys from the card

The tablet takes its API keys and a few settings from one file on the microSD card, so nothing long has
to be typed on the glass. At start-up it reads the file, stores each value where that setting lives,
overwrites the file with zeros and deletes it (renaming it to `.USED` if the card won't delete). The island
then names what came in, for example `from the card: TBA_API_KEY, HA_URL, HA_TOKEN`. Values are never
logged or shown.

## Where

`CATOS/KEYS.ENV` on the card, or `KEYS.ENV` at the card's root. Both are read if both are there. The older
single-key files `CATOS/KEYS/OPENAI.TXT` and `CATOS/KEYS/ANTHROPIC.TXT` still work.

## Format

dotenv: one `NAME=value` a line. `#` starts a comment line, and ` #` after a bare value starts a trailing
comment. Values may be in `"double"` or `'single'` quotes. An `export ` prefix is allowed, and Windows line
endings are fine. Unknown names are ignored, and so are empty values: an empty value never clears a
setting.

```sh
# CATOS/KEYS.ENV
OPENAI_API_KEY=sk-...
ANTHROPIC_API_KEY=sk-ant-...
TBA_API_KEY=...
HA_URL=http://192.168.1.30:8123
HA_TOKEN="eyJ..."
NEXUS_API_KEY=...
FRC_EVENTS_USER=myusername
FRC_EVENTS_TOKEN=...
TEAM=5805
WIFI_SSID="pit wifi"
WIFI_PASS=...
```

## The names

| Name | Stored as (kv) | Used by | Where to get it |
|---|---|---|---|
| `OPENAI_API_KEY` | `oai_key` | the assistant (OpenAI route), the companion's voice | platform.openai.com → API keys |
| `ANTHROPIC_API_KEY` | `ai_key` | the assistant (direct route) | console.anthropic.com → API keys |
| `TBA_API_KEY` | `tba_key` | the blue alliance app | thebluealliance.com/account → Read API Keys (a read key, sent as `X-TBA-Auth-Key`) |
| `HA_URL` | `ha_url` | home mode's Home Assistant tiles | your Home Assistant's address, e.g. `http://homeassistant.local:8123` (a trailing `/` is dropped) |
| `HA_TOKEN` | `ha_token` | the same | Home Assistant → your profile → Security → Long-lived access tokens |
| `NEXUS_API_KEY` | `nexus_key` | kept for later (header `Nexus-Api-Key`) | frc.nexus → your account → API |
| `FRC_EVENTS_USER` | `frc_ev_user` | kept for later | frc-events.firstinspires.org/services/API → register |
| `FRC_EVENTS_TOKEN` | `frc_ev_token` | kept for later | the token FIRST emails after registering there |
| `TEAM` | `team` | the team number everything looks for (robot address, the blue alliance) | your team number, 1–99999 |
| `WIFI_SSID` | the Wi-Fi driver | joins that network and remembers it | your network's name |
| `WIFI_PASS` | the Wi-Fi driver | its password; leave it out for an open network | |

A new value replaces the old one. The import runs before the settings are loaded, so everything that
reads them at start-up sees the new values without a second restart.
