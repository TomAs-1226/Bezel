// Catalyst Link's window. Plain modules, no framework, as in Catalyst Console.
//
// Everything the window knows comes through this app's Rust side: `link_state` / `link_log` for the
// process it runs, and `link_api` for the Link's HTTP API on 127.0.0.1 (the Rust side adds the token,
// which never reaches this page). Anything that came from a tablet — a work order's text, a device's
// name — is put on the page as text, never as markup.

import { spring, stateLayer } from "./motion.js";
import * as fmt from "./format.js";

const T = window.__TAURI__;
const invoke = T?.core?.invoke ?? (async () => { throw new Error("open this page in the Catalyst Link app"); });
const listen = T?.event?.listen ?? (async () => () => {});

const $ = (id) => document.getElementById(id);

/** h("div", {class: "row"}, "text", child…): strings become text nodes, never HTML. */
function h(tag, attrs = {}, ...kids) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === "class") node.className = v;
    else if (k.startsWith("on")) node.addEventListener(k.slice(2), v);
    else if (k === "text") node.textContent = v;
    else node.setAttribute(k, v === true ? "" : v);
  }
  for (const kid of kids.flat()) {
    if (kid == null || kid === false) continue;
    node.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
  return node;
}

function dot(kind) { return h("i", { class: `cat-dot${kind ? ` cat-dot--${kind}` : ""}` }); }
function kv(key, value, kind) {
  const v = h("span", { class: "cat-kv__value" });
  if (kind !== undefined) v.append(h("span", { class: "kv-dot" }, dot(kind), value)); else v.append(value ?? "—");
  return h("div", { class: "cat-kv" }, h("span", { class: "cat-kv__key" }, key), v);
}
function fill(node, ...kids) { node.replaceChildren(...kids.flat().filter((k) => k != null && k !== false)); }

function toast(text, bad = false) {
  const t = h("div", { class: `toast${bad ? " is-bad" : ""}`, role: "status" }, text);
  $("toasts").append(t);
  setTimeout(() => { t.classList.add("leaving"); setTimeout(() => t.remove(), 800); }, bad ? 5200 : 2800);
}

async function api(method, path, body) {
  try {
    return await invoke("link_api", { method, path, body: body ?? null });
  } catch (e) {
    return { status: 0, body: { ok: false, error: String(e) } };
  }
}

function problem(r) {
  return r?.body?.error || (r?.status ? `the link answered ${r.status}` : "the link isn't answering");
}

// --- state ------------------------------------------------------------------------------------------

const S = {
  panel: "status",
  view: null,          // the supervisor's view of the Link process
  overview: null,      // GET /admin/overview
  info: null,          // app_info
  settings: null,
  pairWanted: false,   // "pair a tablet" was pressed: show the steps while nothing is pending
  pairPrimed: false,   // the first pairing answer has been seen
  lastPairing: null,   // the most recent successful pairing the Link reported
  pairCode: "",
  pairDone: null,      // {device, at} shown after a pairing lands
  media: null,
  mediaAt: 0,
  artId: null,
  inboxFilter: "open,claimed",
  inboxSel: null,
  inboxItem: null,
  logLast: 0,
  logLines: [],
  forgetArmed: null,
};

// --- firmware -----------------------------------------------------------------------------------
//
// One esptool run, pushed rather than polled: a write takes about a minute and esptool reports
// progress many times a second, which a one-second poll renders as a bar that lurches.

const F = { logLast: 0, ready: null, running: false };

async function refreshFlashReady() {
  try {
    F.ready = await invoke("flash_ready");
  } catch {
    F.ready = null;
  }
  const problem = F.ready?.problem || null;
  $("flashProblem").hidden = !problem;
  if (problem) $("flashProblemText").textContent = problem;

  const dir = F.ready?.firmware_dir;
  $("flashFirmwareDir").textContent = dir
    ? `the build beside this app: ${dir}`
    : "no firmware folder found beside this app \u2014 choose an image";

  const sel = $("flashPort");
  const chosen = sel.value;
  sel.innerHTML = "";
  const auto = document.createElement("option");
  auto.value = "";
  auto.textContent = "find it automatically";
  sel.append(auto);
  for (const port of F.ready?.ports || []) {
    const o = document.createElement("option");
    o.value = port.name;
    o.textContent = port.likely_tablet
      ? `${port.name} \u2014 ${port.description} (looks like the tablet)`
      : `${port.name} \u2014 ${port.description}`;
    sel.append(o);
  }
  if (chosen) sel.value = chosen;
  // Nothing preselects a port: writing firmware to the wrong device is not a mistake worth
  // defaulting into, so "find it automatically" stays the default and esptool decides.
  $("btnFlashStart").disabled = !!problem || F.running;
}

function paintFlash(view) {
  if (!view) return;
  F.running = !!view.running;
  $("flashStatus").textContent = view.message || view.phase || "";
  $("btnFlashStart").disabled = F.running || !!F.ready?.problem;
  $("btnFlashCancel").hidden = !F.running;
  const bar = $("flashBar");
  if (view.percent >= 0) {
    bar.style.width = `${view.percent}%`;
  } else if (!F.running) {
    bar.style.width = view.phase === "verified" ? "100%" : "0%";
  }
}

async function refreshFlashLog() {
  try {
    const page = await invoke("flash_log", { after: F.logLast });
    if (!page.lines.length) return;
    F.logLast = page.last;
    const box = $("flashLog");
    const stuck = box.scrollTop + box.clientHeight >= box.scrollHeight - 8;
    for (const line of page.lines) {
      const row = document.createElement("div");
      row.className = "log__line";
      row.textContent = line;
      box.append(row);
    }
    while (box.childElementCount > 400) box.firstElementChild.remove();
    if (stuck) box.scrollTop = box.scrollHeight;
  } catch {
    /* the log is a convenience; losing it must not take the panel down */
  }
}

async function startFlash() {
  const merged = document.querySelector('input[name="flashTarget"]:checked')?.value === "merged";
  if (merged) {
    const ok = window.confirm(
      "Writing the merged image erases the tablet's pairing, wi-fi and settings.\n\n" +
        "It will have to be paired with this PC again. Continue?"
    );
    if (!ok) return;
  }
  const image = $("flashImage").value.trim();
  const port = $("flashPort").value;
  try {
    await invoke("flash_start", { merged, port: port || null, image: image || null });
    $("flashLog").innerHTML = "";
    F.logLast = 0;
  } catch (e) {
    $("flashStatus").textContent = String(e);
  }
}

// --- card ---------------------------------------------------------------------------------------

const PROV_FIELDS = [
  "team", "wifi_ssid", "wifi_pass",
  "anthropic_api_key", "openai_api_key", "tba_api_key", "nexus_api_key",
  "ha_url", "ha_token", "frc_events_user", "frc_events_token",
];

function provFields() {
  const form = $("provForm");
  const out = {};
  for (const name of PROV_FIELDS) {
    out[name] = form.elements[name]?.value ?? "";
  }
  return out;
}

async function refreshProvision() {
  const card = $("provCard").value.trim();
  if (!card) {
    $("provPending").textContent = "";
    return;
  }
  try {
    const pending = await invoke("provision_pending", { card });
    $("provPending").textContent = pending
      ? `a KEYS.ENV is already on this card (${pending}) \u2014 the tablet has not read it yet`
      : "no KEYS.ENV on this card";
  } catch {
    $("provPending").textContent = "";
  }
}

async function writeProvision() {
  const card = $("provCard").value.trim();
  if (!card) {
    $("provStatus").textContent = "choose the card first";
    return;
  }
  try {
    const written = await invoke("provision_write", {
      card,
      fields: provFields(),
      inCatos: $("provCatos").checked,
    });
    $("provStatus").textContent = `wrote ${written.keys.length} setting(s) to ${written.path}`;
    toast("written \u2014 put the card back in the tablet");
    // Cleared after a successful write: these are credentials, and leaving a wi-fi password sitting
    // in a form on a shared pit laptop is the kind of thing this panel should not encourage.
    for (const name of PROV_FIELDS) {
      const el = $("provForm").elements[name];
      if (el && el.type === "password") el.value = "";
    }
    await refreshProvision();
  } catch (e) {
    $("provStatus").textContent = String(e);
  }
}

// --- runs ---------------------------------------------------------------------------------------

const R = { sel: null, summary: null };

async function refreshRuns() {
  let files = [];
  try {
    files = await invoke("runs_list");
  } catch {
    files = [];
  }
  const list = $("runsList");
  list.innerHTML = "";
  $("runsStatus").textContent = files.length ? `${files.length} file(s)` : "nothing uploaded yet";
  for (const f of files) {
    const row = document.createElement("button");
    row.className = "row row--button";
    row.type = "button";
    if (f.name === R.sel) row.setAttribute("aria-current", "true");
    const when = f.modified ? new Date(f.modified * 1000).toLocaleString() : "";
    row.innerHTML = "";
    const title = document.createElement("b");
    title.textContent = f.name;
    const meta = document.createElement("small");
    const len = f.duration_s != null ? ` \u00b7 ${fmtDuration(f.duration_s)}` : "";
    meta.textContent = `${fmtBytes(f.bytes)}${len}${when ? " \u00b7 " + when : ""}${f.is_run ? "" : " \u00b7 not a run"}`;
    row.append(title, meta);
    row.addEventListener("click", () => openRun(f));
    list.append(row);
  }
}

/** Seconds as a person reads them: `2m 14s`, or `9.4s` under a minute. */
function fmtDuration(seconds) {
  const m = Math.floor(seconds / 60);
  const s = seconds % 60;
  return m ? `${m}m ${s.toFixed(0)}s` : `${s.toFixed(1)}s`;
}

function fmtBytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(0)} kB`;
  return `${(n / 1024 / 1024).toFixed(1)} MB`;
}

async function openRun(file) {
  R.sel = file.name;
  R.summary = null;
  $("btnRunsOpen").hidden = false;
  await refreshRuns();
  const detail = $("runsDetail");
  if (!file.is_run) {
    detail.textContent = "not a recorded run \u2014 open it to see it in whatever this PC uses.";
    return;
  }
  detail.textContent = "reading\u2026";
  try {
    R.summary = await invoke("runs_summary", { name: file.name });
  } catch (e) {
    detail.textContent = String(e);
    return;
  }
  paintRun();
}

function paintRun() {
  const s = R.summary;
  const detail = $("runsDetail");
  if (!s) return;
  detail.innerHTML = "";

  const head = document.createElement("div");
  head.className = "cat-card";
  const mins = Math.floor(s.duration_s / 60);
  const secs = (s.duration_s % 60).toFixed(1);
  head.innerHTML = "";
  const h = document.createElement("div");
  h.className = "cat-card__head";
  const t = document.createElement("span");
  t.className = "cat-card__title";
  t.textContent = s.name;
  h.append(t);
  const body = document.createElement("p");
  body.className = "quiet";
  body.textContent =
    `${mins ? mins + " min " : ""}${secs} s \u00b7 ${s.rows} rows \u00b7 ` +
    `${s.channels.length} channel(s) \u00b7 ${fmtBytes(s.bytes)}` +
    (s.marks.length ? ` \u00b7 ${s.marks.length} mark(s)` : "");
  head.append(h, body);
  detail.append(head);

  const card = document.createElement("div");
  card.className = "cat-card";
  const ch = document.createElement("div");
  ch.className = "cat-card__head";
  const ct = document.createElement("span");
  ct.className = "cat-card__title";
  ct.textContent = "channels";
  ch.append(ct);
  card.append(ch);

  for (let i = 0; i < s.channels.length; i++) {
    const values = s.series[i] || [];
    const present = values.filter((v) => v !== null && v !== undefined);
    const row = document.createElement("div");
    row.className = "row";
    const name = document.createElement("b");
    name.textContent = s.channels[i];
    const meta = document.createElement("small");
    if (!present.length) {
      // A channel the robot never published reads as absent, never as zero: that is the recorder's
      // own rule and the panel keeps it.
      meta.textContent = "never published";
    } else {
      const min = Math.min(...present);
      const max = Math.max(...present);
      const last = present[present.length - 1];
      meta.textContent = `${fmtNum(min)} \u2026 ${fmtNum(max)} \u00b7 last ${fmtNum(last)}` +
        (present.length < values.length ? ` \u00b7 ${values.length - present.length} gap(s)` : "");
    }
    row.append(name, meta);
    card.append(row);
  }
  detail.append(card);
}

function fmtNum(v) {
  if (!isFinite(v)) return "\u2014";
  const a = Math.abs(v);
  if (a >= 1000 || (a < 0.01 && a > 0)) return v.toExponential(2);
  return v.toFixed(a >= 100 ? 0 : a >= 1 ? 2 : 3);
}

// --- navigation ---------------------------------------------------------------------------------------

const PANELS = ["status", "pairing", "media", "claude", "inbox", "flash", "provision", "runs", "log", "settings"];

function show(panel) {
  if (!PANELS.includes(panel)) return;
  const changed = panel !== S.panel;
  S.panel = panel;
  for (const b of document.querySelectorAll(".nav__item")) b.setAttribute("aria-selected", String(b.dataset.panel === panel));
  for (const p of document.querySelectorAll(".panel")) {
    const on = p.dataset.panel === panel;
    p.hidden = !on;
    if (on && changed) { p.classList.remove("enter"); void p.offsetWidth; p.classList.add("enter"); }
  }
  refreshPanel(true);
}

document.querySelector(".nav").addEventListener("click", (e) => {
  const b = e.target.closest(".nav__item");
  if (b) show(b.dataset.panel);
});
document.querySelector(".nav").addEventListener("keydown", (e) => {
  if (e.key !== "ArrowDown" && e.key !== "ArrowUp") return;
  e.preventDefault();
  const i = PANELS.indexOf(S.panel) + (e.key === "ArrowDown" ? 1 : -1);
  const next = PANELS[(i + PANELS.length) % PANELS.length];
  show(next);
  document.querySelector(`.nav__item[data-panel="${next}"]`)?.focus();
});
document.addEventListener("click", (e) => {
  const g = e.target.closest("[data-goto]");
  if (g) show(g.dataset.goto);
});
// Bezel's press: a state layer from the point pressed, on everything pressable.
document.addEventListener("pointerdown", (e) => {
  const b = e.target.closest(".cat-btn, .nav__item, .round, .row--button");
  if (b && !b.disabled) stateLayer(b, e, { opacity: b.classList.contains("round--big") ? 0.16 : 0.1 });
});

// --- the Link process (rail and status hero) -----------------------------------------------------------

async function refreshView() {
  try { S.view = await invoke("link_state"); } catch { S.view = null; }
  paintLink();
}

function paintLink() {
  const v = S.view;
  const st = fmt.linkState(v);
  $("linkDot").className = `cat-dot${st.dot ? ` cat-dot--${st.dot}` : ""}`;
  $("heroDot").className = $("linkDot").className;
  $("linkWord").textContent = st.word;
  $("linkDetail").textContent = st.detail;
  $("heroWord").textContent = st.word;
  const name = S.overview?.name;
  $("heroDetail").textContent = v?.up && name
    ? `catalyst link ${S.overview.version} on ${name}, for ${S.overview.repo_repo || "the robot project"}`
    : v?.state === "setup" ? "" : (v?.message || st.detail || "");
  $("heroDetail").hidden = !$("heroDetail").textContent;
  $("heroSetup").hidden = v?.state !== "setup";
  const running = v && (v.up || v.state === "starting");
  $("btnStartStop").textContent = v?.attached ? "detach" : running ? "stop" : "start";
  $("btnRestart").disabled = !v || v.state === "setup";
  $("btnStartStop").disabled = !v || v.state === "setup";
}

$("btnStartStop").addEventListener("click", async () => {
  const v = S.view;
  const running = v && (v.up || v.state === "starting");
  S.view = await invoke(running ? "link_stop" : "link_start").catch((e) => { toast(String(e), true); return S.view; });
  paintLink();
});
$("btnRestart").addEventListener("click", async () => {
  S.view = await invoke("link_restart").catch((e) => { toast(String(e), true); return S.view; });
  paintLink();
  toast("restarting the link");
});
$("btnChooseRepo").addEventListener("click", async () => {
  const folder = await invoke("pick_folder").catch(() => null);
  if (!folder) return;
  const s = { ...(S.settings || (await invoke("get_settings"))), repo: folder };
  delete s.link_dir_found;
  const saved = await invoke("save_settings", { settings: s }).catch((e) => { toast(String(e), true); return null; });
  if (saved) { S.settings = saved.settings; toast("starting the link"); refreshView(); }
});

// --- status ---------------------------------------------------------------------------------------------

async function refreshOverview() {
  if (!S.view?.up) { S.overview = null; paintBadges(); return; }
  const r = await api("GET", "/admin/overview");
  S.overview = r.status === 200 ? r.body : null;
  paintBadges();
}

function paintBadges() {
  const o = S.overview;
  const open = o ? (o.inbox?.open || 0) + (o.inbox?.claimed || 0) : 0;
  $("countInbox").hidden = !open;
  $("countInbox").textContent = open;
  const running = o ? (o.sessions?.running || 0) + (o.sessions?.waiting_for_input || 0) : 0;
  $("countClaude").hidden = !running;
  $("countClaude").textContent = running;
  $("badgePairing").hidden = !o?.pairing?.pending;
}

function devicesRows(target, devices, { manage = false } = {}) {
  if (!devices?.length) {
    fill(target, h("div", { class: "empty" }, S.view?.up ? "no tablet has paired with this link yet." : "the link isn't running."));
    return;
  }
  fill(target, devices.map((d) => {
    const fresh = fmt.freshness(d.last_seen);
    const armed = S.forgetArmed === d.id;
    const forget = manage && h("button", {
      class: `cat-btn small${armed ? " cat-btn--danger" : ""}`,
      onclick: () => forgetDevice(d),
      "aria-label": `forget ${d.name}`,
    }, armed ? "forget it?" : "forget");
    return h("div", { class: "row" },
      dot(fresh),
      h("div", { class: "row__main" },
        h("div", { class: "row__title" }, d.name),
        h("div", { class: "row__sub" }, `last seen ${fmt.ago(d.last_seen)} · `, h("span", { class: "mono" }, d.ip || "—"), ` · paired ${fmt.ago(d.paired_at)}`)),
      h("div", { class: "row__side" }, forget || ""));
  }));
}

async function forgetDevice(d) {
  if (S.forgetArmed !== d.id) {
    S.forgetArmed = d.id;
    paintPairingLists();
    setTimeout(() => { if (S.forgetArmed === d.id) { S.forgetArmed = null; paintPairingLists(); } }, 4000);
    return;
  }
  S.forgetArmed = null;
  const r = await api("POST", `/admin/devices/${encodeURIComponent(d.id)}/forget`, {});
  if (r.status === 200) toast(`forgot ${d.name}: it has to pair again`); else toast(problem(r), true);
  await refreshOverview();
  paintPairingLists();
}

function paintStatus() {
  const o = S.overview;
  const v = S.view;
  $("stVersion").textContent = o ? `v${o.version}` : "";
  fill($("heroAddr"), (o?.addresses?.length ? o.addresses : o ? ["127.0.0.1"] : []).map((a) =>
    h("span", { class: "cat-chip" }, `http://${a}:${o.port}`)));
  if (!o) {
    // the command is worth reading only when it didn't work
    const broke = v?.state === "failed" || v?.state === "exited";
    fill($("stServer"),
      kv("process", v?.state === "setup" ? "not started" : v?.state === "starting" ? "starting…" : v?.message || "—"),
      broke && v.command ? h("div", { class: "cmdline" }, v.command) : null);
    fill($("stFeatures"), h("div", { class: "empty" }, "shown once the link is running."));
    devicesRows($("stDevices"), []);
    return;
  }
  fill($("stServer"),
    kv("name", o.name),
    kv("port", `${o.port} on ${o.bind}`),
    kv("process", v?.attached ? "started outside this app" : v?.pid ? `pid ${v.pid}${v.restarts ? ` · restarted ${v.restarts}×` : ""}` : "—"),
    kv("robot project", `${o.repo_repo || "—"} · ${o.repo_branch || "?"}${o.repo_dirty ? " · dirty" : ""}`),
    kv("started", fmt.ago(o.started_at)),
    kv("state folder", h("span", { class: "mono" }, o.state_home)));
  const pend = o.pairing.pending;
  const mdnsWord = o.mdns.state === "advertising" ? "advertising _catalyst-link._tcp" : `off${o.mdns.why ? `: ${o.mdns.why}` : ""}`;
  fill($("stFeatures"),
    kv("pairing", o.pairing.enabled ? (pend ? `${pend.device} is pairing now` : "on") : "off", o.pairing.enabled ? (pend ? "warn" : "ok") : ""),
    kv("findable", mdnsWord, o.mdns.state === "advertising" ? "ok" : o.mdns.state === "failed" ? "bad" : ""),
    kv("media remote", o.media.available ? "on" : (o.media.reason || "off"), o.media.available ? "ok" : ""),
    kv("claude", o.claude.via ? `${o.claude.via}${o.claude.available ? "" : " (not ready)"}` : "off", o.claude.available ? "ok" : o.claude.via ? "warn" : ""),
    kv("inbox", `${o.inbox.open} open · ${o.inbox.claimed} claimed`),
    kv("claude code", Object.keys(o.sessions).length ? Object.entries(o.sessions).map(([k, n]) => `${n} ${fmt.sessionState(k).word}`).join(" · ") : "no sessions reported"));
  devicesRows($("stDevices"), o.devices);
}

// --- pairing --------------------------------------------------------------------------------------------

async function refreshPairing() {
  if (!S.view?.up) { S.pairing = null; paintPairing(); return; }
  const r = await api("GET", "/admin/pairing");
  S.pairing = r.status === 200 ? r.body : null;
  const last = S.pairing?.last || null;
  if (!S.pairPrimed) {
    // the first look: whatever paired before this window opened is history, not news
    S.pairPrimed = true;
    S.lastPairing = last;
  } else if (last && last.id !== S.lastPairing?.id) {
    S.lastPairing = last;
    S.pairDone = last;
    S.pairWanted = false;
    toast(`paired ${last.device}`);
    refreshOverview().then(paintPairingLists);
  }
  paintPairing();
}

function renderCode(code) {
  const box = $("pairCode");
  if (code === S.pairCode) return;
  S.pairCode = code;
  const shown = fmt.pairCode(code);
  fill(box, [...shown].map((c) => h("span", { class: c === " " ? "gap" : "" }, c === " " ? "" : c)));
  box.setAttribute("aria-label", `code ${[...code].join(" ")}`);
  // Each digit arrives on the release spring, a beat after the one before it.
  [...box.children].forEach((d, i) => {
    d.style.opacity = "0";
    setTimeout(() => spring({ from: 1, to: 0, role: "release", onFrame: (x) => {
      d.style.transform = `translateY(${x * 18}px)`;
      d.style.opacity = String(Math.min(1, 1 - x));
    } }), i * 40);
  });
}

function paintPairing() {
  const card = $("pairCard");
  const p = S.pairing;
  $("pairName").textContent = S.overview?.name || "this pc";
  let mode = "idle";
  if (S.view?.up && S.overview && !S.overview.pairing.enabled) mode = "off";
  else if (p?.pending) mode = "code";
  else if (S.pairDone) mode = "done";
  else if (S.pairWanted) mode = "wait";
  card.dataset.mode = mode;
  $("btnPair").disabled = !S.view?.up;
  if (mode === "code") {
    const pend = p.pending;
    renderCode(pend.code);
    $("pairDevice").textContent = (pend.device || "the tablet").toUpperCase();
    const left = Math.max(0, pend.expires_in);
    $("pairBar").style.transform = `scaleX(${Math.max(0, Math.min(1, left / (pend.ttl || 120)))})`;
    $("pairLeft").textContent = `${fmt.clock(left)} left`;
    $("pairTries").textContent = `${pend.tries_left} ${pend.tries_left === 1 ? "try" : "tries"}`;
    $("pairIp").textContent = pend.ip;
  } else {
    S.pairCode = "";
  }
  if (mode === "done") $("pairedName").textContent = S.pairDone.device;
  paintPairingLists();
}

function paintPairingLists() {
  const o = S.overview;
  devicesRows($("devList"), o?.devices, { manage: true });
  $("devCount").textContent = o?.devices?.length ? `${o.devices.length} PAIRED` : "";
  fill($("handList"), (o?.by_hand || []).map((c) => h("div", { class: "row" },
    dot(fmt.freshness(c.last_seen)),
    h("div", { class: "row__main" },
      h("div", { class: "row__title" }, "token typed by hand"),
      h("div", { class: "row__sub" }, h("span", { class: "mono" }, c.ip), ` · last seen ${fmt.ago(c.last_seen)}`)),
    h("div", { class: "row__side" }, "main token"))));
  if (S.panel === "status" && o) devicesRows($("stDevices"), o.devices);
}

$("btnPair").addEventListener("click", () => { S.pairWanted = true; S.pairDone = null; paintPairing(); });
$("btnPairStop").addEventListener("click", () => { S.pairWanted = false; paintPairing(); });
$("btnPairAgain").addEventListener("click", () => { S.pairDone = null; S.pairWanted = true; paintPairing(); });
$("btnPairCancel").addEventListener("click", async () => {
  const r = await api("POST", "/admin/pairing/cancel", {});
  if (r.status !== 200) toast(problem(r), true);
  S.pairWanted = false;
  refreshPairing();
});

// --- now playing ----------------------------------------------------------------------------------------

async function refreshMedia() {
  if (!S.view?.up) { S.media = null; paintMedia(); return; }
  const r = await api("GET", "/media/now");
  S.media = r.status === 200 ? r.body : { available: false, reason: problem(r) };
  S.mediaAt = performance.now();
  const art = S.media?.playing?.art || null;
  if (art !== S.artId) {
    S.artId = art;
    const box = $("mediaArt");
    if (!art) { box.style.backgroundImage = ""; box.classList.remove("has-art"); }
    else {
      const a = await api("GET", "/media/art?format=jpeg&size=440&encoding=base64");
      if (a.status === 200 && a.body?.data) {
        box.style.backgroundImage = `url(data:image/jpeg;base64,${a.body.data})`;
        box.classList.add("has-art");
      } else { box.style.backgroundImage = ""; box.classList.remove("has-art"); }
    }
  }
  paintMedia();
}

function paintMedia() {
  const m = S.media;
  const card = $("mediaCard");
  const notice = $("mediaNotice");
  const p = m?.playing;
  card.dataset.state = p?.state || "none";
  const off = !m?.available;
  notice.hidden = !off;
  if (off) {
    fill(notice,
      h("b", {}, S.view?.up ? "the media remote isn't available" : "the link isn't running"),
      h("span", {}, m?.reason || ""),
      S.view?.up && /winrt|install|windows/i.test(m?.reason || "")
        ? h("span", {}, "install it with ", h("code", {}, "python -m pip install \".[media]\""), " in tab5/link, then restart the link.")
        : null);
  }
  $("mediaTitle").textContent = p ? (p.title || "untitled") : off ? "—" : "nothing playing";
  $("mediaArtist").textContent = p ? [p.artist, p.album].filter(Boolean).join(" · ") : "";
  $("mediaApp").textContent = p?.app ? p.app.toUpperCase() : "";
  paintProgress();
  const can = p?.can || {};
  for (const b of card.querySelectorAll("[data-media]")) {
    const a = b.dataset.media;
    const gate = { previous: "previous", next: "next", toggle: "toggle" }[a];
    b.disabled = off || (gate ? p == null || can[gate] === false : false);
  }
  const vol = $("mediaVolume");
  const hasLevel = m?.volume != null;
  vol.disabled = off || !hasLevel;
  if (hasLevel && document.activeElement !== vol) setRange(vol, Math.round(m.volume * 100));
  $("mediaVolText").textContent = off ? "" : hasLevel ? `${Math.round(m.volume * 100)}%${m.muted ? " · muted" : ""}` : "level needs pycaw";
}

function paintProgress() {
  const p = S.media?.playing;
  let pos = p?.position;
  if (pos != null && p.state === "playing") pos += (performance.now() - S.mediaAt) / 1000;
  if (pos != null && p?.duration) pos = Math.min(pos, p.duration);
  $("mediaPos").textContent = fmt.clock(pos);
  $("mediaDur").textContent = fmt.clock(p?.duration);
  $("mediaBar").style.width = p?.duration && pos != null ? `${(pos / p.duration) * 100}%` : "0%";
}

function setRange(r, value) { r.value = value; r.style.setProperty("--fill", `${value}%`); }

$("mediaCard").addEventListener("click", async (e) => {
  const b = e.target.closest("[data-media]");
  if (!b || b.disabled) return;
  const r = await api("POST", "/media/control", { action: b.dataset.media });
  if (r.status !== 200) toast(problem(r), true);
  setTimeout(refreshMedia, 250);
});
let volTimer = 0;
$("mediaVolume").addEventListener("input", (e) => {
  setRange(e.target, e.target.value);
  clearTimeout(volTimer);
  volTimer = setTimeout(async () => {
    const r = await api("POST", "/media/control", { action: "volume", level: Number(e.target.value) / 100 });
    if (r.status !== 200) toast(problem(r), true);
  }, 120);
});

// --- claude code ----------------------------------------------------------------------------------------

async function refreshHooks() {
  if (!S.view?.up) return paintHooks(null);
  const r = await api("GET", "/admin/claude-hooks");
  paintHooks(r.status === 200 ? r.body : { error: problem(r) });
}

function paintHooks(st) {
  const btn = $("btnHooks");
  btn.disabled = !st || !!st.error && !st.path;
  const good = st?.installed && !st.outdated;
  btn.dataset.install = good ? "false" : "true";
  btn.textContent = good ? "remove the hooks" : st?.outdated ? "update the hooks" : st?.partial ? "repair the hooks" : "install the hooks";
  btn.className = `cat-btn${good ? "" : " cat-btn--primary"}`;
  $("hooksTitle").textContent = !st ? "claude code's hooks"
    : good ? "claude code reports to the link"
    : st.outdated ? "the hooks point somewhere else"
    : st.partial ? "some of the hooks are missing" : "claude code isn't reporting yet";
  fill($("hooksDetail"), !st ? "shown once the link is running." : st.error ? st.error : st.outdated ? [
    "the hooks in ", h("code", {}, st.path), " run another python, another copy of the link or another port. update them to point at this link.",
  ] : [
    st.installed ? "installed in " : "adds the link's hook to ", h("code", {}, st.path),
    st.installed ? ", next to any hooks of your own. new sessions pick it up; restart the running ones." : ", leaving every other hook and setting as it is (a backup is kept next to it).",
  ]);
}

$("btnHooks").addEventListener("click", async () => {
  const install = $("btnHooks").dataset.install === "true";
  $("btnHooks").disabled = true;
  const r = await api("POST", "/admin/claude-hooks", { install });
  if (r.status === 200) toast(install ? "hooks installed: restart your claude code sessions" : "hooks removed");
  else toast(problem(r), true);
  refreshHooks();
});

async function refreshSessions() {
  if (!S.view?.up) { fill($("ccList"), h("div", { class: "empty" }, "the link isn't running.")); return; }
  const r = await api("GET", "/v1/claude/sessions");
  if (r.status !== 200) { fill($("ccList"), h("div", { class: "empty" }, problem(r))); return; }
  const list = r.body.sessions || [];
  $("ccTurns").textContent = `${r.body.turns_recorded || 0} TURNS RECORDED`;
  if (!list.length) {
    fill($("ccList"), h("div", { class: "empty" }, "no claude code session has reported. with the hooks installed, a session shows up on its first prompt."));
    return;
  }
  fill($("ccList"), list.map((s) => {
    const st = fmt.sessionState(s.state);
    const eta = s.eta ? `~${fmt.duration(s.eta.remaining_s)} left` : "";
    return h("div", { class: "row session" },
      dot(st.dot),
      h("div", { class: "row__main" },
        h("div", { class: "row__title" }, s.title),
        h("div", { class: "row__sub" }, [s.project, s.step || (s.state === "done" ? `finished ${fmt.ago(s.finished_at)}` : "")].filter(Boolean).join(" · "))),
      h("div", { class: "row__side" },
        h("span", {}, st.word),
        // running: how long it has worked on this turn; waiting: how long it has waited on you
        h("span", { class: "eta" }, s.state === "running" ? fmt.duration(s.elapsed_s) : s.state === "waiting_for_input" ? fmt.duration(s.since_s) : ""),
        eta ? h("span", { class: "dim" }, eta) : null));
  }));
}

// --- inbox ----------------------------------------------------------------------------------------------

async function refreshInbox() {
  if (!S.view?.up) { fill($("inboxList"), h("div", { class: "empty" }, "the link isn't running.")); return; }
  const r = await api("GET", `/inbox?status=${encodeURIComponent(S.inboxFilter)}`);
  if (r.status !== 200) { fill($("inboxList"), h("div", { class: "empty" }, problem(r))); return; }
  const items = r.body.items || [];
  if (!items.length) {
    fill($("inboxList"), h("div", { class: "empty" }, S.inboxFilter === "open,claimed" ? "nothing to do: the tablet hasn't sent a work order." : "none."));
    return;
  }
  fill($("inboxList"), items.map((it) => h("div", {
    class: "row row--button", role: "button", tabindex: "0", "aria-current": String(it.id === S.inboxSel),
    onclick: () => openItem(it.id), onkeydown: (e) => { if (e.key === "Enter") openItem(it.id); },
  },
    h("span", { class: `cat-chip small chip-status--${it.status}` }, it.status),
    h("div", { class: "row__main" },
      h("div", { class: "row__title" }, it.title),
      h("div", { class: "row__sub" }, [it.kind, it.priority !== "normal" ? it.priority : "", it.when].filter(Boolean).join(" · "))),
    h("span", {}))));
}

async function openItem(id) {
  S.inboxSel = id;
  const r = await api("GET", `/inbox/${encodeURIComponent(id)}`);
  S.inboxItem = r.status === 200 ? r.body.item : null;
  if (!S.inboxItem) toast(problem(r), true);
  paintItem();
  refreshInbox();
}

function paintItem() {
  const it = S.inboxItem;
  const box = $("inboxDetail");
  if (!it) { fill(box, h("p", { class: "quiet empty" }, "pick a work order to read it.")); return; }
  const note = h("textarea", { class: "wo__note", placeholder: "a note for the technician (needed for done and reject)", rows: "3" });
  const moves = fmt.inboxActions(it.status).map((a) => h("button", {
    class: `cat-btn${a === "done" ? " cat-btn--primary" : ""}`,
    onclick: () => move(it, a, note.value),
  }, a));
  const robot = it.robot && Object.keys(it.robot).length
    ? h("details", { class: "wo__robot" }, h("summary", {}, "robot snapshot when it was filed"), h("pre", {}, JSON.stringify(it.robot, null, 2)))
    : null;
  fill(box,
    h("div", { class: "wo__chips" },
      h("span", { class: `cat-chip small chip-status--${it.status}` }, it.status),
      it.kind ? h("span", { class: "cat-chip small" }, it.kind) : null,
      it.priority ? h("span", { class: "cat-chip small" }, it.priority) : null,
      it.patch ? h("span", { class: "cat-chip small" }, `patch ${it.patch}`) : null),
    h("div", { class: "wo__title" }, it.title || it.id),
    h("div", { class: "wo__meta" }, `${it.id} · ${it.when || ""}${it.from ? ` · from ${it.from}` : ""}`),
    h("div", { class: "wo__body" }, (it.body || "").trim() || "(no text)"),
    robot,
    it.notes ? h("div", { class: "wo__body" }, it.notes.trim()) : null,
    moves.length ? note : null,
    h("div", { class: "wo__actions" }, moves,
      h("button", { class: "cat-btn cat-btn--ghost", onclick: () => invoke("open_work_order", { path: it.path }).catch((e) => toast(String(e), true)) }, "open the file")));
}

async function move(it, action, note) {
  const status = fmt.MOVE[action];
  if ((status === "done" || status === "rejected") && !note.trim()) {
    toast(`${action} needs a note: what changed, or why not`, true);
    return;
  }
  const r = await api("POST", `/admin/inbox/${encodeURIComponent(it.id)}/status`, { status, note });
  if (r.status === 200) toast(`${status === "open" ? "released" : status}: ${it.title || it.id}`); else toast(problem(r), true);
  await openItem(it.id);
  refreshOverview();
}

$("inboxTabs").addEventListener("click", (e) => {
  const t = e.target.closest(".cat-tab");
  if (!t) return;
  S.inboxFilter = t.dataset.filter;
  for (const b of $("inboxTabs").children) b.setAttribute("aria-selected", String(b === t));
  refreshInbox();
});

// --- log ------------------------------------------------------------------------------------------------

async function refreshLog() {
  let page;
  try { page = await invoke("link_log", { after: S.logLast }); } catch { return; }
  if (!page.lines.length) return;
  S.logLast = page.last;
  S.logLines.push(...page.lines);
  if (S.logLines.length > 1500) S.logLines.splice(0, S.logLines.length - 1500);
  paintLog(page.lines);
}

function logRow(l) {
  const t = new Date(l.t);
  const hh = t.toTimeString().slice(0, 8);
  const bad = /traceback|error|failed|exception/i.test(l.text) && !/ 200 /.test(l.text);
  return h("div", { class: `log__line${bad ? " is-error" : ""}`, "data-stream": l.stream },
    h("span", { class: "log__t" }, hh), h("span", { class: "log__x" }, l.text));
}

function paintLog(added) {
  const box = $("logBox");
  const q = $("logFilter").value.trim().toLowerCase();
  const keep = (l) => !q || l.text.toLowerCase().includes(q);
  if (added && box.childElementCount && !q) box.append(...added.filter(keep).map(logRow));
  else fill(box, S.logLines.filter(keep).map(logRow));
  if (!box.childElementCount) fill(box, h("div", { class: "empty" }, S.view?.attached ? "attached to a link started outside this app: its output isn't visible here." : "nothing yet."));
  while (box.childElementCount > 1500) box.firstElementChild.remove();
  if ($("logFollow").checked) box.scrollTop = box.scrollHeight;
}

$("logFilter").addEventListener("input", () => paintLog());
$("btnLogCopy").addEventListener("click", async () => {
  const q = $("logFilter").value.trim().toLowerCase();
  const text = S.logLines.filter((l) => !q || l.text.toLowerCase().includes(q))
    .map((l) => `${new Date(l.t).toTimeString().slice(0, 8)}  ${l.text}`).join("\n");
  try { await navigator.clipboard.writeText(text); toast("copied the log"); } catch { toast("couldn't reach the clipboard", true); }
});

// --- settings -------------------------------------------------------------------------------------------

const FORM_KEYS = ["repo", "port", "name", "media", "pairing", "toast", "mdns", "start_with_windows", "start_minimized", "python", "link_dir"];

async function loadSettings() {
  try { S.settings = await invoke("get_settings"); } catch (e) { $("setStatus").textContent = String(e); return; }
  const form = $("settingsForm");
  for (const k of FORM_KEYS) {
    const input = form.elements[k];
    if (!input) continue;
    if (input.type === "checkbox") input.checked = !!S.settings[k]; else input.value = S.settings[k] ?? "";
  }
  $("setLinkDir").textContent = S.settings.link_dir_found
    ? `runs catalyst_link from ${S.settings.link_dir_found}`
    : "no catalyst_link folder found: runs the pip-installed package (python -m pip install . in tab5/link)";
  if (S.info) $("setPaths").textContent = `settings: ${S.info.settings_path} · the link's state: ${S.info.link_home}`;
}

function readForm() {
  const form = $("settingsForm");
  const out = {};
  for (const k of FORM_KEYS) {
    const input = form.elements[k];
    if (!input) continue;
    out[k] = input.type === "checkbox" ? input.checked : input.type === "number" ? Number(input.value) : input.value.trim();
  }
  return out;
}

$("btnSave").addEventListener("click", async () => {
  const s = readForm();
  if (!Number.isInteger(s.port) || s.port < 1024 || s.port > 65535) { toast("the port is a number from 1024 to 65535", true); return; }
  $("btnSave").disabled = true;
  try {
    const saved = await invoke("save_settings", { settings: s });
    S.settings = saved.settings;
    if (saved.autostart_error) toast(`start with windows: ${saved.autostart_error}`, true);
    toast(saved.restarted ? "saved: the link restarts with it" : "saved");
    $("setStatus").textContent = "";
    await loadSettings();
    refreshView();
  } catch (e) {
    toast(String(e), true);
  } finally {
    $("btnSave").disabled = false;
  }
});
$("btnPickRepo").addEventListener("click", async () => {
  const folder = await invoke("pick_folder").catch(() => null);
  if (folder) $("settingsForm").elements.repo.value = folder;
});
$("settingsForm").addEventListener("submit", (e) => e.preventDefault());

// --- the clock ------------------------------------------------------------------------------------------
// One timer. Each panel refreshes at its own pace while it is the one on screen; the rail's state and
// badges refresh always, slowly.

let tickN = 0;
let busy = false;

async function refreshPanel(now = false) {
  const p = S.panel;
  if (p === "status") { if (now) await refreshOverview(); paintStatus(); }
  if (p === "pairing") { if (now) await refreshOverview(); await refreshPairing(); }
  if (p === "media") await refreshMedia();
  if (p === "claude") { if (now || tickN % 10 === 0) await refreshHooks(); await refreshSessions(); }
  if (p === "inbox") { await refreshInbox(); if (now && S.inboxSel) paintItem(); }
  if (p === "log") { if (now) paintLog(); }
  if (p === "flash") { if (now) await refreshFlashReady(); await refreshFlashLog(); }
  if (p === "provision" && now) await refreshProvision();
  if (p === "runs" && now) await refreshRuns();
  if (p === "settings" && now) await loadSettings();
}

async function tick() {
  if (busy) return;
  busy = true;
  try {
    tickN++;
    const wasUp = S.view?.up;
    await refreshView();
    await refreshLog();
    const everyN = { status: 2, pairing: 1, media: 1, claude: 2, inbox: 5, flash: 1, provision: 0, runs: 0, log: 1, settings: 0 }[S.panel];
    if (tickN % 3 === 0 || wasUp !== S.view?.up) await refreshOverview();
    if (everyN && tickN % everyN === 0) await refreshPanel(false);
    else if (wasUp !== S.view?.up) await refreshPanel(true);
  } finally {
    busy = false;
  }
}

setInterval(() => { if (S.panel === "media") paintProgress(); }, 250);

async function boot() {
  try { S.info = await invoke("app_info"); } catch { S.info = null; }
  $("appVersion").textContent = S.info ? `catalyst link desktop ${S.info.version}` : "open in the catalyst link app";
  listen("navigate", (e) => show(String(e.payload)));
  listen("pairing-request", (e) => toast(`${e.payload} wants to pair`));
  listen("link-state", (e) => { S.view = e.payload; paintLink(); });
  listen("flash-state", (e) => { paintFlash(e.payload); refreshFlashLog(); });

  $("btnFlashStart").addEventListener("click", startFlash);
  $("btnFlashCancel").addEventListener("click", () => invoke("flash_cancel"));
  $("btnFlashPorts").addEventListener("click", refreshFlashReady);
  $("btnFlashPick").addEventListener("click", async () => {
    const picked = await invoke("flash_pick_image");
    if (picked) $("flashImage").value = picked;
  });

  $("btnProvWrite").addEventListener("click", writeProvision);
  $("btnProvPick").addEventListener("click", async () => {
    const picked = await invoke("provision_pick_card");
    if (picked) { $("provCard").value = picked; await refreshProvision(); }
  });
  $("provCard").addEventListener("change", refreshProvision);

  $("btnRunsOpen").addEventListener("click", () => {
    if (R.sel) invoke("runs_open", { name: R.sel }).catch((e) => { $("runsStatus").textContent = String(e); });
  });
  await refreshView();
  await refreshOverview();
  await loadSettings();
  show("status");
  if (S.view?.state === "setup") toast("choose the robot project to start the link");
  setInterval(tick, 1000);
}

boot();
