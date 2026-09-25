// How the window words things. Pure functions, so `npm test` covers them without a window.

/** 75 -> "1m 15s", 4000 -> "1h 06m", 9 -> "9s". */
export function duration(seconds) {
  if (seconds == null || !Number.isFinite(seconds)) return "—";
  const s = Math.max(0, Math.round(seconds));
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${String(s % 60).padStart(2, "0")}s`;
  return `${Math.floor(s / 3600)}h ${String(Math.floor((s % 3600) / 60)).padStart(2, "0")}m`;
}

/** 83 -> "1:23", 3725 -> "1:02:05": a media position. */
export function clock(seconds) {
  if (seconds == null || !Number.isFinite(seconds)) return "–:––";
  const s = Math.max(0, Math.floor(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = String(s % 60).padStart(2, "0");
  return h ? `${h}:${String(m).padStart(2, "0")}:${sec}` : `${m}:${sec}`;
}

/** An ISO time -> "just now", "4 min ago", "3 h ago", "2 days ago", relative to `now` (ms). */
export function ago(iso, now = Date.now()) {
  if (!iso) return "never";
  const t = Date.parse(iso);
  if (!Number.isFinite(t)) return "—";
  const s = Math.max(0, (now - t) / 1000);
  if (s < 45) return "just now";
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  const d = Math.round(s / 86400);
  return d === 1 ? "yesterday" : `${d} days ago`;
}

/** "482913" -> "482 913", the way pairing.py prints it. Anything else is returned as it came. */
export function pairCode(code) {
  const c = String(code ?? "");
  return /^\d{6}$/.test(c) ? `${c.slice(0, 3)} ${c.slice(3)}` : c;
}

/** The word and the dot class for a Claude Code session state (sessions.py). */
export function sessionState(state) {
  switch (state) {
    case "running": return { word: "running", dot: "info" };
    case "waiting_for_input": return { word: "waiting on you", dot: "warn" };
    case "done": return { word: "done", dot: "ok" };
    case "error": return { word: "error", dot: "bad" };
    default: return { word: String(state || "idle"), dot: "" };
  }
}

/** The Link's own state (the supervisor's `state`) as words and a dot. */
export function linkState(view) {
  const port = view?.port ? `:${view.port}` : "";
  switch (view?.state) {
    case "running": return { word: "running", detail: `serving on ${port}`, dot: "ok" };
    case "attached": return { word: "attached", detail: `using the link on ${port}`, dot: "ok" };
    case "starting": return { word: "starting", detail: `starting on ${port}`, dot: "info" };
    case "setup": return { word: "needs setup", detail: "choose the robot project", dot: "warn" };
    case "failed": return { word: "failed", detail: view.message || "", dot: "bad" };
    case "exited": return { word: "stopped", detail: view.message || "", dot: "bad" };
    case "stopped": return { word: "stopped", detail: view.message || "", dot: "" };
    default: return { word: "—", detail: "", dot: "" };
  }
}

/** Which inbox moves the PC offers from a status (inbox.py TRANSITIONS, from the PC's side). */
export function inboxActions(status) {
  switch (status) {
    case "open": return ["claim", "done", "reject"];
    case "claimed": return ["release", "done", "reject"];
    default: return [];
  }
}

/** The status a move asks the Link for. */
export const MOVE = { claim: "claimed", release: "open", done: "done", reject: "rejected" };

/** A tablet's last-seen as a freshness class: a tablet polls every few seconds while it's awake. */
export function freshness(iso, now = Date.now()) {
  const t = Date.parse(iso || "");
  if (!Number.isFinite(t)) return "";
  const s = (now - t) / 1000;
  if (s < 30) return "ok";
  if (s < 600) return "info";
  return "";
}
