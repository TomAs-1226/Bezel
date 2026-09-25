//! The app's settings, and where the Link's own state lives.
//!
//! Settings are a JSON file in the app's config folder (`%APPDATA%\com.frccatalyst.link\settings.json`;
//! `CATALYST_LINK_DESKTOP_SETTINGS` names another file). A missing or broken file falls back to the
//! defaults rather than failing the launch.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const IDENTIFIER: &str = "com.frccatalyst.link";

#[derive(Serialize, Deserialize, Clone, PartialEq, Debug)]
#[serde(default)]
pub struct Settings {
    /// The robot project's git repo (`serve --repo`). Empty: the Link can't start until one is chosen.
    pub repo: String,
    pub port: u16,
    /// What the tablet shows; empty: the PC's hostname.
    pub name: String,
    pub media: bool,
    pub pairing: bool,
    /// Also show a pairing code as a Windows notification (the window shows it regardless).
    pub toast: bool,
    pub mdns: bool,
    pub start_with_windows: bool,
    pub start_minimized: bool,
    /// The Python that runs the Link.
    pub python: String,
    /// The folder holding `catalyst_link/`; empty: the one this app was built next to, else the
    /// pip-installed package.
    pub link_dir: String,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            repo: String::new(),
            port: 8765,
            name: String::new(),
            media: true,
            pairing: true,
            toast: true,
            mdns: true,
            start_with_windows: false,
            start_minimized: false,
            python: "python".into(),
            link_dir: String::new(),
        }
    }
}

impl Settings {
    /// Whether a change to these settings needs the Link restarted.
    pub fn link_changed(&self, other: &Settings) -> bool {
        self.repo != other.repo
            || self.port != other.port
            || self.name != other.name
            || self.media != other.media
            || self.pairing != other.pairing
            || self.toast != other.toast
            || self.mdns != other.mdns
            || self.python != other.python
            || self.link_dir != other.link_dir
    }
}

pub fn path() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("CATALYST_LINK_DESKTOP_SETTINGS") {
        return Some(PathBuf::from(p));
    }
    #[cfg(windows)]
    let base = std::env::var_os("APPDATA").map(PathBuf::from);
    #[cfg(not(windows))]
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")));
    base.map(|b| b.join(IDENTIFIER).join("settings.json"))
}

pub fn load() -> Settings {
    path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .and_then(|s| serde_json::from_str(s.trim_start_matches('\u{feff}')).ok())
        .unwrap_or_default()
}

pub fn save(s: &Settings) -> Result<(), String> {
    let p = path().ok_or("no config folder")?;
    if let Some(dir) = p.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let body = serde_json::to_string_pretty(s).map_err(|e| e.to_string())?;
    let tmp = p.with_extension("json.tmp");
    std::fs::write(&tmp, body + "\n").map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &p).map_err(|e| e.to_string())
}

/// The Link's state folder: `CATALYST_LINK_HOME`, else `~/.catalyst-link` (state.py's rule).
pub fn link_home() -> PathBuf {
    if let Some(h) = std::env::var_os("CATALYST_LINK_HOME") {
        return PathBuf::from(h);
    }
    let home = std::env::var_os("USERPROFILE")
        .or_else(|| std::env::var_os("HOME"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".catalyst-link")
}

/// The Link's main token, read fresh each time (the CLI may rotate it). It stays in this process: the
/// window never sees it.
pub fn token() -> Option<String> {
    let t = std::fs::read_to_string(link_home().join("token")).ok()?;
    let t = t.trim().to_string();
    (!t.is_empty()).then_some(t)
}

/// The folder with `catalyst_link/` in it: the setting, else the source tree this app was built from
/// (`tab5/link`, two levels above `src-tauri`), else None: rely on `pip install`.
pub fn link_dir(s: &Settings) -> Option<PathBuf> {
    let has_pkg = |p: &Path| p.join("catalyst_link").join("__init__.py").is_file();
    if !s.link_dir.trim().is_empty() {
        let p = PathBuf::from(s.link_dir.trim());
        return has_pkg(&p).then_some(p);
    }
    let built = Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("..");
    if has_pkg(&built) {
        return built.canonicalize().ok().map(strip_verbatim);
    }
    None
}

/// `\\?\C:\...` (what canonicalize returns on Windows) confuses Python's cwd handling; drop the prefix.
fn strip_verbatim(p: PathBuf) -> PathBuf {
    let s = p.to_string_lossy();
    match s.strip_prefix(r"\\?\") {
        Some(rest) => PathBuf::from(rest),
        None => p,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_partial_file_keeps_the_defaults() {
        let s: Settings = serde_json::from_str(r#"{"port": 9000}"#).unwrap();
        assert_eq!(s.port, 9000);
        assert!(s.media && s.pairing && !s.start_with_windows);
    }

    #[test]
    fn autostart_and_minimize_dont_restart_the_link() {
        let a = Settings::default();
        let mut b = a.clone();
        b.start_minimized = true;
        b.start_with_windows = true;
        assert!(!a.link_changed(&b));
        b.port = 9001;
        assert!(a.link_changed(&b));
    }
}
