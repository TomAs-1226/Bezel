//! Writing the tablet's `KEYS.ENV` to its microSD card, because there is no other way in.
//!
//! # There is no network route for this, and that is not an oversight of this module
//!
//! A tablet is configured by a single dotenv-style file the firmware reads once from the card, applies,
//! and then zeroes and deletes. Provisioning it from the PC would ideally happen over the Link, and it
//! cannot, for three reasons that were each checked rather than assumed:
//!
//! * The Link's HTTP API has **no PC-to-tablet push route of any kind**. Every route is either
//!   tablet-initiated or loopback-only admin; the one file route (`POST /files`) carries recordings
//!   *from* the tablet.
//! * The tablet has no USB mass-storage mode. Its USB-C port is the P4's USB-Serial/JTAG and its USB-A
//!   port is host-mode, for the robot tether.
//! * Wi-Fi cannot be the channel, because the Wi-Fi credentials are the main thing being provisioned.
//!
//! So the route is physical: the card goes in a reader, and this writes a file to it. Inventing a push
//! route would mean a new Link endpoint *and* a new firmware-side poller, which is a change to the wire
//! contract and not a desktop-app feature.
//!
//! The honest consequence, which the UI must state and not paper over: **this cannot confirm the
//! tablet read the file.** Confirmation is the tablet's own "from the card" message after the card goes
//! back in. A panel that implied otherwise would be lying about the one thing the user wants to know.
//!
//! # What is written
//!
//! `NAME=value` per line. Empty values are omitted rather than written, because the firmware treats an
//! empty value as "leave this alone" — writing `WIFI_PASS=` would look like clearing the password and
//! would in fact do nothing, which is the worst of both readings. Values containing spaces or quotes
//! are double-quoted and escaped.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// The fields the firmware reads. Names must match its own table exactly.
///
/// There is deliberately **no robot-address field**. The firmware derives the robot's address from
/// `TEAM`, so offering one would be a second source of truth for something that already has one — and
/// a way for the two to disagree.
#[derive(Deserialize, Default, Clone)]
pub struct Fields {
    #[serde(default)]
    pub wifi_ssid: String,
    #[serde(default)]
    pub wifi_pass: String,
    /// 1–99999, as text so an empty box means "do not write this" rather than zero.
    #[serde(default)]
    pub team: String,
    #[serde(default)]
    pub openai_api_key: String,
    #[serde(default)]
    pub anthropic_api_key: String,
    #[serde(default)]
    pub tba_api_key: String,
    #[serde(default)]
    pub nexus_api_key: String,
    #[serde(default)]
    pub ha_url: String,
    #[serde(default)]
    pub ha_token: String,
    #[serde(default)]
    pub frc_events_user: String,
    #[serde(default)]
    pub frc_events_token: String,
}

#[derive(Serialize)]
pub struct Written {
    pub path: String,
    pub keys: Vec<String>,
    pub bytes: usize,
}

impl Fields {
    fn pairs(&self) -> Vec<(&'static str, &str)> {
        let all: [(&'static str, &str); 11] = [
            ("WIFI_SSID", &self.wifi_ssid),
            ("WIFI_PASS", &self.wifi_pass),
            ("TEAM", &self.team),
            ("OPENAI_API_KEY", &self.openai_api_key),
            ("ANTHROPIC_API_KEY", &self.anthropic_api_key),
            ("TBA_API_KEY", &self.tba_api_key),
            ("NEXUS_API_KEY", &self.nexus_api_key),
            ("HA_URL", &self.ha_url),
            ("HA_TOKEN", &self.ha_token),
            ("FRC_EVENTS_USER", &self.frc_events_user),
            ("FRC_EVENTS_TOKEN", &self.frc_events_token),
        ];
        all.into_iter().filter(|(_, v)| !v.trim().is_empty()).collect()
    }

    /// Validate what can be validated without the tablet.
    pub fn problem(&self) -> Option<String> {
        if let Some(team) = Some(self.team.trim()).filter(|t| !t.is_empty()) {
            match team.parse::<u32>() {
                Ok(n) if (1..=99999).contains(&n) => {}
                _ => return Some(format!("team number must be 1-99999, not \"{team}\"")),
            }
        }
        if !self.wifi_pass.trim().is_empty() && self.wifi_ssid.trim().is_empty() {
            return Some("a Wi-Fi password with no SSID cannot be used; give the network name too".into());
        }
        if self.pairs().is_empty() {
            return Some("nothing to write - every field is empty".into());
        }
        None
    }

    /// The file's contents.
    pub fn render(&self) -> String {
        let mut out = String::new();
        out.push_str("# Written by Catalyst Link. The tablet reads this once, applies it, then zeroes\n");
        out.push_str("# and deletes it. If it is still here after a boot, the tablet never saw the card.\n");
        for (name, value) in self.pairs() {
            out.push_str(name);
            out.push('=');
            out.push_str(&quote_if_needed(value.trim()));
            out.push('\n');
        }
        out
    }
}

/// Quote only when the value would otherwise be misread. An unquoted value is easier for a person to
/// check by eye, and most keys need no quoting.
fn quote_if_needed(value: &str) -> String {
    let needs = value.is_empty()
        || value.contains(' ')
        || value.contains('"')
        || value.contains('\'')
        || value.contains('#')
        || value.contains('\\');
    if !needs {
        return value.to_string();
    }
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for c in value.chars() {
        if c == '"' || c == '\\' {
            out.push('\\');
        }
        out.push(c);
    }
    out.push('"');
    out
}

/// Write `KEYS.ENV` onto a card.
///
/// `root` is the card's drive or folder. `in_catos` picks `CATOS/KEYS.ENV` over `KEYS.ENV` at the root;
/// the firmware reads both, and `CATOS/` is the tidier of the two.
pub fn write(root: &Path, fields: &Fields, in_catos: bool) -> Result<Written, String> {
    if let Some(problem) = fields.problem() {
        return Err(problem);
    }
    if !root.is_dir() {
        return Err(format!("{} is not a folder - pick the card's drive", root.display()));
    }

    let dir: PathBuf = if in_catos { root.join("CATOS") } else { root.to_path_buf() };
    if in_catos && !dir.is_dir() {
        std::fs::create_dir_all(&dir).map_err(|e| format!("could not make {}: {e}", dir.display()))?;
    }
    let path = dir.join("KEYS.ENV");

    let body = fields.render();
    // Written whole, not appended: this file is a one-shot instruction, and appending to one the
    // tablet has not consumed yet would leave two sets of values in it.
    std::fs::write(&path, body.as_bytes())
        .map_err(|e| format!("could not write {}: {e}", path.display()))?;

    Ok(Written {
        path: path.display().to_string(),
        keys: fields.pairs().into_iter().map(|(k, _)| k.to_string()).collect(),
        bytes: body.len(),
    })
}

/// Whether a card at `root` still has an unconsumed `KEYS.ENV` — the only signal available that the
/// tablet has *not* read it. Its absence is not proof it was read; the card may simply be a different one.
pub fn pending(root: &Path) -> Option<String> {
    for candidate in [root.join("CATOS").join("KEYS.ENV"), root.join("KEYS.ENV")] {
        if candidate.is_file() {
            return Some(candidate.display().to_string());
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wifi(ssid: &str, pass: &str) -> Fields {
        Fields { wifi_ssid: ssid.into(), wifi_pass: pass.into(), ..Default::default() }
    }

    #[test]
    fn an_empty_field_is_omitted_rather_than_written_empty() {
        let f = wifi("pit-net", "");
        let text = f.render();
        assert!(text.contains("WIFI_SSID=pit-net"));
        assert!(
            !text.contains("WIFI_PASS"),
            "the firmware reads an empty value as 'leave alone', so writing one is misleading: {text}"
        );
    }

    #[test]
    fn values_needing_quotes_get_them_and_the_rest_do_not() {
        assert_eq!(quote_if_needed("simple"), "simple");
        assert_eq!(quote_if_needed("two words"), "\"two words\"");
        assert_eq!(quote_if_needed("has\"quote"), "\"has\\\"quote\"");
        assert_eq!(quote_if_needed("hash#inside"), "\"hash#inside\"");
        assert_eq!(quote_if_needed("back\\slash"), "\"back\\\\slash\"");
    }

    #[test]
    fn a_password_without_a_network_name_is_refused() {
        let f = wifi("", "secret");
        assert!(f.problem().unwrap().contains("SSID"));
    }

    #[test]
    fn a_team_number_outside_the_real_range_is_refused() {
        let mut f = Fields::default();
        f.team = "0".into();
        assert!(f.problem().is_some());
        f.team = "100000".into();
        assert!(f.problem().is_some());
        f.team = "abc".into();
        assert!(f.problem().is_some());
        f.team = "5805".into();
        assert!(f.problem().is_none(), "a real team number must be accepted");
    }

    #[test]
    fn writing_nothing_is_an_error_not_an_empty_file() {
        let f = Fields::default();
        assert!(f.problem().unwrap().contains("nothing to write"));
    }

    #[test]
    fn the_file_says_how_to_tell_whether_the_tablet_read_it() {
        let text = wifi("pit-net", "hunter2").render();
        assert!(
            text.contains("never saw the card"),
            "the file is the only place this can explain itself once the app is closed"
        );
    }
}
