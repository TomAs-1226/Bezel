//! Flashing the tablet's firmware, from the app instead of from a hand-typed command line.
//!
//! Until now, putting firmware on a Catalyst Tab meant reading `tab5/firmware/README.md`, installing
//! esptool, and typing a `write_flash` with four addresses in it. That is fine for whoever built the
//! image and wrong for everyone else, which is most people holding the tablet: the addresses are easy
//! to transpose, and the failure mode of getting one wrong is a tablet that does not boot.
//!
//! This is deliberately the smallest thing that removes that. It is **not** a second
//! [`crate::supervisor`]: that module watches a long-lived server and restarts it when it dies, and
//! neither behaviour makes sense here. A flash runs once, for about a minute, and a flash that fails
//! must stay failed and say why — retrying it automatically is how you turn one bad cable into a
//! half-written partition table.
//!
//! What it borrows from the supervisor, because those parts were right: piped stdio read line by line
//! on its own thread, and the Windows job object, so an app that is killed mid-flash does not leave an
//! esptool running against the tablet's serial port. The last part matters more here than there —
//! two esptools on one port is worse than none.
//!
//! **Verification is esptool's**, not ours. `write_flash` hashes what it wrote and reads it back,
//! printing `Hash of data verified.`; this watches for that line and reports a flash without it as
//! unverified. That is a stronger check than hashing the file on disk, which only proves the file was
//! not corrupt — it says nothing about what actually landed on the chip, which is the thing anyone
//! cares about.

use std::io::{BufRead, BufReader, Read};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};

use serde::Serialize;

use crate::settings::{self, Settings};

/// The chip in a Tab5. Not configurable: this app flashes Catalyst Tabs.
const CHIP: &str = "esp32p4";

/// esptool's default is slower; this is the rate `firmware/README.md` documents.
const BAUD: &str = "921600";

/// Espressif's USB vendor id, so the likely tablet can be marked in the port list rather than leaving
/// someone to guess which COM port is the one.
const ESPRESSIF_VID: u16 = 0x303A;

const LOG_KEEP: usize = 600;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Phase {
    /// Nothing has run.
    Idle,
    /// esptool is connecting, erasing or writing.
    Running,
    /// Finished, and esptool said it verified what it wrote.
    Verified,
    /// Finished with exit 0, but no `Hash of data verified.` line was seen.
    Unverified,
    /// Failed, or was cancelled.
    Failed,
}

#[derive(Serialize, Clone)]
pub struct View {
    pub phase: Phase,
    pub message: String,
    /// 0 to 100 while writing, else -1: a bar that has no number to show should show nothing rather
    /// than sit at zero, which reads as stalled.
    pub percent: i32,
    pub running: bool,
}

/// One image to write, and where it goes.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum Target {
    /// `catalyst-tab-merged.bin` at 0x0: bootloader, partition table and app in one file.
    ///
    /// **This erases NVS**, so the tablet forgets its Link pairing, its Wi-Fi and its settings, and
    /// has to be paired again. The UI must say so before it starts.
    Merged,
    /// `catalyst_tab.bin` at 0x10000: the app alone.
    ///
    /// Keeps NVS, so pairing and settings survive. The right choice for an update.
    AppOnly,
}

impl Target {
    fn file(self) -> &'static str {
        match self {
            Target::Merged => "catalyst-tab-merged.bin",
            Target::AppOnly => "catalyst_tab.bin",
        }
    }

    fn address(self) -> &'static str {
        match self {
            Target::Merged => "0x0",
            Target::AppOnly => "0x10000",
        }
    }

    /// Whether writing this wipes the tablet's stored settings and pairing.
    pub fn erases_settings(self) -> bool {
        matches!(self, Target::Merged)
    }
}

#[derive(Serialize, Clone)]
pub struct PortInfo {
    pub name: String,
    pub description: String,
    /// True when the USB vendor id is Espressif's — a strong hint, not a guarantee.
    pub likely_tablet: bool,
}

struct Inner {
    phase: Phase,
    message: String,
    percent: i32,
    log: std::collections::VecDeque<String>,
    child: Option<Child>,
    verified: bool,
    generation: u64,
}

pub struct Flasher {
    inner: Mutex<Inner>,
    on_change: Mutex<Option<Box<dyn Fn(View) + Send + Sync>>>,
    #[cfg(windows)]
    job: Option<crate::supervisor::JobHandle>,
}

impl Flasher {
    pub fn new() -> Arc<Flasher> {
        Arc::new(Flasher {
            inner: Mutex::new(Inner {
                phase: Phase::Idle,
                message: String::new(),
                percent: -1,
                log: std::collections::VecDeque::new(),
                child: None,
                verified: false,
                generation: 0,
            }),
            on_change: Mutex::new(None),
            #[cfg(windows)]
            job: crate::supervisor::JobHandle::new(),
        })
    }

    pub fn on_change(&self, f: impl Fn(View) + Send + Sync + 'static) {
        *self.on_change.lock().unwrap() = Some(Box::new(f));
    }

    pub fn view(&self) -> View {
        let g = self.inner.lock().unwrap();
        Self::view_of(&g)
    }

    fn view_of(g: &Inner) -> View {
        View {
            phase: g.phase,
            message: g.message.clone(),
            percent: g.percent,
            running: g.phase == Phase::Running,
        }
    }

    fn notify(&self, g: &Inner) {
        if let Some(f) = self.on_change.lock().unwrap().as_ref() {
            f(Self::view_of(g));
        }
    }

    /// Every log line since `after`, with the new cursor — the same shape the log panel already uses.
    pub fn log_since(&self, after: usize) -> (Vec<String>, usize) {
        let g = self.inner.lock().unwrap();
        let total = g.log.len();
        let from = after.min(total);
        (g.log.iter().skip(from).cloned().collect(), total)
    }

    fn push(self: &Arc<Self>, line: &str) {
        let text = line.trim_end_matches(['\r', '\n']).to_string();
        if text.is_empty() {
            return;
        }
        let mut g = self.inner.lock().unwrap();
        if g.log.len() >= LOG_KEEP {
            g.log.pop_front();
        }
        g.log.push_back(text.clone());

        // esptool's progress looks like "Writing at 0x0001e000... (37 %)".
        if let Some(pct) = parse_percent(&text) {
            g.percent = pct;
        }
        if text.contains("Hash of data verified") {
            g.verified = true;
        }
        drop(g);
        let g = self.inner.lock().unwrap();
        self.notify(&g);
    }

    fn set(self: &Arc<Self>, phase: Phase, message: impl Into<String>) {
        let mut g = self.inner.lock().unwrap();
        g.phase = phase;
        g.message = message.into();
        if phase != Phase::Running {
            g.percent = -1;
        }
        self.notify(&g);
    }

    /// The serial ports on this machine, likely tablet first.
    pub fn ports() -> Vec<PortInfo> {
        let mut out: Vec<PortInfo> = match serialport::available_ports() {
            Ok(ports) => ports
                .into_iter()
                .map(|p| {
                    let (description, likely) = match &p.port_type {
                        serialport::SerialPortType::UsbPort(usb) => {
                            let name = usb
                                .product
                                .clone()
                                .or_else(|| usb.manufacturer.clone())
                                .unwrap_or_else(|| "USB serial".into());
                            (name, usb.vid == ESPRESSIF_VID)
                        }
                        _ => ("serial port".to_string(), false),
                    };
                    PortInfo { name: p.port_name, description, likely_tablet: likely }
                })
                .collect(),
            Err(_) => Vec::new(),
        };
        out.sort_by_key(|p| (!p.likely_tablet, p.name.clone()));
        out
    }

    /// Whether the Python the app uses can import esptool, and a sentence to show if it cannot.
    pub fn esptool_available(settings: &Settings) -> Result<String, String> {
        let python = python_of(settings);
        let out = Command::new(&python)
            .args(["-c", "import esptool, sys; sys.stdout.write(getattr(esptool, '__version__', 'unknown'))"])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .no_window()
            .output()
            .map_err(|e| format!("could not run {python}: {e} (settings › python)"))?;
        if out.status.success() {
            Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
        } else {
            Err(format!(
                "{python} cannot import esptool. Install it with `python -m pip install \".[flash]\"` \
                 in tab5/link, or `python -m pip install esptool`."
            ))
        }
    }

    /// The firmware folder that ships with the source tree this app was built from.
    pub fn firmware_dir(settings: &Settings) -> Option<PathBuf> {
        // `link_dir` is `tab5/link`; the images are `tab5/firmware`.
        let link = settings::link_dir(settings)?;
        let dir = link.parent()?.join("firmware");
        dir.is_dir().then_some(dir)
    }

    /// Start a flash. Returns the command line that was run, for the log.
    pub fn start(
        self: &Arc<Self>,
        settings: &Settings,
        target: Target,
        port: Option<String>,
        image: Option<PathBuf>,
    ) -> Result<String, String> {
        {
            let g = self.inner.lock().unwrap();
            if g.phase == Phase::Running {
                return Err("a flash is already running".into());
            }
        }

        let image = match image {
            Some(p) => p,
            None => Self::firmware_dir(settings)
                .map(|d| d.join(target.file()))
                .ok_or_else(|| {
                    format!(
                        "no firmware folder found beside this app, so there is no {} to write. \
                         Pick an image file, or set settings › python › link folder.",
                        target.file()
                    )
                })?,
        };
        if !image.is_file() {
            return Err(format!("{} is not a file", image.display()));
        }

        Self::esptool_available(settings)?;

        let python = python_of(settings);
        let mut args: Vec<String> = vec![
            "-u".into(),
            "-m".into(),
            "esptool".into(),
            "--chip".into(),
            CHIP.into(),
            "-b".into(),
            BAUD.into(),
        ];
        if let Some(p) = port.as_deref().filter(|p| !p.trim().is_empty()) {
            args.push("-p".into());
            args.push(p.trim().into());
        }
        args.push("write_flash".into());
        args.push(target.address().into());
        args.push(image.display().to_string());

        let shown = format!(
            "{python} {}",
            args.iter()
                .map(|a| if a.contains(' ') { format!("\"{a}\"") } else { a.clone() })
                .collect::<Vec<_>>()
                .join(" ")
        );

        let mut cmd = Command::new(&python);
        cmd.args(&args)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .env("PYTHONIOENCODING", "utf-8")
            .env("PYTHONUNBUFFERED", "1")
            .no_window();

        let mut child = cmd.spawn().map_err(|e| format!("could not run {python}: {e}"))?;

        #[cfg(windows)]
        if let Some(job) = &self.job {
            job.assign(&child);
        }

        let stdout = child.stdout.take();
        let stderr = child.stderr.take();

        let generation = {
            let mut g = self.inner.lock().unwrap();
            g.generation += 1;
            g.verified = false;
            g.percent = -1;
            g.child = Some(child);
            g.generation
        };

        self.set(Phase::Running, "connecting to the tablet");
        self.push(&format!("$ {shown}"));

        if let Some(out) = stdout {
            self.pump(out);
        }
        if let Some(err) = stderr {
            self.pump(err);
        }

        let me = Arc::clone(self);
        std::thread::spawn(move || me.wait(generation, target));

        Ok(shown)
    }

    /// Stop a running flash.
    ///
    /// Interrupting a `write_flash` can leave a partially written partition, so the UI must say that
    /// and the message says it too — a cancel is not a no-op, it is a half-flashed tablet that needs
    /// a full merged write to recover.
    pub fn cancel(self: &Arc<Self>) {
        let mut g = self.inner.lock().unwrap();
        if let Some(child) = g.child.as_mut() {
            let _ = child.kill();
        }
        drop(g);
        self.set(
            Phase::Failed,
            "cancelled. A stopped write can leave the flash half-written — write the merged image to recover.",
        );
    }

    fn pump<R: Read + Send + 'static>(self: &Arc<Self>, r: R) {
        let me = Arc::clone(self);
        std::thread::spawn(move || {
            let mut reader = BufReader::new(r);
            let mut buf = Vec::new();
            loop {
                buf.clear();
                // read_until on b'\r' as well as b'\n', because esptool writes its progress with
                // carriage returns: reading whole lines only would show nothing until the write
                // finished, which is exactly the minute a progress bar is for.
                match read_until_either(&mut reader, &mut buf) {
                    Ok(0) | Err(_) => break,
                    Ok(_) => me.push(&String::from_utf8_lossy(&buf)),
                }
            }
        });
    }

    fn wait(self: Arc<Self>, generation: u64, target: Target) {
        let status = {
            let mut g = self.inner.lock().unwrap();
            if g.generation != generation {
                return;
            }
            match g.child.as_mut() {
                Some(child) => child.wait(),
                None => return,
            }
        };

        {
            let mut g = self.inner.lock().unwrap();
            if g.generation != generation {
                return;
            }
            g.child = None;
        }

        let verified = self.inner.lock().unwrap().verified;
        match status {
            Ok(s) if s.success() && verified => {
                let extra = if target.erases_settings() {
                    " The tablet's settings and Link pairing were erased; pair it again."
                } else {
                    ""
                };
                self.set(Phase::Verified, format!("written and verified.{extra}"));
            }
            Ok(s) if s.success() => self.set(
                Phase::Unverified,
                "esptool finished without error but never said it verified the data. \
                 Treat this as unflashed and try again.",
            ),
            Ok(s) => self.set(
                Phase::Failed,
                format!(
                    "esptool exited {}. The usual causes: the tablet is on its USB-A port (which cannot \
                     flash — use USB-C), the port is held by something else, or it is not in download mode.",
                    s.code().map(|c| c.to_string()).unwrap_or_else(|| "abnormally".into())
                ),
            ),
            Err(e) => self.set(Phase::Failed, format!("could not wait for esptool: {e}")),
        }
    }
}

fn python_of(s: &Settings) -> String {
    if s.python.trim().is_empty() {
        "python".to_string()
    } else {
        s.python.trim().to_string()
    }
}

/// `Writing at 0x0001e000... (37 %)` -> `Some(37)`.
fn parse_percent(line: &str) -> Option<i32> {
    let open = line.rfind('(')?;
    let close = line[open..].find('%')?;
    line[open + 1..open + close].trim().parse::<i32>().ok().filter(|p| (0..=100).contains(p))
}

/// Read to the next `\n` or `\r`, whichever comes first.
fn read_until_either<R: BufRead>(r: &mut R, buf: &mut Vec<u8>) -> std::io::Result<usize> {
    let mut total = 0;
    loop {
        let (done, used) = {
            let available = match r.fill_buf() {
                Ok(b) if b.is_empty() => return Ok(total),
                Ok(b) => b,
                Err(ref e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(e),
            };
            match available.iter().position(|&b| b == b'\n' || b == b'\r') {
                Some(i) => {
                    buf.extend_from_slice(&available[..=i]);
                    (true, i + 1)
                }
                None => {
                    buf.extend_from_slice(available);
                    (false, available.len())
                }
            }
        };
        r.consume(used);
        total += used;
        if done || used == 0 {
            return Ok(total);
        }
    }
}

/// `CREATE_NO_WINDOW`, so flashing does not flash a console up. Mirrors what the supervisor does.
trait NoWindow {
    fn no_window(&mut self) -> &mut Self;
}

impl NoWindow for Command {
    #[cfg(windows)]
    fn no_window(&mut self) -> &mut Self {
        use std::os::windows::process::CommandExt;
        self.creation_flags(0x0800_0000)
    }
    #[cfg(not(windows))]
    fn no_window(&mut self) -> &mut Self {
        self
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn progress_is_read_out_of_esptools_own_line_shape() {
        assert_eq!(parse_percent("Writing at 0x0001e000... (37 %)"), Some(37));
        assert_eq!(parse_percent("Writing at 0x00010000... (100 %)"), Some(100));
        assert_eq!(parse_percent("Writing at 0x00000000... (0 %)"), Some(0));
    }

    #[test]
    fn a_line_without_a_percentage_is_not_progress() {
        assert_eq!(parse_percent("Connecting...."), None);
        assert_eq!(parse_percent("Hash of data verified."), None);
        assert_eq!(parse_percent(""), None);
        // A percentage outside 0-100 is a parse that went wrong, not a reading.
        assert_eq!(parse_percent("something (900 %)"), None);
    }

    #[test]
    fn the_merged_image_is_the_one_that_erases_settings() {
        assert!(Target::Merged.erases_settings(), "a merged write clears NVS, so pairing is lost");
        assert!(!Target::AppOnly.erases_settings(), "an app-only write must keep pairing");
        assert_eq!(Target::Merged.address(), "0x0");
        assert_eq!(Target::AppOnly.address(), "0x10000");
    }

    #[test]
    fn progress_lines_split_on_carriage_returns_as_well_as_newlines() {
        // esptool separates progress updates with \r; splitting only on \n would show nothing until
        // the write finished.
        let data = b"Connecting...\rWriting at 0x0... (5 %)\rdone\n";
        let mut reader = std::io::BufReader::new(&data[..]);
        let mut lines = Vec::new();
        loop {
            let mut buf = Vec::new();
            match read_until_either(&mut reader, &mut buf) {
                Ok(0) | Err(_) => break,
                Ok(_) => lines.push(String::from_utf8_lossy(&buf).trim().to_string()),
            }
        }
        assert_eq!(lines, vec!["Connecting...", "Writing at 0x0... (5 %)", "done"]);
    }
}
