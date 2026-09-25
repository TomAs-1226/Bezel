//! Runs the Link: `python -m catalyst_link serve --gui …` as a child process, watched.
//!
//! The Link stays exactly the Python program the CLI runs; this app starts it, reads its output into
//! the log panel (masked, see redact.rs), notices when it is up, restarts it if it falls over after
//! having run a while, and stops it with the app. A job object ties the child to this process, so even
//! a killed app never leaves a Link holding the port.
//!
//! If a Link is already answering on the port when the app starts (one started from a terminal, or at
//! login by Task Scheduler), the app attaches to it instead of starting a second: every panel works
//! through its HTTP API as before; only the log is unavailable, because that process's output isn't ours.

use std::collections::VecDeque;
use std::io::{BufRead, BufReader, Read};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::Serialize;

use crate::http;
use crate::redact::redact;
use crate::settings::{self, Settings};

const LOG_KEEP: usize = 1500;
const RESTART_AFTER_S: u64 = 20; // a Link that ran this long and then died is restarted...
const RESTART_MAX: u32 = 5; // ...this many times, then left for a human

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Phase {
    NeedsSetup,
    Stopped,
    Starting,
    Running,
    Attached,
    Exited,
    Failed,
}

impl Phase {
    fn word(self) -> &'static str {
        match self {
            Phase::NeedsSetup => "setup",
            Phase::Stopped => "stopped",
            Phase::Starting => "starting",
            Phase::Running => "running",
            Phase::Attached => "attached",
            Phase::Exited => "exited",
            Phase::Failed => "failed",
        }
    }
}

#[derive(Serialize, Clone)]
pub struct View {
    pub state: &'static str,
    pub message: String,
    pub pid: Option<u32>,
    pub port: u16,
    pub attached: bool,
    pub up: bool,
    pub since_s: f64,
    pub restarts: u32,
    pub command: String,
    pub link_dir: Option<String>,
}

#[derive(Serialize, Clone)]
pub struct LogLine {
    pub seq: u64,
    pub t: u64,
    pub text: String,
    pub stream: &'static str,
}

struct Inner {
    child: Option<Child>,
    phase: Phase,
    message: String,
    since: Instant,
    port: u16,
    wanted: bool,
    gen: u64,
    restarts: u32,
    command: String,
    link_dir: Option<PathBuf>,
    settings: Settings,
}

struct LogRing {
    lines: VecDeque<LogLine>,
    next: u64,
}

type Listener = Box<dyn Fn(&View) + Send + Sync>;

pub struct Supervisor {
    inner: Mutex<Inner>,
    log: Mutex<LogRing>,
    listener: Mutex<Option<Listener>>,
    #[cfg(windows)]
    job: Option<job::Job>,
}

impl Supervisor {
    pub fn new(settings: Settings) -> Arc<Self> {
        Arc::new(Self {
            inner: Mutex::new(Inner {
                child: None,
                phase: Phase::Stopped,
                message: String::new(),
                since: Instant::now(),
                port: settings.port,
                wanted: false,
                gen: 0,
                restarts: 0,
                command: String::new(),
                link_dir: None,
                settings,
            }),
            log: Mutex::new(LogRing { lines: VecDeque::new(), next: 1 }),
            listener: Mutex::new(None),
            #[cfg(windows)]
            job: job::Job::new(),
        })
    }

    /// Called (off the lock) whenever the phase changes: the tray's tooltip.
    pub fn on_change(&self, f: Listener) {
        *self.listener.lock().unwrap() = Some(f);
    }

    fn changed(&self) {
        let v = self.view();
        if let Some(f) = self.listener.lock().unwrap().as_ref() {
            f(&v);
        }
    }

    pub fn view(&self) -> View {
        let g = self.inner.lock().unwrap();
        View {
            state: g.phase.word(),
            message: g.message.clone(),
            pid: g.child.as_ref().map(|c| c.id()),
            port: g.port,
            attached: g.phase == Phase::Attached,
            up: matches!(g.phase, Phase::Running | Phase::Attached),
            since_s: g.since.elapsed().as_secs_f64(),
            restarts: g.restarts,
            command: g.command.clone(),
            link_dir: g.link_dir.as_ref().map(|p| p.display().to_string()),
        }
    }

    pub fn port(&self) -> u16 {
        self.inner.lock().unwrap().port
    }

    pub fn is_up(&self) -> bool {
        matches!(self.inner.lock().unwrap().phase, Phase::Running | Phase::Attached)
    }

    // --- the log ---------------------------------------------------------------------------------

    pub fn log_line(&self, text: &str, stream: &'static str) {
        let t = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
        let mut log = self.log.lock().unwrap();
        let seq = log.next;
        log.next += 1;
        log.lines.push_back(LogLine { seq, t, text: redact(text.trim_end()), stream });
        while log.lines.len() > LOG_KEEP {
            log.lines.pop_front();
        }
    }

    pub fn log_since(&self, after: u64) -> (Vec<LogLine>, u64) {
        let log = self.log.lock().unwrap();
        let lines = log.lines.iter().filter(|l| l.seq > after).cloned().collect();
        (lines, log.next - 1)
    }

    // --- start and stop --------------------------------------------------------------------------

    pub fn start(self: &Arc<Self>, settings: Settings) {
        {
            let mut g = self.inner.lock().unwrap();
            g.settings = settings.clone();
            g.port = settings.port;
            g.wanted = true;
            if g.child.is_some() {
                return;
            }
        }
        self.spawn_now(false);
    }

    fn set_phase(&self, g: &mut Inner, phase: Phase, message: impl Into<String>) {
        g.phase = phase;
        g.message = message.into();
        g.since = Instant::now();
    }

    fn spawn_now(self: &Arc<Self>, is_restart: bool) {
        let settings = self.inner.lock().unwrap().settings.clone();
        let port = settings.port;

        if settings.repo.trim().is_empty() {
            let mut g = self.inner.lock().unwrap();
            self.set_phase(&mut g, Phase::NeedsSetup, "choose the robot project in settings to start the link");
            drop(g);
            self.changed();
            return;
        }

        // Someone else's Link on the port: use it rather than fight it.
        if let Ok(r) = http::request(port, "GET", "/link/status", None, None, Duration::from_millis(600)) {
            if r.status == 200 && String::from_utf8_lossy(&r.body).contains("\"name\"") {
                let mut g = self.inner.lock().unwrap();
                g.gen += 1;
                let gen = g.gen;
                self.set_phase(&mut g, Phase::Attached, format!("a link started outside this app is on :{port}; using it"));
                g.command.clear();
                drop(g);
                self.log_line(&format!("a link is already answering on :{port}; attached to it (its log isn't visible here)"), "app");
                self.changed();
                let me = Arc::clone(self);
                std::thread::spawn(move || me.watch_attached(gen));
                return;
            }
        }

        let link_dir = settings::link_dir(&settings);
        let mut args: Vec<String> = vec!["-u".into(), "-m".into(), "catalyst_link".into(), "serve".into(),
            "--repo".into(), settings.repo.trim().into(), "--port".into(), port.to_string(), "--gui".into()];
        if !settings.media { args.push("--no-media".into()); }
        if !settings.pairing { args.push("--no-pair".into()); }
        if !settings.toast { args.push("--no-pair-toast".into()); }
        if !settings.mdns { args.push("--no-mdns".into()); }
        if !settings.name.trim().is_empty() {
            args.push("--name".into());
            args.push(settings.name.trim().into());
        }
        let python = if settings.python.trim().is_empty() { "python".to_string() } else { settings.python.trim().to_string() };
        let shown = format!("{python} {}", args.iter().map(|a| if a.contains(' ') { format!("\"{a}\"") } else { a.clone() }).collect::<Vec<_>>().join(" "));

        let mut cmd = Command::new(&python);
        cmd.args(&args)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .env("PYTHONIOENCODING", "utf-8")
            .env("PYTHONUNBUFFERED", "1");
        if let Some(dir) = &link_dir {
            cmd.current_dir(dir);
            let mut pp = dir.display().to_string();
            if let Some(old) = std::env::var_os("PYTHONPATH") {
                pp.push(if cfg!(windows) { ';' } else { ':' });
                pp.push_str(&old.to_string_lossy());
            }
            cmd.env("PYTHONPATH", pp);
        }
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console flashes up
        }

        self.log_line(&format!("{}starting: {shown}", if is_restart { "re" } else { "" }), "app");
        match cmd.spawn() {
            Err(e) => {
                let mut g = self.inner.lock().unwrap();
                g.command = shown;
                g.link_dir = link_dir;
                self.set_phase(&mut g, Phase::Failed, format!("couldn't run {python}: {e} (settings › python)"));
                drop(g);
                self.log_line(&format!("couldn't run {python}: {e}"), "app");
                self.changed();
            }
            Ok(mut child) => {
                #[cfg(windows)]
                if let Some(job) = &self.job {
                    job.assign(&child);
                }
                let out = child.stdout.take();
                let err = child.stderr.take();
                let gen;
                {
                    let mut g = self.inner.lock().unwrap();
                    g.gen += 1;
                    gen = g.gen;
                    g.child = Some(child);
                    g.command = shown;
                    g.link_dir = link_dir;
                    self.set_phase(&mut g, Phase::Starting, format!("starting on :{port}"));
                }
                if let Some(o) = out { self.pump(o, "out"); }
                if let Some(e) = err { self.pump(e, "err"); }
                self.changed();
                let me = Arc::clone(self);
                std::thread::spawn(move || me.watch(gen));
            }
        }
    }

    fn pump<R: Read + Send + 'static>(self: &Arc<Self>, r: R, stream: &'static str) {
        let me = Arc::clone(self);
        std::thread::spawn(move || {
            let mut reader = BufReader::new(r);
            let mut buf = Vec::new();
            loop {
                buf.clear();
                match reader.read_until(b'\n', &mut buf) {
                    Ok(0) | Err(_) => break,
                    Ok(_) => me.log_line(&String::from_utf8_lossy(&buf), stream),
                }
            }
        });
    }

    /// Our child: is it up yet, has it exited, should it come back.
    fn watch(self: Arc<Self>, gen: u64) {
        let started = Instant::now();
        loop {
            std::thread::sleep(Duration::from_millis(300));
            let (port, starting) = {
                let mut g = self.inner.lock().unwrap();
                if g.gen != gen {
                    return;
                }
                let exited = match g.child.as_mut().map(|c| c.try_wait()) {
                    Some(Ok(Some(status))) => Some(status.code()),
                    Some(Err(_)) => Some(None),
                    Some(Ok(None)) => None,
                    None => return,
                };
                if let Some(code) = exited {
                    g.child = None;
                    let ran = started.elapsed().as_secs();
                    let why = match code {
                        Some(c) => format!("the link stopped (exit code {c})"),
                        None => "the link stopped".to_string(),
                    };
                    let retry = g.wanted && ran >= RESTART_AFTER_S && g.restarts < RESTART_MAX;
                    let hint = if g.phase == Phase::Starting { " before it was up: see the log" } else { "" };
                    let next = if g.phase == Phase::Starting { Phase::Failed } else { Phase::Exited };
                    self.set_phase(&mut g, next, format!("{why}{hint}"));
                    if retry {
                        g.restarts += 1;
                    }
                    drop(g);
                    self.log_line(&format!("{why}{}", if retry { "; restarting in 3 s" } else { "" }), "app");
                    self.changed();
                    if retry {
                        std::thread::sleep(Duration::from_secs(3));
                        let still = { let g = self.inner.lock().unwrap(); g.wanted && g.child.is_none() && g.gen == gen };
                        if still {
                            self.spawn_now(true);
                        }
                    }
                    return;
                }
                (g.port, g.phase == Phase::Starting)
            };
            if starting {
                let ok = http::request(port, "GET", "/link/status", None, None, Duration::from_millis(1500))
                    .map(|r| r.status == 200)
                    .unwrap_or(false);
                if ok {
                    let mut g = self.inner.lock().unwrap();
                    if g.gen == gen && g.phase == Phase::Starting {
                        self.set_phase(&mut g, Phase::Running, format!("serving on :{port}"));
                        drop(g);
                        self.changed();
                    }
                }
            }
        }
    }

    /// Someone else's Link: say so when it goes away, and start ours if we still want one.
    fn watch_attached(self: Arc<Self>, gen: u64) {
        loop {
            std::thread::sleep(Duration::from_secs(2));
            let port = {
                let g = self.inner.lock().unwrap();
                if g.gen != gen || g.phase != Phase::Attached {
                    return;
                }
                g.port
            };
            let up = http::request(port, "GET", "/link/status", None, None, Duration::from_millis(800))
                .map(|r| r.status == 200)
                .unwrap_or(false);
            if !up {
                let wanted = {
                    let mut g = self.inner.lock().unwrap();
                    if g.gen != gen {
                        return;
                    }
                    self.set_phase(&mut g, Phase::Stopped, "the link this app was attached to went away");
                    g.wanted
                };
                self.log_line("the link this app was attached to went away", "app");
                self.changed();
                if wanted {
                    self.spawn_now(false);
                }
                return;
            }
        }
    }

    pub fn stop(&self) {
        let child = {
            let mut g = self.inner.lock().unwrap();
            g.wanted = false;
            g.gen += 1;
            g.restarts = 0;
            let c = g.child.take();
            let msg = if g.phase == Phase::Attached { "detached (the other link keeps running)" } else { "stopped" };
            self.set_phase(&mut g, Phase::Stopped, msg);
            c
        };
        if let Some(mut c) = child {
            let _ = c.kill();
            let _ = c.wait();
            self.log_line("stopped the link", "app");
        }
        self.changed();
    }

    pub fn restart(self: &Arc<Self>, settings: Settings) {
        self.stop();
        {
            let mut g = self.inner.lock().unwrap();
            g.restarts = 0;
        }
        self.start(settings);
    }
}

#[cfg(windows)]
mod job {
    //! A job object with "kill on close": the app holds the only handle, so when the app goes away, by
    //! any means, Windows ends the Link with it. SILENT_BREAKAWAY_OK lets what the Link itself starts
    //! (a work-order hook, a compile check) live on as it would from a terminal.
    use std::os::windows::io::AsRawHandle;
    use std::process::Child;
    use windows_sys::Win32::System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject,
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK,
    };

    pub struct Job(usize);

    impl Job {
        pub fn new() -> Option<Job> {
            unsafe {
                let h = CreateJobObjectW(std::ptr::null(), std::ptr::null());
                if h.is_null() {
                    return None;
                }
                let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
                info.BasicLimitInformation.LimitFlags =
                    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
                let ok = SetInformationJobObject(
                    h,
                    JobObjectExtendedLimitInformation,
                    &info as *const _ as *const core::ffi::c_void,
                    std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                );
                if ok == 0 {
                    return None;
                }
                Some(Job(h as usize))
            }
        }

        pub fn assign(&self, child: &Child) {
            unsafe {
                AssignProcessToJobObject(self.0 as _, child.as_raw_handle() as _);
            }
        }
    }
}
