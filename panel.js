// The panel's core: what is where, what moves, and the render state for each frame. Seven slots sit side by
// side (four home pages, apps, now playing, standby), and an app window can open over them. Surfaces, glass
// and text layers come from the DOM; the other modules add behaviour through ctx.

import { Coast, Motion, SPRINGS, pageFor, project, rubberBand, velocityTracker } from './motion.js';
import { createRaster } from './raster.js';
import { thumbnail } from './scenes.js';

export const hex = (value) => {
  const h = String(value).trim().replace('#', '');
  const n = parseInt(h.length === 3 ? h.replace(/./g, (c) => c + c) : h, 16) || 0;
  return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
};
export const mixRgb = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

export const SLOTS = 7;
export const HOME_PAGES = 4;
export const APPS = 4;
export const PLAYING = 5;
export const STANDBY = 6;
const sectionOf = (slot) => (slot < HOME_PAGES ? 0 : slot - HOME_PAGES + 1);
const slotOf = (el) => { const s = el.closest('.scr'); return s ? Number(s.dataset.slot) : -1; };
// A radius as drawn: percentages resolve against the element, and no corner exceeds half the short side.
const radiusOf = (el) => {
  const value = getComputedStyle(el).borderTopLeftRadius;
  const short = Math.min(el.offsetWidth, el.offsetHeight) || Infinity;
  const r = value.endsWith('%') ? (parseFloat(value) / 100) * short : parseFloat(value) || 0;
  return Math.min(r, short / 2);
};
// Glass melts together only inside its group, as in a GlassEffectContainer; separate groups never touch.
const GROUPS = { dock: 0, transport: 1, 'pill-back': 2, 'pill-out': 2, toast: 3, knob: 4, pager: 5, 'activity-0': 7, 'activity-1': 7, 'activity-2': 7 };
// How long each section waits before its chrome steps back: home, apps, now playing (never), standby.
const IDLE = [4000, 4000, 0, 2500];

export function createPanel(screen, settings, { cover }) {
  const q = (selector) => screen.querySelector(selector);
  const screensEl = q('#screens');
  const homeCol = q('#home-col');
  const appLayer = q('#app-layer');
  const dock = q('#dock');
  const pager = q('#pager');
  const pagerDots = [...pager.children];
  const dockButtons = [...dock.querySelectorAll('.dock-item')];
  const dockIcons = dockButtons.map((b) => b.querySelector('.icon'));
  const slotEls = [...screen.querySelectorAll('.scr')];
  const hooks = { measure: [], frame: [], retone: [], index: [] };
  const uploads = [];
  const slotPans = new Map();
  const dragTargets = [];
  let W = 1, H = 1, unit = 1, index = 0, lastHome = 0, scrollMax = 0, shownDot = -1;
  let dockItems = [], ring = { x: 0, y: 0 };
  let suppressUntil = 0;
  let extraDirty = null;
  let appWindow = () => null;
  let controlCenter = () => null;
  let appOpen = false;

  const page = new Motion(0, { spring: SPRINGS.page, epsilon: 0.1 });
  const scroll = new Motion(0, { spring: SPRINGS.release, epsilon: 0.1 });
  const coast = new Coast(scroll, () => [-scrollMax, 0]);
  const lens = new Motion(0, { spring: SPRINGS.release, epsilon: 0.1 });
  const lift = new Motion(0, { spring: SPRINGS.wobble, epsilon: 0.001 });
  const chrome = new Motion(1, { spring: SPRINGS.release, epsilon: 0.001 });

  const surfaces = [...screen.querySelectorAll('[data-surface]')].map((el) => ({
    el, slot: slotOf(el), app: appLayer.contains(el), cc: !!el.closest('#cc'), home: homeCol.contains(el), ink: el.hasAttribute('data-ink'), base: null, r0: 0,
    clipEl: el.dataset.clip ? q(`#${el.dataset.clip}`) : null, clip: null, from: null, to: null,
    fade: new Motion(1, { spring: SPRINGS.effect, epsilon: 0.002 }),
    radius: new Motion(0, { spring: SPRINGS.release, epsilon: 0.05 }),
    layer: new Motion(0, { spring: SPRINGS.layer, epsilon: 0.002 }),
    lx: 0, ly: 0, geometry: null, alpha: null,
  }));
  const surfaceMap = new Map(surfaces.map((s) => [s.el, s]));
  const glass = [...screen.querySelectorAll('[data-glass]')].map((el) => ({
    el, slot: slotOf(el), app: appLayer.contains(el), cc: !!el.closest('#cc'), group: GROUPS[el.id] ?? (el.closest('#cc') ? 6 : 4), base: null, r: 0, px: 0, py: 0, dx: 0, dy: 0, shift: '',
    gel: el === dock ? 0.02 : 0.06, centred: getComputedStyle(el).transform !== 'none',
    press: new Motion(0, { spring: SPRINGS.release, epsilon: 0.001 }),
    strength: new Motion(el.id === 'toast' ? 0 : 1, { spring: SPRINGS.effect, epsilon: 0.002 }),
    tint: new Motion(0, { spring: SPRINGS.effect, epsilon: 0.002 }),
    geometry: null,
  }));
  const dockGlass = glass.find((g) => g.el === dock);
  const pagerGlass = glass.find((g) => g.el === pager);

  // Text layers: each page's text, icons and marked shapes are rasterized so the renderer can draw them under
  // the glass and the glass can bend them. Apps rasterize their own windows in apps.js.
  const rasters = [[0, homeCol], [1, q('#rooms-view')], [2, q('#climate-view')], [3, q('#today-view')], [APPS, q('#apps-view')], [PLAYING, q('#play-view')]]
    .map(([slot, root]) => ({ slot, root, name: `slot${slot}`, raster: createRaster(root), origin: { x: 0, y: 0 } }));
  const art = q('#media-art');
  const artCanvas = document.createElement('canvas');
  artCanvas.width = artCanvas.height = 184;
  rasters[0].raster.setImage(art, artCanvas);
  /** Puts a cover on the overview's media tile. The renderer takes the full-size art for now playing itself. */
  function setCover(image) {
    artCanvas.getContext('2d', { willReadFrequently: true }).drawImage(image, 0, 0, 184, 184);
    const url = `url(${thumbnail(image)})`;
    art.style.backgroundImage = url;
    screen.style.setProperty('--cover', url);
    if (rasters[0].raster.width > 1) dirty(art);
  }
  setCover(cover);

  function upload(name, source, region) {
    if (!region) {
      for (let i = uploads.length - 1; i >= 0; i--) if (uploads[i].name === name) uploads.splice(i, 1);
    } else if (uploads.some((u) => u.name === name && !u.region)) {
      return;
    }
    uploads.push({ name, source, region });
  }
  function dirty(el) {
    const owner = rasters.find((r) => r.root.contains(el));
    if (!owner) { extraDirty?.(el); return; }
    const region = owner.raster.update(el);
    if (region) upload(owner.name, owner.raster.canvas, region);
  }

  function recolor(s, animate) {
    const c = hex(getComputedStyle(s.el).getPropertyValue('--fill'));
    if (!animate || !s.to) { s.from = c; s.to = c; s.fade.set(1); return; }
    s.from = mixRgb(s.from, s.to, s.fade.value);
    s.to = c;
    s.fade.set(0);
    s.fade.to(1);
  }
  function reshape(s) {
    s.r0 = radiusOf(s.el);
    s.radius.to(s.r0, { spring: SPRINGS.release });
  }

  function measure() {
    const box = screen.getBoundingClientRect();
    W = box.width; H = box.height; unit = W / 720;
    const rel = (el) => { const r = el.getBoundingClientRect(); return { x: r.left - box.left, y: r.top - box.top, w: r.width, h: r.height }; };
    screensEl.style.transform = 'none';
    homeCol.style.transform = 'none';
    appLayer.style.transform = 'none';
    for (const g of glass) g.el.style.transform = g.centred ? 'translateX(-50%)' : '';
    for (const h of hooks.measure) h.before?.();
    for (const s of surfaces) {
      s.base = rel(s.el);
      s.r0 = radiusOf(s.el);
      if (!s.radius.moving) s.radius.set(s.r0);
      s.clip = s.clipEl ? { ...rel(s.clipEl), r: radiusOf(s.clipEl), surface: surfaceMap.get(s.clipEl) ?? null } : null;
      if (!s.to) recolor(s, false);
    }
    for (const g of glass) { g.base = rel(g.el); g.r = radiusOf(g.el); }
    dockItems = dockButtons.map(rel);
    for (const r of rasters) { r.origin = rel(r.root); r.raster.draw(); upload(r.name, r.raster.canvas, null); }
    const np = rel(q('#np-ring'));
    ring = { x: np.x - PLAYING * W, y: np.y };
    scrollMax = Math.max(0, homeCol.offsetHeight - (H - rasters[0].origin.y));
    for (const h of hooks.measure) h.after?.(rel);
    page.set(-index * W);
    scroll.set(clamp(scroll.value, -scrollMax, 0));
    lens.set(dockItems[sectionOf(index)].x);
    shownDot = -1;
    apply();
  }

  function apply() {
    screensEl.style.transform = `translate3d(${page.value}px, 0, 0)`;
    homeCol.style.transform = `translate3d(0, ${scroll.value}px, 0)`;
    const c = chrome.value;
    dockGlass.dy = (1 - c) * 46 * unit;
    pagerGlass.dy = (1 - c) * 18 * unit;
    for (const g of glass) {
      const move = g.dx || g.dy ? `translate(${g.dx.toFixed(2)}px, ${g.dy.toFixed(2)}px) ` : '';
      g.el.style.transform = `${g.centred ? 'translateX(-50%) ' : ''}${move}${g.shift}scale(${1 + g.gel * g.press.value})`;
    }
    // The glass itself fades through its strength; only the dock's glyphs are DOM.
    dock.style.opacity = c > 0.999 ? '' : c.toFixed(3);
    const pos = -page.value / W;
    pager.style.opacity = (c * clamp(HOME_PAGES - pos, 0, 1)).toFixed(3);
    const dot = clamp(Math.round(pos), 0, HOME_PAGES - 1);
    if (dot !== shownDot) { shownDot = dot; pagerDots.forEach((d, k) => d.classList.toggle('is-on', k === dot)); }
    const item = dockItems[sectionOf(index)];
    if (!item) return;
    const cx = lens.value + item.w / 2;
    dockIcons.forEach((icon, k) => {
      const it = dockItems[k];
      const s = 1 + 0.2 * lift.value * Math.max(0, 1 - Math.abs(it.x + it.w / 2 - cx) / it.w);
      icon.style.transform = Math.abs(s - 1) < 1e-3 ? '' : `scale(${s})`;
    });
  }

  function go(i, velocity, wakeChrome = true) {
    const from = index;
    index = clamp(i, 0, SLOTS - 1);
    if (index < HOME_PAGES) lastHome = index;
    const section = sectionOf(index);
    page.to(-index * W, { velocity });
    lens.to(dockItems[section].x);
    dockButtons.forEach((b, k) => {
      b.classList.toggle('is-on', k === section);
      if (k === section) b.setAttribute('aria-current', 'page'); else b.removeAttribute('aria-current');
    });
    for (const s of slotEls) s.inert = Number(s.dataset.slot) !== index;
    if (wakeChrome) wake();
    for (const fn of hooks.index) fn(index, from);
  }
  // Home returns to the page you were on; pressed again it goes to the overview, then to its top.
  function goSection(k) {
    if (k > 0) go(HOME_PAGES + k - 1);
    else if (sectionOf(index) > 0) go(lastHome);
    else if (index > 0) go(0);
    else { coast.cancel(); scroll.to(0, { spring: SPRINGS.release }); wake(); }
  }

  const local = (e) => { const b = screen.getBoundingClientRect(); return [e.clientX - b.left, e.clientY - b.top]; };

  // ---------- chrome ----------
  // On home, apps and standby the dock and pager step back after a few idle seconds, and while the overview scrolls
  // down. A touch brings them back; a touch where the dock was does nothing else. An open app keeps them away.
  const down = new Map(); // pointer id → whether lifting it wakes the chrome
  let idleTimer = 0;
  let anchor = 0;
  function schedule() {
    clearTimeout(idleTimer);
    const wait = IDLE[sectionOf(index)];
    if (!wait || chrome.target === 0 || appOpen) return;
    idleTimer = setTimeout(() => { if (down.size || dock.matches(':hover')) schedule(); else tuck(); }, wait);
  }
  function wake() {
    if (appOpen) return;
    if (chrome.target !== 1) {
      chrome.to(1, { spring: SPRINGS.release });
      anchor = clamp(scroll.value, -scrollMax, 0);
    }
    dock.style.pointerEvents = '';
    schedule();
  }
  function tuck(force = false) {
    clearTimeout(idleTimer);
    if ((!IDLE[sectionOf(index)] && !force) || chrome.target === 0) return;
    chrome.to(0, { spring: SPRINGS.smooth });
    dock.style.pointerEvents = 'none';
    anchor = clamp(scroll.value, -scrollMax, 0);
  }
  // Scrolling down sends the chrome away and scrolling back up returns it. The bounce at an edge doesn't count.
  function watchScroll() {
    const v = clamp(scroll.value, -scrollMax, 0);
    if (index !== 0 || appOpen) { anchor = v; return; }
    if (chrome.target === 1) {
      if (v < anchor - 32 * unit) tuck();
      else anchor = Math.max(anchor, v);
    } else if (v > anchor + 32 * unit) {
      wake();
    } else {
      anchor = Math.min(anchor, v);
    }
  }

  // Where the dock sits, give or take a fingertip. Content around it stays live while the dock is away.
  const overDock = (e) => {
    const [x, y] = local(e);
    const b = dockGlass.base;
    const pad = 14 * unit;
    return !!b && x > b.x - pad && x < b.x + b.w + pad && y > b.y - pad && y < b.y + b.h + pad;
  };
  screen.addEventListener('pointerdown', (e) => {
    suppressUntil = 0;
    if (!appOpen && chrome.target === 0 && overDock(e)) {
      e.stopPropagation();
      suppressUntil = performance.now() + 800;
      wake();
      return;
    }
    down.set(e.pointerId, true);
    clearTimeout(idleTimer);
  }, true);
  const lifted = (e) => {
    if (!down.has(e.pointerId)) return;
    const wakes = down.get(e.pointerId);
    down.delete(e.pointerId);
    if (wakes) wake(); else if (!down.size) schedule();
  };
  window.addEventListener('pointerup', lifted);
  window.addEventListener('pointercancel', lifted);
  window.addEventListener('blur', () => { down.clear(); schedule(); });
  screen.addEventListener('focusin', (e) => { if (e.target.matches(':focus-visible')) wake(); });
  // With a mouse, reaching for the bottom edge brings the dock back, as an auto-hiding dock does.
  screen.addEventListener('pointermove', (e) => {
    if (e.pointerType === 'mouse' && !e.buttons && chrome.target === 0 && local(e)[1] > H - 110 * unit) wake();
  });

  // A tap that ended a drag is not a tap.
  screen.addEventListener('click', (e) => { if (performance.now() < suppressUntil) { e.stopPropagation(); e.preventDefault(); suppressUntil = 0; } }, true);

  // Glass answers on pointer-down: it flexes and glows from the finger, then springs back.
  for (const g of glass) {
    g.el.addEventListener('pointerdown', (e) => { if (e.button > 0) return; [g.px, g.py] = local(e); g.press.to(1, { spring: SPRINGS.hold }); });
    for (const type of ['pointerup', 'pointercancel', 'pointerleave']) g.el.addEventListener(type, () => g.press.to(0, { spring: SPRINGS.release }));
  }

  // ---------- dock: a droplet lifts out of the bar under the finger, follows a drag, and melts back in ----------
  dockButtons.forEach((b) => b.addEventListener('click', (e) => {
    if (e.detail === 0) { lift.to(0.6, { spring: SPRINGS.hold }); setTimeout(() => lift.to(0, { spring: SPRINGS.wobble }), 150); }
    goSection(Number(b.dataset.section));
  }));
  const dockTrack = velocityTracker();
  let dockDrag = null;
  dock.addEventListener('pointerdown', (e) => {
    if (e.button > 0) return;
    e.stopPropagation();
    const [x] = local(e);
    const hit = dockItems.findIndex((it) => x >= it.x && x <= it.x + it.w);
    const start = hit >= 0 ? dockItems[hit].x : lens.value;
    lens.to(start, { spring: SPRINGS.hold });
    lift.to(1, { spring: SPRINGS.hold });
    dockDrag = { id: e.pointerId, grab: x - start, x0: x, moved: false };
    dockTrack.reset();
  });
  dock.addEventListener('pointermove', (e) => {
    if (!dockDrag || e.pointerId !== dockDrag.id) return;
    const [x] = local(e);
    if (!dockDrag.moved && Math.abs(x - dockDrag.x0) < 10) return;
    if (!dockDrag.moved) { dockDrag.moved = true; try { dock.setPointerCapture(e.pointerId); } catch { /* synthetic events */ } }
    const first = dockItems[0].x, last = dockItems[dockItems.length - 1].x, raw = x - dockDrag.grab;
    const shown = raw < first ? first - rubberBand(first - raw, 120 * unit) : raw > last ? last + rubberBand(raw - last, 120 * unit) : raw;
    const t = e.timeStamp / 1000;
    dockTrack.add(t, shown);
    lens.set(shown, dockTrack.velocity(t));
  });
  const endDock = (e) => {
    if (!dockDrag || e.pointerId !== dockDrag.id) return;
    const drag = dockDrag;
    dockDrag = null;
    lift.to(0, { spring: SPRINGS.wobble });
    if (!drag.moved) return;
    suppressUntil = performance.now() + 400;
    const v = dockTrack.velocity(e.timeStamp / 1000);
    const aim = lens.value + project(v, 0.99);
    let best = 0;
    dockItems.forEach((it, k) => { if (Math.abs(it.x - aim) < Math.abs(dockItems[best].x - aim)) best = k; });
    lens.to(dockItems[best].x, { velocity: v });
    if (best !== sectionOf(index)) goSection(best);
  };
  window.addEventListener('pointerup', endDock);
  window.addEventListener('pointercancel', endDock);

  // ---------- pans ----------
  // About ten pixels of hysteresis, then one axis. An element that claimed that axis gets the drag (a room's
  // dimmer, Cover Flow, a lock slider). Otherwise sideways pans page between slots, and vertical pans go to the
  // slot's own pan (standby's faces) or the overview's scroll, which coasts and resists at its edges.
  const panTrack = velocityTracker();
  let pan = null;
  screen.addEventListener('pointerdown', (e) => {
    if (e.button > 0 || pan) return;
    pan = { id: e.pointerId, x0: e.clientX, y0: e.clientY, axis: null, from: index, target: e.target, handler: null, along: 'y' };
  });
  window.addEventListener('pointermove', (e) => {
    if (!pan || e.pointerId !== pan.id) return;
    const t = e.timeStamp / 1000;
    if (!pan.axis) {
      const dx = e.clientX - pan.x0, dy = e.clientY - pan.y0;
      if (Math.hypot(dx, dy) < 10) return;
      const along = Math.abs(dx) >= Math.abs(dy) ? 'x' : 'y';
      pan.along = along;
      // the innermost element that claimed this axis wins: a lock slider inside an app beats the app's own dismiss
      const claims = dragTargets.filter((d) => d.axis === along && d.el.contains(pan.target));
      pan.handler = claims.sort((a, b) => (a.el.contains(b.el) ? 1 : b.el.contains(a.el) ? -1 : 0))[0]?.handler ?? null;
      let axis = pan.handler ? 'own' : appOpen ? 'none' : along;
      if (axis === 'y') {
        pan.handler = slotPans.get(index) ?? null;
        axis = pan.handler ? 'own' : index === 0 ? 'y' : 'none';
      }
      Object.assign(pan, { axis, x0: e.clientX, y0: e.clientY, page0: page.value, scroll0: scroll.value });
      if (axis !== 'none') { try { screen.setPointerCapture(e.pointerId); } catch { /* synthetic events */ } }
      for (const s of surfaces) s.layer.to(0);
      for (const g of glass) g.press.to(0, { spring: SPRINGS.release });
      coast.cancel();
      panTrack.reset();
      if (down.has(e.pointerId) && (axis === 'y' || pan.handler?.quiet)) down.set(e.pointerId, false);
      pan.handler?.begin(pan.target, t);
    }
    if (pan.axis === 'x') {
      const raw = pan.page0 + (e.clientX - pan.x0), min = -(SLOTS - 1) * W;
      const shown = raw > 0 ? rubberBand(raw, W) : raw < min ? min - rubberBand(min - raw, W) : raw;
      panTrack.add(t, shown);
      page.set(shown, panTrack.velocity(t));
    } else if (pan.axis === 'y') {
      const raw = pan.scroll0 + (e.clientY - pan.y0), min = -scrollMax;
      const shown = raw > 0 ? rubberBand(raw, H) : raw < min ? min - rubberBand(min - raw, H) : raw;
      panTrack.add(t, shown);
      scroll.set(shown, panTrack.velocity(t));
    } else if (pan.axis === 'own') {
      pan.handler.move(pan.along === 'x' ? e.clientX - pan.x0 : e.clientY - pan.y0, t);
    }
  });
  const endPan = (e) => {
    if (!pan || e.pointerId !== pan.id) return;
    const p = pan;
    pan = null;
    if (!p.axis) return;
    suppressUntil = performance.now() + 400;
    const t = e.timeStamp / 1000;
    if (p.axis === 'x') {
      const v = panTrack.velocity(t);
      go(pageFor(page.value, v, W, p.from, SLOTS), v);
    } else if (p.axis === 'y') {
      coast.release(panTrack.velocity(t));
    } else if (p.axis === 'own') {
      p.handler.end(t);
    }
  };
  window.addEventListener('pointerup', endPan);
  window.addEventListener('pointercancel', endPan);
  screen.addEventListener('wheel', (e) => {
    if (index !== 0 || appOpen) return;
    const from = scroll.moving ? scroll.target : scroll.value;
    const next = clamp(from - e.deltaY, -scrollMax, 0);
    if (next === from) return;
    e.preventDefault();
    coast.cancel();
    scroll.to(next, { spring: SPRINGS.release });
  }, { passive: false });

  // ---------- frame ----------
  function frame(t, dt) {
    const extra = {};
    for (const hook of hooks.frame) Object.assign(extra, hook(t, dt) || {});
    watchScroll();
    apply();
    const visible = (r) => r.x < W && r.x + r.w > 0 && r.y < H && r.y + r.h > 0;
    const win = appWindow();
    // An app's own elements scale with its window: measured full-screen, drawn inside the window's rect.
    const sx = win ? win.w / W : 1;
    const sy = win ? win.h / H : 1;
    const inWindow = (r) => ({ x: win.x + r.x * sx, y: win.y + r.y * sy, w: r.w * sx, h: r.h * sy });

    const cc = controlCenter();
    const pageSurf = [];
    const appSurf = [];
    const ccSurf = [];
    for (const s of surfaces) {
      if (s.cc ? !cc : s.app ? !win?.el?.contains(s.el) : win?.full) continue;
      const dx = s.slot >= 0 ? page.value : 0;
      const dy = s.home ? scroll.value : 0;
      const g = s.geometry ? s.geometry() : { ...s.base, r: s.radius.value };
      if (g.w < 0.5 || g.h < 0.5) continue;
      const c = s.clip;
      let r = { x: g.x + dx, y: g.y + dy, w: g.w, h: g.h };
      let radius = g.r;
      let clipRect = c ? { x: c.x + dx, y: c.y + dy, w: c.w, h: c.h, r: c.surface ? c.surface.radius.value : c.r } : null;
      if (s.app) {
        r = inWindow(r);
        radius *= Math.min(sx, sy);
        if (clipRect) clipRect = { ...inWindow(clipRect), r: clipRect.r * Math.min(sx, sy) };
      }
      if (!visible(r)) continue;
      (s.cc ? ccSurf : s.app ? appSurf : pageSurf).push({
        ...r, r: radius, color: mixRgb(s.from, s.to, s.fade.value), alpha: s.alpha ? s.alpha() : 1,
        layer: s.layer.value, lx: s.lx, ly: s.ly, ink: s.ink, clip: clipRect,
      });
    }
    const surfSplit = pageSurf.length;
    const surfSplit2 = surfSplit + appSurf.length;

    // Two page layers at most are ever on screen; standby's faces take the next two textures, the app window the last.
    const draws = [null, null, null, null, null];
    let n = 0;
    if (!win?.full) {
      for (const r of rasters) {
        const slotX = r.slot * W + page.value;
        if (n > 1 || slotX >= W || slotX + W <= 0) continue;
        draws[n++] = { name: r.name, x: r.origin.x + page.value, y: r.origin.y + (r.slot === 0 ? scroll.value : 0), w: r.raster.width, h: r.raster.height };
      }
      (extra.scapeDraws ?? []).slice(0, 2).forEach((d, k) => { draws[2 + k] = d; });
    }
    if (win && extra.appDraw) draws[4] = { ...extra.appDraw, ...inWindow(extra.appDraw) };

    const fade = chrome.value;
    const onHome = clamp(HOME_PAGES + page.value / W, 0, 1);
    const gls = [];
    for (const g of glass) {
      if (g.app && !win?.el?.contains(g.el)) continue;
      if (g.cc && !cc) continue;
      const k = 1 + g.gel * g.press.value;
      let b = g.geometry ? g.geometry() : { ...g.base, r: g.r };
      b = { ...b, x: b.x + g.dx, y: b.y + g.dy };
      if (g.app) b = { ...inWindow(b), r: b.r * Math.min(sx, sy) };
      const strength = g.strength.value * (g === dockGlass ? fade : g === pagerGlass ? fade * onHome : 1) * (g.app ? win.glass ?? 1 : 1) * (g.cc ? cc.glass ?? 1 : 1 - (cc?.progress ?? 0));
      const r = { x: b.x + (g.slot >= 0 ? page.value : 0) - (b.w * (k - 1)) / 2, y: b.y - (b.h * (k - 1)) / 2, w: b.w * k, h: b.h * k };
      if (strength > 0.002 && visible(r)) {
        gls.push({ ...r, r: b.r * k, strength, press: g.press.value, tint: g.tint.value, hue: g.hue ?? 0, px: g.px, py: g.py, group: g.group });
      }
    }

    // The droplet swells out of the bar when lifted and stretches along its motion, keeping its volume.
    const item = dockItems[sectionOf(index)];
    const speed = settings.calm ? 0 : Math.abs(lens.velocity);
    const st = Math.min(speed * 0.04, 30 * unit);
    const lf = settings.calm ? Math.min(lift.value, 0.3) : lift.value;
    const dw = item.w * (1 + 0.12 * lf) + st;
    const dh = (item.h * (1 + 0.42 * lf)) / (1 + st / (item.w * 1.6));
    const dcx = lens.value + item.w / 2;
    const dcy = item.y + dockGlass.dy + item.h / 2 - 2 * unit * lf;
    const drop = { x: dcx - dw / 2, y: dcy - dh / 2, w: dw, h: dh, r: dh / 2 };
    const dockStrength = dockGlass.strength.value * fade;
    if ((lf > 0.01 || st > 1) && dockStrength > 0.002) gls.push({ ...drop, strength: dockStrength, press: lf * 0.5, tint: 0, px: dcx, py: dcy, group: 0 });

    return {
      ...extra,
      page: page.value,
      scroll: scroll.value,
      edge: fade,
      surfaces: pageSurf.concat(appSurf),
      // the control center's fills are drawn inside its glass, over the blurred page
      overlays: ccSurf,
      back: cc?.progress ?? 0,
      surfSplit,
      surfSplit2,
      dim: cc?.dim ?? 0,
      glass: gls,
      lens: { ...drop, amt: Math.max(lf, Math.min(speed / 1400, 0.6)) },
      draws,
      win: win ? { x: win.x, y: win.y, w: win.w, h: win.h, r: win.r, color: win.color, shadow: win.shadow } : null,
      disc: [ring.x, ring.y, 168 * unit, extra.discAngle ?? 0],
      ring: [ring.x, ring.y, 186 * unit, extra.progress ?? 0],
      dial: extra.dial ?? null,
      dialColor: extra.dialColor ?? null,
      scapeF: extra.scapeF ?? 0,
      clock: extra.clock ?? [0, 0, 0, 0],
      digits: extra.digits ?? [0, 0, 0, 0],
      sphere: win?.full ? null : extra.sphere ?? null,
    };
  }

  const ctx = {
    screen, settings,
    get W() { return W; }, get H() { return H; }, get unit() { return unit; },
    get index() { return index; }, get page() { return page.value; }, get section() { return sectionOf(index); },
    get appOpen() { return appOpen; },
    surface: (el) => surfaceMap.get(el),
    glass: (el) => glass.find((g) => g.el === el),
    local, dirty, upload, go, wake, setCover,
    recolor: (el) => { const s = surfaceMap.get(el); if (s) recolor(s, true); },
    reshape: (el) => { const s = surfaceMap.get(el); if (s) reshape(s); },
    suppressClick: () => { suppressUntil = performance.now() + 400; },
    onMeasure: (hook) => hooks.measure.push(hook),
    onFrame: (fn) => hooks.frame.push(fn),
    onRetone: (fn) => hooks.retone.push(fn),
    /** Called with (index, from) whenever a slot becomes current. */
    onIndex: (fn) => hooks.index.push(fn),
    /** A slot's own vertical pan: { begin(target, t), move(d, t), end(t), quiet } — quiet leaves the chrome away. */
    setVerticalPan: (slot, handler) => slotPans.set(slot, handler),
    /** An element that takes drags along one axis that start on it, with the same handler shape. */
    addDragTarget: (el, handler, axis = 'y') => dragTargets.push({ el, handler, axis }),
    /** The app window for this frame, { x, y, w, h, r, color, shadow, full, glass } or null, from apps.js. */
    setAppWindow: (fn) => { appWindow = fn; },
    /** The control center for this frame, { progress, dim, glass } or null, from cc.js. */
    setControlCenter: (fn) => { controlCenter = fn; },
    setAppOpen: (open) => {
      appOpen = open;
      if (open) tuck(true); else wake();
    },
    setExtraDirty: (fn) => { extraDirty = fn; },
    toast: (text, icon) => ctx.showToast?.(text, icon),
    measure: () => { measure(); go(index, 0, false); },
    /** Measures only what is inside root: an app that just became visible, without repainting every page. */
    measureWithin(root) {
      const box = screen.getBoundingClientRect();
      const rel = (el) => { const r = el.getBoundingClientRect(); return { x: r.left - box.left, y: r.top - box.top, w: r.width, h: r.height }; };
      for (const s of surfaces) {
        if (!root.contains(s.el)) continue;
        s.base = rel(s.el);
        s.r0 = radiusOf(s.el);
        if (!s.radius.moving) s.radius.set(s.r0);
        s.clip = s.clipEl ? { ...rel(s.clipEl), r: radiusOf(s.clipEl), surface: surfaceMap.get(s.clipEl) ?? null } : null;
      }
      for (const g of glass) if (root.contains(g.el)) { g.base = rel(g.el); g.r = radiusOf(g.el); }
    },
    /** How long home and apps wait before the chrome steps back; 0 keeps it. */
    setIdle(ms) {
      IDLE[0] = ms;
      IDLE[1] = ms;
      IDLE[3] = ms ? Math.min(ms, 2500) : 0;
      if (ms) schedule(); else wake();
    },
  };

  schedule();

  return {
    ctx,
    measure: () => { measure(); go(index, 0, false); },
    frame,
    drainUploads: () => uploads.splice(0),
    retone() {
      for (const s of surfaces) recolor(s, true);
      for (const r of rasters) { r.raster.draw(); upload(r.name, r.raster.canvas, null); }
      for (const fn of hooks.retone) fn();
    },
  };
}
