// Controls on the overview, now playing and standby: press responses, the light slider, scenes, the stepper,
// media, the standby actions and the island. Every response starts on pointer-down and settles on a Detent
// spring (press feedback pattern). The house itself, its lights and its setpoint, lives in ctx.home, so every
// page shows the same values.

import { Motion, SPRINGS, rubberBand, velocityTracker } from './motion.js';
import { ALBUMS } from './scenes.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const clock = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;

const ROOMS = ['living', 'kitchen', 'bedroom', 'office', 'hallway', 'bathroom'];
// The overview's "main" light is the living room's.
const SCENES = {
  meeting: { rooms: { living: 1 }, setpoint: 21, text: 'meeting · main 100% · 21.0°', icon: 'groups' },
  coffee: { rooms: { living: 0.72, kitchen: 0.8 }, setpoint: 21.5, text: 'coffee · main 72% · 21.5°', icon: 'coffee' },
  focus: { rooms: { living: 0.45, office: 0.8 }, setpoint: 20.5, text: 'focus · main 45% · desk 80%', icon: 'center_focus_strong' },
  away: { rooms: null, setpoint: 18, text: 'away · all lights off · 18.0°', icon: 'luggage' },
};
// One song from each album in the library, in the same order, so a track and its cover always match.
const TRACKS = ALBUMS.map((a) => ({ title: a.single, by: a.artist, album: a.title.toLowerCase(), length: a.length }));

/** A state layer from the touch point and a shape morph on a Material surface; `also` mirrors the layer. */
export function pressSurface(ctx, el, morph, also = []) {
  const s = ctx.surface(el);
  const layers = [s, ...also.map((a) => ctx.surface(a))];
  let hovering = false;
  const layer = (v, spring, e) => {
    for (const l of layers) {
      if (e) [l.lx, l.ly] = ctx.local(e);
      l.layer.to(v, { spring });
    }
  };
  // a button inside the surface answers for itself
  const own = (e) => { const inner = e.target.closest('button, [role="slider"]'); return !inner || inner === el; };
  el.addEventListener('pointerenter', (e) => {
    if (e.pointerType !== 'mouse') return;
    hovering = true;
    layer(0.06, SPRINGS.effect, e);
  });
  el.addEventListener('pointerleave', () => {
    hovering = false;
    layer(0, SPRINGS.effect);
    s.radius.to(s.r0, { spring: SPRINGS.release });
  });
  el.addEventListener('pointerdown', (e) => {
    if (e.button > 0 || !own(e)) return;
    layer(0.12, SPRINGS.layer, e);
    s.radius.to(s.r0 * morph, { spring: SPRINGS.hold });
  });
  for (const type of ['pointerup', 'pointercancel']) {
    el.addEventListener(type, () => {
      layer(hovering ? 0.06 : 0, SPRINGS.effect);
      s.radius.to(s.r0, { spring: SPRINGS.release });
    });
  }
}

/** A state layer on a control that sits on glass, drawn by CSS through --layer. */
export function pressLayer(el) {
  const m = new Motion(0, { spring: SPRINGS.layer, epsilon: 0.002, onChange: (v) => el.style.setProperty('--layer', v.toFixed(3)) });
  let hovering = false;
  el.addEventListener('pointerenter', (e) => { if (e.pointerType === 'mouse') { hovering = true; m.to(0.05, { spring: SPRINGS.effect }); } });
  el.addEventListener('pointerleave', () => { hovering = false; m.to(0, { spring: SPRINGS.effect }); });
  el.addEventListener('pointerdown', (e) => { if (e.button <= 0) m.to(0.12, { spring: SPRINGS.layer }); });
  for (const type of ['pointerup', 'pointercancel']) el.addEventListener(type, () => m.to(hovering ? 0.05 : 0, { spring: SPRINGS.effect }));
}

export function wireControls(ctx) {
  const { screen, settings } = ctx;
  const q = (selector) => screen.querySelector(selector);

  const levels = Object.fromEntries(ROOMS.map((id) => [id, new Motion(Number(q(`#room-${id}`).dataset.level), { spring: SPRINGS.settle, epsilon: 0.0005 })]));
  const setpoint = new Motion(21.5, { spring: SPRINGS.settle, epsilon: 0.001 });
  const volume = new Motion(0.38, { spring: SPRINGS.settle, epsilon: 0.0005 });
  ctx.home = { levels, setpoint, volume };

  for (const el of screen.querySelectorAll('.chip')) pressSurface(ctx, el, 0.7);
  for (const el of screen.querySelectorAll('.step, .fab')) pressSurface(ctx, el, 0.6);
  // A tile that leads to a page answers like a button and opens it; the buttons on it stay its own.
  for (const el of screen.querySelectorAll('.tile[data-go]')) {
    pressSurface(ctx, el, 0.85);
    el.addEventListener('click', (e) => { if (!e.target.closest('button')) ctx.go(Number(el.dataset.go)); });
  }
  for (const el of screen.querySelectorAll('.transport button, .pill, .dock-item')) pressLayer(el);

  // ---------- the light slider: the living room's main light ----------
  const level = q('#level');
  const on = q('#level-on');
  const off = q('#level-off');
  const handle = q('#level-handle');
  const stop = q('#level-stop');
  const valueEl = q('#level-value');
  const value = levels.living;
  const held = new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 });
  const stretch = new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 });
  const GAP = 6;
  const HANDLE = 6;
  let geo = { x: 0, y: 0, w: 1, h: 1 };
  let stopLeft = 0, stopHidden = null, lastPct = -1;

  function layout() {
    const u = ctx.unit;
    const hw = (HANDLE - 2 * held.value) * u;
    const gap = GAP * u;
    const v = value.value;
    const shown = v < 0 ? -rubberBand(-v, 1) * 0.12 : v > 1 ? 1 + rubberBand(v - 1, 1) * 0.12 : v;
    const cx = gap + hw / 2 + shown * (geo.w - 2 * gap - hw);
    const hh = Math.min(geo.h - 12 * u, geo.h - 40 * u + 16 * u * held.value + stretch.value * geo.h * 0.25);
    return { cx, hw, gap, hh, u };
  }
  // Both segments reach 40 px past the track so the track's clip draws their outer corners. The active one
  // takes ink: the label turns its on-colour exactly where the fill passes under it.
  ctx.surface(on).geometry = () => {
    const { cx, hw, gap, u } = layout();
    return { x: geo.x - 40 * u, y: geo.y, w: Math.max(cx - hw / 2 - gap, 0) + 40 * u, h: geo.h, r: 8 * u };
  };
  ctx.surface(off).geometry = () => {
    const { cx, hw, gap, u } = layout();
    const x = geo.x + cx + hw / 2 + gap;
    return { x, y: geo.y, w: Math.max(geo.x + geo.w + 40 * u - x, 0), h: geo.h, r: 8 * u };
  };
  ctx.surface(handle).geometry = () => {
    const { cx, hw, hh } = layout();
    return { x: geo.x + cx - hw / 2, y: geo.y + (geo.h - hh) / 2, w: hw, h: hh, r: hw / 2 };
  };

  function syncLevel() {
    const { cx, hw, gap, hh, u } = layout();
    on.style.left = `${-40 * u}px`;
    on.style.width = `${Math.max(cx - hw / 2 - gap, 0) + 40 * u}px`;
    off.style.left = `${cx + hw / 2 + gap}px`;
    off.style.width = `${Math.max(geo.w + 40 * u - cx - hw / 2 - gap, 0)}px`;
    Object.assign(handle.style, { left: `${cx - hw / 2}px`, width: `${hw}px`, top: `${(geo.h - hh) / 2}px`, height: `${hh}px` });
    const hide = cx + hw / 2 + gap > stopLeft - 4 * u;
    if (hide !== stopHidden) { stopHidden = hide; stop.classList.toggle('is-hidden', hide); ctx.dirty(stop); }
    const pct = Math.round(clamp(value.value, 0, 1) * 100);
    if (pct !== lastPct) { lastPct = pct; valueEl.textContent = `${pct}%`; level.setAttribute('aria-valuenow', String(pct)); ctx.dirty(valueEl); }
  }

  const levelTrack = velocityTracker();
  let drag = null;
  level.addEventListener('pointerdown', (e) => {
    if (e.button > 0) return;
    e.stopPropagation();
    try { level.setPointerCapture(e.pointerId); } catch { /* synthetic events */ }
    const r = level.getBoundingClientRect();
    const { cx, u } = layout();
    const x = e.clientX - r.left;
    const span = r.width - 2 * GAP * u - HANDLE * u;
    const toValue = (px) => (px - GAP * u - (HANDLE * u) / 2) / span;
    const grab = Math.abs(x - cx) < 28 * u;
    drag = { id: e.pointerId, left: r.left, toValue, offset: grab ? x - cx : 0, x0: e.clientX, moved: false };
    held.to(1, { spring: SPRINGS.hold });
    levelTrack.reset();
    // Pressing the track moves the handle there; grabbing the handle keeps the grab offset.
    if (!grab) value.to(clamp(toValue(x), 0, 1), { spring: SPRINGS.settle });
  });
  level.addEventListener('pointermove', (e) => {
    if (!drag || e.pointerId !== drag.id) return;
    if (!drag.moved && Math.abs(e.clientX - drag.x0) < 3) return;
    drag.moved = true;
    const t = e.timeStamp / 1000;
    const v = drag.toValue(e.clientX - drag.left - drag.offset);
    levelTrack.add(t, v);
    const speed = levelTrack.velocity(t);
    value.set(v, speed);
    stretch.set(settings.calm ? 0 : Math.min(Math.abs(speed) * 0.08, 0.35));
  });
  const endLevel = (e) => {
    if (!drag || e.pointerId !== drag.id) return;
    drag = null;
    held.to(0, { spring: SPRINGS.release });
    stretch.to(0, { spring: SPRINGS.release });
    if (value.value < 0 || value.value > 1) value.to(clamp(value.value, 0, 1), { spring: SPRINGS.release, velocity: 0 });
  };
  level.addEventListener('pointerup', endLevel);
  level.addEventListener('pointercancel', endLevel);
  level.addEventListener('keydown', (e) => {
    const step = { ArrowRight: 0.05, ArrowUp: 0.05, ArrowLeft: -0.05, ArrowDown: -0.05, PageUp: 0.1, PageDown: -0.1 }[e.key];
    const next = step !== undefined ? clamp(value.target + step, 0, 1) : e.key === 'Home' ? 0 : e.key === 'End' ? 1 : null;
    if (next === null) return;
    e.preventDefault();
    value.set(next); // keyboard changes land at once; animating them is a press feedback trap
  });

  // ---------- climate tile: steps land on pointer-down and repeat while held ----------
  const setEl = q('#climate-set');
  let shownSet = '';
  function syncSetpoint() {
    const text = `${(Math.round(clamp(setpoint.value, 16, 28) * 2) / 2).toFixed(1)}°`;
    if (text === shownSet) return;
    shownSet = text;
    setEl.textContent = text;
    ctx.dirty(setEl);
  }
  for (const b of screen.querySelectorAll('.step')) {
    let timer = 0;
    const step = () => setpoint.to(clamp(Math.round((setpoint.target + Number(b.dataset.step)) * 2) / 2, 16, 28), { spring: SPRINGS.release });
    const stopRepeat = () => { clearTimeout(timer); timer = 0; };
    b.addEventListener('pointerdown', (e) => {
      if (e.button > 0) return;
      step();
      stopRepeat();
      let repeats = 0;
      const again = () => { repeats += 1; step(); timer = setTimeout(again, Math.max(60, 150 - repeats * 12)); };
      timer = setTimeout(again, 420);
    });
    for (const type of ['pointerup', 'pointercancel', 'pointerleave']) b.addEventListener(type, stopRepeat);
    b.addEventListener('click', (e) => { if (e.detail === 0) step(); });
  }

  // ---------- scenes ----------
  const chips = [...screen.querySelectorAll('.chip')];
  function applyScene(name, announce = true) {
    const scene = SCENES[name];
    for (const chip of chips) {
      const isOn = chip.dataset.scene === name;
      if (chip.classList.contains('is-on') === isOn) continue;
      chip.classList.toggle('is-on', isOn);
      chip.setAttribute('aria-pressed', String(isOn));
      ctx.recolor(chip);
      ctx.dirty(chip);
    }
    for (const id of ROOMS) {
      const to = scene.rooms ? scene.rooms[id] : 0;
      if (to !== undefined) levels[id].to(to, { spring: SPRINGS.settle });
    }
    setpoint.to(scene.setpoint, { spring: SPRINGS.settle });
    if (announce) ctx.toast(scene.text, scene.icon);
  }
  chips.forEach((chip) => chip.addEventListener('click', () => applyScene(chip.dataset.scene)));
  ctx.applyScene = applyScene;

  // ---------- media ----------
  let track = 0;
  let elapsed = 74;
  let angle = 0;
  let lastSec = -1;
  const spin = new Motion(0, { spring: SPRINGS.release, epsilon: 0.001 });
  const playButtons = [q('#media-play'), q('#np-play')];
  ctx.togglePlay = () => playButtons[0].click();
  ctx.nowPlaying = () => ({ ...TRACKS[track], index: track, playing: settings.playing, elapsed });
  playButtons.forEach((b) => b.addEventListener('click', () => {
    settings.playing = !settings.playing;
    for (const p of playButtons) {
      p.querySelector('.icon').textContent = settings.playing ? 'pause' : 'play_arrow';
      p.setAttribute('aria-label', settings.playing ? 'Pause' : 'Play');
    }
    ctx.dirty(playButtons[0]);
  }));
  function setTrack(i, direction) {
    track = (i + TRACKS.length) % TRACKS.length;
    const t = TRACKS[track];
    q('#media-title').textContent = t.title;
    q('#media-by').textContent = t.by;
    q('#np-title').textContent = t.title;
    q('#np-by').textContent = `${t.by} · ${t.album}`;
    for (const id of ['#media-title', '#media-by', '#np-title', '#np-by']) ctx.dirty(q(id));
    elapsed = 0;
    spin.to(spin.target + direction * 0.9, { spring: SPRINGS.release, velocity: direction * 9 });
    ctx.trackChanged?.(track);
  }
  ctx.skipTrack = (dir) => setTrack(track + dir, dir);
  ctx.playTrack = (k) => {
    if (k !== track) setTrack(k, k > track ? 1 : -1);
    if (!settings.playing) playButtons[0].click();
  };
  screen.querySelectorAll('[data-skip]').forEach((b) => b.addEventListener('click', () => setTrack(track + Number(b.dataset.skip), Number(b.dataset.skip))));

  // ---------- standby actions ----------
  const pills = [q('#pill-back'), q('#pill-out')];
  pills.forEach((p) => p.addEventListener('click', () => {
    for (const o of pills) {
      const isOn = o === p;
      ctx.glass(o).tint.to(isOn ? 1 : 0, { spring: SPRINGS.effect });
      o.setAttribute('aria-pressed', String(isOn));
    }
    if (p.id === 'pill-back') { applyScene('coffee', false); ctx.toast('welcome back · lights on · 21.5°', 'waving_hand'); }
    else { applyScene('away', false); ctx.toast('see you later · lights off · 18.0°', 'luggage'); }
  }));

  // ---------- the island: a message grows out of a drop of glass at the top edge and shrinks back into it ----------
  const island = q('#toast');
  const islandGlass = ctx.glass(island);
  const islandText = q('#toast-text');
  const islandIcon = q('#toast-icon');
  const grow = new Motion(0, { spring: SPRINGS.release, epsilon: 0.001 });
  const width = new Motion(0, { spring: SPRINGS.release, epsilon: 0.1 });
  const swap = new Motion(1, { spring: SPRINGS.effect, epsilon: 0.002 });
  let islandBase = null;
  let hideTimer = 0;
  const measureIsland = () => { islandBase = { cx: ctx.W / 2, y: island.offsetTop, h: island.offsetHeight }; };
  islandGlass.geometry = () => {
    if (!islandBase) return { x: 0, y: 0, w: 0, h: 0, r: 0 };
    const k = Math.max(grow.value, 0);
    const h = islandBase.h * (0.58 + 0.42 * k);
    const w = Math.max(width.value * (0.3 + 0.7 * k), h);
    const y = islandBase.y + (islandBase.h - h) / 2 - (1 - Math.min(k, 1)) * 8 * ctx.unit;
    return { x: islandBase.cx - w / 2, y, w, h, r: h / 2 };
  };
  ctx.showToast = (text, icon) => {
    // quiet keeps the island still, except for timers, which you asked to hear about
    if (settings.quiet && icon !== 'timer') return;
    const showing = grow.target > 0;
    islandText.textContent = text;
    islandIcon.textContent = icon || 'check';
    measureIsland();
    // A new message while one is showing reshapes the island and swaps its words, rather than starting over.
    if (showing) {
      width.to(island.offsetWidth, { spring: SPRINGS.release });
      swap.set(0);
      swap.to(1, { spring: SPRINGS.effect });
    } else {
      width.set(island.offsetWidth);
    }
    clearTimeout(hideTimer);
    islandGlass.strength.to(1, { spring: SPRINGS.layer });
    grow.to(1, { spring: SPRINGS.release });
    hideTimer = setTimeout(() => {
      grow.to(0, { spring: SPRINGS.smooth });
      hideTimer = setTimeout(() => islandGlass.strength.to(0, { spring: SPRINGS.effect }), 240);
    }, 2400);
  };

  ctx.onMeasure({
    after(rel) {
      geo = rel(level);
      stopLeft = rel(stop).x - geo.x;
      stopHidden = null;
      lastPct = -1;
      shownSet = '';
      measureIsland();
      if (grow.target === 0) width.set(island.offsetWidth);
      syncLevel();
    },
  });

  ctx.onFrame((t, dt) => {
    syncLevel();
    syncSetpoint();
    const length = TRACKS[track].length;
    if (settings.playing) {
      if (!settings.calm) angle += dt * 0.32;
      elapsed += dt;
      if (elapsed >= length) setTrack(ctx.music?.next(track, TRACKS.length) ?? track + 1, 1);
    }
    const sec = Math.floor(elapsed);
    if (sec !== lastSec) {
      lastSec = sec;
      q('#np-elapsed').textContent = clock(elapsed);
      q('#np-left').textContent = `−${clock(Math.max(0, TRACKS[track].length - elapsed))}`;
      ctx.dirty(q('#np-elapsed'));
      ctx.dirty(q('#np-left'));
    }
    island.style.opacity = String(clamp((grow.value - 0.55) / 0.45, 0, 1) * swap.value);
    return { discAngle: angle + spin.value, progress: elapsed / TRACKS[track].length };
  });
}
