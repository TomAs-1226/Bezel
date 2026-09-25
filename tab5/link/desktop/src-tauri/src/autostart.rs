//! "Start with Windows": a value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
//!
//! Per-user, no admin rights, and exactly what Settings › Apps › Startup lists and can switch off. The
//! registry is the truth: the toggle reads it back rather than trusting what was last saved, so a user
//! who turned it off in Task Manager (which marks it disabled under `...\Explorer\StartupApproved\Run`
//! instead of deleting it) sees it off here too, and turning it on here clears that mark.

pub const VALUE: &str = "Catalyst Link";

#[cfg(windows)]
mod imp {
    use super::VALUE;
    use winreg::enums::{HKEY_CURRENT_USER, KEY_READ, KEY_SET_VALUE};
    use winreg::RegKey;

    const RUN: &str = r"Software\Microsoft\Windows\CurrentVersion\Run";
    const APPROVED: &str = r"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";

    /// Task Manager's switch: a binary value whose first byte is odd (3) when the entry is disabled.
    fn disabled_in_task_manager() -> bool {
        RegKey::predef(HKEY_CURRENT_USER)
            .open_subkey_with_flags(APPROVED, KEY_READ)
            .and_then(|k| k.get_raw_value(VALUE))
            .map(|v| v.bytes.first().is_some_and(|b| b & 1 == 1))
            .unwrap_or(false)
    }

    pub fn command() -> Result<String, String> {
        let exe = std::env::current_exe().map_err(|e| e.to_string())?;
        Ok(format!("\"{}\" --autostart", exe.display()))
    }

    pub fn enabled() -> bool {
        RegKey::predef(HKEY_CURRENT_USER)
            .open_subkey_with_flags(RUN, KEY_READ)
            .and_then(|k| k.get_value::<String, _>(VALUE))
            .is_ok()
            && !disabled_in_task_manager()
    }

    pub fn set(on: bool) -> Result<(), String> {
        let (key, _) = RegKey::predef(HKEY_CURRENT_USER)
            .create_subkey(RUN)
            .map_err(|e| e.to_string())?;
        if on {
            if let Ok(approved) = RegKey::predef(HKEY_CURRENT_USER).open_subkey_with_flags(APPROVED, KEY_SET_VALUE) {
                let _ = approved.delete_value(VALUE); // no mark: enabled
            }
            key.set_value(VALUE, &command()?).map_err(|e| e.to_string())
        } else {
            match key.delete_value(VALUE) {
                Ok(()) => Ok(()),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(e) => Err(e.to_string()),
            }
        }
    }
}

#[cfg(not(windows))]
mod imp {
    pub fn enabled() -> bool {
        false
    }
    pub fn set(_on: bool) -> Result<(), String> {
        Err("start with the OS is Windows-only for now".into())
    }
}

pub use imp::{enabled, set};
