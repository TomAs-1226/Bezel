// Catalyst Link — the desktop app.
//
// The Link is the PC side of Catalyst Tab: a Python HTTP server (tab5/link/catalyst_link) that the
// tablet talks to. This app is a window and a tray icon around it, built the way Catalyst Console is
// (Tauri 2, plain HTML/CSS/JS, the Catalyst identity): it runs the Link as a watched child process,
// and every panel reads and acts through the Link's own HTTP API on 127.0.0.1.
//
// Two rules the shape answers to:
//
// 1. **The token never reaches the window.** The Link's main token is read from its state folder by
//    this Rust side, which makes every request; the page asks for data, never for credentials. The
//    only secret the window ever shows is a pairing code, and only in the pairing panel.
// 2. **The Link stays the Link.** `python -m catalyst_link serve` still runs headless exactly as
//    before; this app adds nothing the CLI lacks except a window. Closing the window hides it; the
//    Link keeps serving the tablet until "quit" in the tray.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod autostart;
mod flash;
mod http;
mod provision;
mod recordings;
mod redact;
mod settings;
mod supervisor;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde::Serialize;
use serde_json::Value;
use tauri::menu::{Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Emitter, Manager, RunEvent, State, WindowEvent};

use settings::Settings;
use supervisor::{LogLine, Supervisor, View};

static QUITTING: AtomicBool = AtomicBool::new(false);

struct App {
    link: Arc<Supervisor>,
    flash: Arc<flash::Flasher>,
    settings: Mutex<Settings>,
}

// --- the window's commands --------------------------------------------------------------------------

#[derive(Serialize)]
struct Info {
    version: &'static str,
    settings_path: Option<String>,
    link_home: String,
    has_token: bool,
}

#[tauri::command]
fn app_info() -> Info {
    Info {
        version: env!("CARGO_PKG_VERSION"),
        settings_path: settings::path().map(|p| p.display().to_string()),
        link_home: settings::link_home().display().to_string(),
        has_token: settings::token().is_some(),
    }
}

#[tauri::command]
fn link_state(app: State<'_, App>) -> View {
    app.link.view()
}

#[tauri::command]
async fn link_start(app: State<'_, App>) -> Result<View, String> {
    let s = app.settings.lock().unwrap().clone();
    app.link.start(s);
    Ok(app.link.view())
}

#[tauri::command]
async fn link_stop(app: State<'_, App>) -> Result<View, String> {
    app.link.stop();
    Ok(app.link.view())
}

#[tauri::command]
async fn link_restart(app: State<'_, App>) -> Result<View, String> {
    let s = app.settings.lock().unwrap().clone();
    app.link.restart(s);
    Ok(app.link.view())
}

#[derive(Serialize)]
struct LogPage {
    lines: Vec<LogLine>,
    last: u64,
}

#[tauri::command]
fn link_log(app: State<'_, App>, after: u64) -> LogPage {
    let (lines, last) = app.link.log_since(after);
    LogPage { lines, last }
}

/// The routes the window may reach, all on the Link on this PC. Nothing else: the page can't point
/// this at another host or at a route that isn't for it.
const ALLOWED: &[&str] = &["/admin/", "/inbox", "/media/", "/v1/claude/sessions", "/link/status", "/code/patches"];

#[derive(Serialize)]
struct ApiAnswer {
    status: u16,
    body: Value,
}

#[tauri::command]
async fn link_api(app: State<'_, App>, method: String, path: String, body: Option<Value>) -> Result<ApiAnswer, String> {
    let method = method.to_ascii_uppercase();
    if method != "GET" && method != "POST" {
        return Err("GET or POST only".into());
    }
    if !path.starts_with('/') || path.contains("..") || !ALLOWED.iter().any(|p| path.starts_with(p)) {
        return Err(format!("not a route for the window: {path}"));
    }
    let port = app.link.port();
    let token = settings::token();
    let raw = body.map(|b| serde_json::to_vec(&b).unwrap_or_default());
    let payload = if method == "POST" { Some(raw.as_deref().unwrap_or(b"{}")) } else { None };
    let r = http::request(port, &method, &path, token.as_deref(), payload, Duration::from_secs(8))?;
    let body = serde_json::from_slice(&r.body).unwrap_or(Value::Null);
    Ok(ApiAnswer { status: r.status, body })
}

#[derive(Serialize)]
struct SettingsView {
    #[serde(flatten)]
    settings: Settings,
    link_dir_found: Option<String>,
}

fn settings_view(s: &Settings) -> SettingsView {
    let mut s = s.clone();
    s.start_with_windows = autostart::enabled(); // the registry is the truth
    SettingsView { link_dir_found: settings::link_dir(&s).map(|p| p.display().to_string()), settings: s }
}

#[tauri::command]
fn get_settings(app: State<'_, App>) -> SettingsView {
    settings_view(&app.settings.lock().unwrap())
}

#[derive(Serialize)]
struct Saved {
    settings: SettingsView,
    restarted: bool,
    autostart_error: Option<String>,
}

#[tauri::command]
async fn save_settings(app: State<'_, App>, settings: Settings) -> Result<Saved, String> {
    if settings.port < 1024 {
        return Err("the port must be 1024 or higher".into());
    }
    let old = app.settings.lock().unwrap().clone();
    settings::save(&settings)?;
    *app.settings.lock().unwrap() = settings.clone();
    let autostart_error = if settings.start_with_windows != autostart::enabled() {
        autostart::set(settings.start_with_windows).err()
    } else {
        None
    };
    let restarted = old.link_changed(&settings) || matches!(app.link.view().state, "setup");
    if restarted {
        app.link.restart(settings.clone());
    }
    Ok(Saved { settings: settings_view(&settings), restarted, autostart_error })
}

#[tauri::command]
async fn pick_folder(app: AppHandle) -> Option<String> {
    use tauri_plugin_dialog::DialogExt;
    app.dialog()
        .file()
        .set_title("the robot project's git repo")
        .blocking_pick_folder()
        .and_then(|p| p.into_path().ok())
        .map(|p| p.display().to_string())
}

/// Open a work order's file in the PC's editor. Only files inside the Link's inbox.
#[tauri::command]
fn open_work_order(app: AppHandle, path: String) -> Result<(), String> {
    use tauri_plugin_opener::OpenerExt;
    let inbox = settings::link_home().join("inbox");
    let inbox = inbox.canonicalize().map_err(|e| e.to_string())?;
    let file = std::path::Path::new(&path).canonicalize().map_err(|e| e.to_string())?;
    if !file.starts_with(&inbox) || file.extension().and_then(|e| e.to_str()) != Some("md") {
        return Err("only work orders in the Link's inbox".into());
    }
    app.opener().open_path(file.display().to_string(), None::<&str>).map_err(|e| e.to_string())
}

// --- flashing the tablet ----------------------------------------------------------------------------

#[derive(Serialize)]
struct FlashReady {
    esptool: Option<String>,
    problem: Option<String>,
    firmware_dir: Option<String>,
    ports: Vec<flash::PortInfo>,
}

/// Everything the flash panel needs to decide whether it can offer the button.
#[tauri::command]
fn flash_ready(state: State<'_, App>) -> FlashReady {
    let settings = state.settings.lock().unwrap().clone();
    let (esptool, problem) = match flash::Flasher::esptool_available(&settings) {
        Ok(v) => (Some(v), None),
        Err(e) => (None, Some(e)),
    };
    FlashReady {
        esptool,
        problem,
        firmware_dir: flash::Flasher::firmware_dir(&settings).map(|p| p.display().to_string()),
        ports: flash::Flasher::ports(),
    }
}

#[tauri::command]
fn flash_start(
    state: State<'_, App>,
    merged: bool,
    port: Option<String>,
    image: Option<String>,
) -> Result<String, String> {
    let settings = state.settings.lock().unwrap().clone();
    let target = if merged { flash::Target::Merged } else { flash::Target::AppOnly };
    state.flash.start(&settings, target, port, image.map(std::path::PathBuf::from))
}

#[tauri::command]
fn flash_cancel(state: State<'_, App>) {
    state.flash.cancel();
}

#[tauri::command]
fn flash_state(state: State<'_, App>) -> flash::View {
    state.flash.view()
}

#[derive(Serialize)]
struct FlashLog {
    lines: Vec<String>,
    last: usize,
}

#[tauri::command]
fn flash_log(state: State<'_, App>, after: usize) -> FlashLog {
    let (lines, last) = state.flash.log_since(after);
    FlashLog { lines, last }
}

/// Pick a firmware image by hand, for a build that is not the one beside the app.
#[tauri::command]
async fn flash_pick_image(app: AppHandle) -> Option<String> {
    use tauri_plugin_dialog::DialogExt;
    app.dialog()
        .file()
        .set_title("a firmware image to write")
        .add_filter("firmware", &["bin"])
        .blocking_pick_file()
        .and_then(|p| p.into_path().ok())
        .map(|p| p.display().to_string())
}

// --- provisioning the tablet's card -----------------------------------------------------------------

#[tauri::command]
async fn provision_pick_card(app: AppHandle) -> Option<String> {
    use tauri_plugin_dialog::DialogExt;
    app.dialog()
        .file()
        .set_title("the tablet's microSD card")
        .blocking_pick_folder()
        .and_then(|p| p.into_path().ok())
        .map(|p| p.display().to_string())
}

#[tauri::command]
fn provision_write(
    card: String,
    fields: provision::Fields,
    in_catos: bool,
) -> Result<provision::Written, String> {
    provision::write(std::path::Path::new(&card), &fields, in_catos)
}

/// Whether a card already carries a KEYS.ENV the tablet has not consumed.
#[tauri::command]
fn provision_pending(card: String) -> Option<String> {
    provision::pending(std::path::Path::new(&card))
}

// --- the runs the tablet uploaded -------------------------------------------------------------------

#[tauri::command]
fn runs_list() -> Vec<recordings::FileEntry> {
    recordings::list()
}

#[tauri::command]
fn runs_summary(name: String) -> Result<recordings::RunSummary, String> {
    recordings::summary(&name)
}

/// Open an uploaded file in whatever the PC uses for it. Only files inside the Link's own folder.
#[tauri::command]
fn runs_open(app: AppHandle, name: String) -> Result<(), String> {
    use tauri_plugin_opener::OpenerExt;
    let path = recordings::path_of(&name)?;
    app.opener().open_path(path, None::<&str>).map_err(|e| e.to_string())
}

#[tauri::command]
fn quit(app: AppHandle) {
    QUITTING.store(true, Ordering::SeqCst);
    app.exit(0);
}

// --- the window and the tray ------------------------------------------------------------------------

fn show_main(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.unminimize();
        let _ = w.show();
        let _ = w.set_focus();
    }
}

fn navigate(app: &AppHandle, panel: &str) {
    show_main(app);
    let _ = app.emit("navigate", panel);
}

fn tooltip(v: &View) -> String {
    match v.state {
        "running" => format!("Catalyst Link — serving on :{}", v.port),
        "attached" => format!("Catalyst Link — using the link on :{}", v.port),
        "setup" => "Catalyst Link — choose the robot project".into(),
        other => format!("Catalyst Link — {other}"),
    }
}

fn build_tray(app: &AppHandle) -> tauri::Result<()> {
    let show = MenuItem::with_id(app, "show", "Open Catalyst Link", true, None::<&str>)?;
    let pair = MenuItem::with_id(app, "pair", "Pair a tablet…", true, None::<&str>)?;
    let restart = MenuItem::with_id(app, "restart", "Restart the link", true, None::<&str>)?;
    let quit = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
    let sep = PredefinedMenuItem::separator(app)?;
    let menu = Menu::with_items(app, &[&show, &pair, &restart, &sep, &quit])?;
    let mut builder = TrayIconBuilder::with_id("main")
        .tooltip("Catalyst Link")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .on_menu_event(|app, event| match event.id.as_ref() {
            "show" => show_main(app),
            "pair" => navigate(app, "pairing"),
            "restart" => {
                let state = app.state::<App>();
                let s = state.settings.lock().unwrap().clone();
                let link = Arc::clone(&state.link);
                std::thread::spawn(move || link.restart(s));
            }
            "quit" => {
                QUITTING.store(true, Ordering::SeqCst);
                app.exit(0);
            }
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = event {
                show_main(tray.app_handle());
            }
        });
    if let Some(icon) = app.default_window_icon() {
        builder = builder.icon(icon.clone());
    }
    builder.build(app)?;
    Ok(())
}

/// A tablet asked to pair: bring the window up on the pairing panel, wherever it was. The code itself
/// stays in the Link until the panel asks for it.
fn watch_pairing(app: AppHandle, link: Arc<Supervisor>) {
    std::thread::spawn(move || {
        let mut last_pending: Option<String> = None;
        loop {
            std::thread::sleep(Duration::from_millis(900));
            if !link.is_up() {
                continue;
            }
            let Some(token) = settings::token() else { continue };
            let Ok(r) = http::request(link.port(), "GET", "/admin/pairing", Some(&token), None, Duration::from_secs(2)) else {
                continue;
            };
            let Ok(v) = serde_json::from_slice::<Value>(&r.body) else { continue };
            let pending = v.get("pending").filter(|p| !p.is_null());
            let id = pending.and_then(|p| p.get("id")).and_then(|i| i.as_str()).map(str::to_string);
            if id.is_some() && id != last_pending {
                let device = pending.and_then(|p| p.get("device")).and_then(|d| d.as_str()).unwrap_or("a tablet");
                let _ = app.emit("pairing-request", device);
                navigate(&app, "pairing");
            }
            last_pending = id;
        }
    });
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let force_minimized = args.iter().any(|a| a == "--minimized");
    let initial = settings::load();
    let link = Supervisor::new(initial.clone());
    let flasher = flash::Flasher::new();

    let app = tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| show_main(app)))
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage(App { link: Arc::clone(&link), flash: Arc::clone(&flasher), settings: Mutex::new(initial.clone()) })
        .invoke_handler(tauri::generate_handler![
            app_info,
            link_state,
            link_start,
            link_stop,
            link_restart,
            link_log,
            link_api,
            get_settings,
            save_settings,
            pick_folder,
            open_work_order,
            flash_ready,
            flash_start,
            flash_cancel,
            flash_state,
            flash_log,
            flash_pick_image,
            provision_pick_card,
            provision_write,
            provision_pending,
            runs_list,
            runs_summary,
            runs_open,
            quit
        ])
        .on_window_event(|window, event| {
            // Closing the window hides it: the tablet still needs the Link. "Quit" is in the tray.
            if let WindowEvent::CloseRequested { api, .. } = event {
                if !QUITTING.load(Ordering::SeqCst) {
                    api.prevent_close();
                    let _ = window.hide();
                }
            }
        })
        .setup(move |app| {
            build_tray(app.handle())?;
            let handle = app.handle().clone();
            link.on_change(Box::new(move |v: &View| {
                if let Some(tray) = handle.tray_by_id("main") {
                    let _ = tray.set_tooltip(Some(tooltip(v)));
                }
                let _ = handle.emit("link-state", v.clone());
            }));
            // The flash panel is pushed, not polled: a write takes about a minute and esptool's
            // progress arrives many times a second, which a one-second poll would render as a bar
            // that lurches. The same mechanism as link-state, for the same reason.
            let flash_handle = app.handle().clone();
            flasher.on_change(move |v: flash::View| {
                let _ = flash_handle.emit("flash-state", v);
            });
            // Started at login (--autostart) or by hand, "start minimized" means the tray only.
            let hidden = force_minimized || initial.start_minimized;
            if !hidden {
                show_main(app.handle());
            }
            let starter = Arc::clone(&link);
            let s = initial.clone();
            std::thread::spawn(move || starter.start(s));
            watch_pairing(app.handle().clone(), Arc::clone(&link));
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("failed to start Catalyst Link");

    app.run(|app, event| {
        if let RunEvent::Exit = event {
            app.state::<App>().link.stop();
        }
    });
}
