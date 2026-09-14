// The small household apps: timers that keep running when you leave, a front door you slide to lock, a
// shopping list, and settings for the panel itself. Presses answer on pointer-down (press feedback pattern);
// the lock and a swiped list item follow the finger 1:1 and hand their release velocity to a Detent spring.

import { pressSurface } from './controls.js';
import { Motion, SPRINGS, project, rubberBand, velocityTracker } from './motion.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const clock = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
const hm = (d = new Date()) => `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;

// ---------- timers ----------
const NAMES = { 60: 'tea', 180: 'eggs', 300: 'pasta', 600: 'rolls' };

export function createTimers(ctx) {
  const { screen } = ctx;
  const q = (s) => screen.querySelector(s);
  const rows = [...screen.querySelectorAll('.timer-row')].map((el) => ({
    el, fill: el.querySelector('.timer-fill'), name: el.querySelector('.timer-name'), left: el.querySelector('.timer-left'), timer: null, shown: '',
  }));
  const sub = q('#timers-sub');
  const empty = q('#timer-empty');
  const namesEl = q('#timer-names');
  const chips = [...screen.querySelectorAll('[data-timer-name]')];
  let open = false;
  let target = null; // the timer the name chips rename: the one added or touched last
  let added = 0;

  const remaining = (timer) => Math.max(0, timer.paused ? timer.left : timer.ends - Date.now()); // ms
  const running = () => rows.filter((r) => r.timer && !r.timer.done && !r.timer.paused);

  function label(r) {
    if (!r.timer) return;
    r.name.textContent = `${r.timer.label} · ${r.timer.paused ? 'paused' : clock(r.timer.total)}`;
    ctx.dirty(r.name);
  }
  function describe() {
    const n = running().length;
    const p = rows.filter((r) => r.timer?.paused).length;
    sub.textContent = n || p ? [n && `${n} running`, p && `${p} paused`].filter(Boolean).join(' · ') : rows.some((r) => r.timer) ? 'all done' : 'none running';
    ctx.dirty(sub);
  }
  // Returns whether a row appeared or disappeared, so the caller repaints the window.
  function syncNames() {
    if (!target?.timer) target = rows.filter((r) => r.timer).sort((a, b) => b.timer.added - a.timer.added)[0] ?? null;
    const any = Boolean(target);
    let changed = false;
    if (namesEl && namesEl.hidden === any) { namesEl.hidden = !any; changed = true; }
    if (empty.hidden !== any) { empty.hidden = any; changed = true; }
    for (const r of rows) {
      const on = r === target;
      if (r.el.classList.contains('is-target') !== on) { r.el.classList.toggle('is-target', on); ctx.dirty(r.name); }
    }
    for (const chip of chips) {
      const on = Boolean(target && target.timer.label === chip.dataset.timerName);
      if (chip.classList.contains('is-on') === on) continue;
      chip.classList.toggle('is-on', on);
      chip.setAttribute('aria-pressed', String(on));
      ctx.recolor(chip);
      ctx.dirty(chip);
    }
    return changed;
  }
  function layoutChanged() { describe(); syncNames(); ctx.appRepaint?.(); }
  function select(r) {
    if (!r.timer) return;
    target = r;
    if (syncNames()) ctx.appRepaint?.();
  }
  function togglePause(r) {
    const t = r.timer;
    if (!t || t.done) return;
    if (t.paused) { t.ends = Date.now() + t.left; t.paused = false; }
    else { t.left = Math.max(0, t.ends - Date.now()); t.paused = true; }
    r.el.classList.toggle('is-paused', t.paused);
    ctx.recolor(r.fill);
    r.shown = '';
    label(r);
    describe();
  }

  for (const r of rows) {
    const fill = ctx.surface(r.fill);
    fill.geometry = () => {
      const c = fill.clip;
      if (!c || !r.timer) return { x: 0, y: 0, w: 0, h: 0, r: 0 };
      const share = clamp(remaining(r.timer) / (r.timer.total * 1000), 0, 1);
      return { x: c.x, y: c.y, w: c.w * share, h: c.h, r: 0 };
    };
    // The row answers on pointer-down, its fill with it. A tap pauses or resumes; a tap on the name picks the
    // timer for the name chips without pausing it.
    pressSurface(ctx, r.el, 0.92, [r.fill]);
    r.el.addEventListener('click', (e) => {
      if (!r.timer) return;
      if (e.target.closest('.timer-name')) { select(r); return; }
      if (e.target.closest('button')) return;
      select(r);
      togglePause(r);
    });
    const more = r.el.querySelector('[data-more]');
    pressSurface(ctx, more, 0.6);
    more.addEventListener('click', () => {
      const t = r.timer;
      if (!t) return;
      if (t.paused) t.left += 60000;
      else t.ends = Math.max(t.ends, Date.now()) + 60000;
      t.total += 60;
      t.done = false;
      r.el.classList.remove('is-done');
      r.shown = '';
      select(r);
      label(r);
      describe();
    });
    r.el.querySelector('[data-remove]').addEventListener('click', () => {
      r.timer = null;
      r.el.hidden = true;
      r.el.classList.remove('is-done', 'is-paused', 'is-target');
      ctx.recolor(r.fill);
      if (target === r) target = null;
      layoutChanged();
    });
  }
  for (const b of screen.querySelectorAll('[data-add]')) {
    pressSurface(ctx, b, 0.7);
    b.addEventListener('click', () => {
      const slot = rows.find((r) => !r.timer);
      if (!slot) { ctx.toast('three timers at once is the limit', 'timer'); return; }
      const total = Number(b.dataset.add);
      slot.timer = { total, ends: Date.now() + total * 1000, done: false, paused: false, left: 0, label: NAMES[total] ?? 'timer', added: ++added };
      slot.el.hidden = false;
      slot.el.classList.remove('is-done', 'is-paused');
      ctx.recolor(slot.fill);
      slot.shown = '';
      target = slot;
      label(slot);
      layoutChanged();
    });
  }
  for (const chip of chips) {
    pressSurface(ctx, chip, 0.7);
    chip.addEventListener('click', () => {
      if (!target?.timer) return;
      target.timer.label = chip.dataset.timerName;
      label(target);
      syncNames();
    });
  }

  ctx.timers = {
    add: (seconds) => screen.querySelector(`[data-add="${seconds}"]`)?.click(),
    /** Pauses or resumes the timer with this row id, as a tap on its row does. */
    toggle: (id) => { const r = rows.find((x) => x.el.id === id); if (r) togglePause(r); },
  };
  /** Every timer, for an activity stack, in row order: { id, label, left, total, paused, done }, in seconds. */
  ctx.timersList = () => rows.filter((r) => r.timer).map((r) => ({
    id: r.el.id, label: r.timer.label, left: remaining(r.timer) / 1000, total: r.timer.total, paused: r.timer.paused, done: r.timer.done,
  }));

  // Timers keep time whether or not the app is open, and say so in the island when one is done.
  ctx.onFrame(() => {
    for (const r of rows) {
      if (!r.timer) continue;
      const left = remaining(r.timer);
      if (!r.timer.done && !r.timer.paused && left <= 0) {
        r.timer.done = true;
        r.el.classList.add('is-done');
        ctx.toast(`${r.timer.label} ${r.timer.label.endsWith('s') ? 'are' : 'is'} ready`, 'timer');
        describe();
        if (open) ctx.dirty(r.el);
      }
      const text = r.timer.done ? 'done' : clock(Math.ceil(left / 1000));
      if (open && text !== r.shown) { r.shown = text; r.left.textContent = text; ctx.dirty(r.left); }
    }
    return {};
  });

  return {
    open() {
      open = true;
      for (const r of rows) r.shown = '';
      describe();
      if (syncNames()) ctx.appRepaint?.();
    },
    close() {
      open = false;
      const next = running().sort((a, b) => a.timer.ends - b.timer.ends)[0];
      if (next) ctx.toast(`${next.timer.label} · ${clock(Math.ceil((next.timer.ends - Date.now()) / 1000))} left`, 'timer');
    },
  };
}

// ---------- security ----------
export function createSecurity(ctx) {
  const { screen, settings } = ctx;
  const q = (s) => screen.querySelector(s);
  const track = q('#lock-track');
  const knob = ctx.glass(q('#lock-knob'));
  const slide = new Motion(0, { spring: SPRINGS.release, epsilon: 0.001 });
  const tracker = velocityTracker();
  const modes = [...screen.querySelectorAll('[data-alarm]')];
  const eventRows = [...screen.querySelectorAll('#events .event-row')];
  let locked = true;
  let from = 0;
  let travel = 1;

  function log(text) {
    for (let i = eventRows.length - 1; i > 0; i--) {
      eventRows[i].children[0].textContent = eventRows[i - 1].children[0].textContent;
      eventRows[i].children[1].textContent = eventRows[i - 1].children[1].textContent;
    }
    eventRows[0].children[0].textContent = hm();
    eventRows[0].children[1].textContent = text;
    ctx.dirty(q('#events'));
  }
  function show() {
    q('#lock-icon').textContent = locked ? 'lock' : 'lock_open';
    q('#lock-state').textContent = locked ? 'locked' : `unlocked at ${hm()}`;
    q('#lock-hint').textContent = locked ? 'slide to unlock' : 'slide to lock';
    q('#security-sub').textContent = `${locked ? 'all doors locked' : 'front door unlocked'} · alarm ${modes.find((m) => m.classList.contains('is-on'))?.dataset.alarm ?? 'off'}`;
    for (const id of ['#lock-icon', '#lock-state', '#lock-hint', '#security-sub']) ctx.dirty(q(id));
  }

  // The knob tracks the finger; past three quarters it commits, otherwise it springs home with the finger's speed.
  ctx.addDragTarget(track, {
    begin() {
      from = slide.value;
      travel = Math.max(1, (knob.base?.w ?? 0) ? track.getBoundingClientRect().width - knob.base.w / (ctx.appOpen ? 1 : 1) - 16 * ctx.unit : 1);
      tracker.reset();
    },
    move(dx, t) {
      const raw = from + dx / travel;
      const value = raw < 0 ? -rubberBand(-raw, 1) * 0.2 : raw > 1 ? 1 + rubberBand(raw - 1, 1) * 0.2 : raw;
      tracker.add(t, value);
      slide.set(value, tracker.velocity(t));
    },
    end(t) {
      const v = tracker.velocity(t);
      if (slide.value > 0.75 || (slide.value > 0.4 && v > 2.5)) {
        locked = !locked;
        slide.to(1, { spring: SPRINGS.release, velocity: v });
        setTimeout(() => slide.to(0, { spring: SPRINGS.smooth }), 380);
        log(`front door ${locked ? 'locked' : 'unlocked'} · panel`);
        ctx.toast(`front door ${locked ? 'locked' : 'unlocked'}`, locked ? 'lock' : 'lock_open');
        show();
      } else {
        slide.to(0, { spring: SPRINGS.release, velocity: v });
      }
    },
  }, 'x');

  for (const m of modes) {
    pressSurface(ctx, m, 0.8);
    m.addEventListener('click', () => {
      for (const o of modes) {
        const isOn = o === m;
        if (o.classList.contains('is-on') === isOn) continue;
        o.classList.toggle('is-on', isOn);
        o.setAttribute('aria-pressed', String(isOn));
        ctx.recolor(o);
        ctx.reshape(o);
        ctx.dirty(o);
      }
      log(`alarm set to ${m.dataset.alarm}`);
      show();
    });
  }

  ctx.security = {
    get locked() { return locked; },
    toggle() {
      locked = !locked;
      log(`front door ${locked ? 'locked' : 'unlocked'} · control center`);
      ctx.toast(`front door ${locked ? 'locked' : 'unlocked'}`, locked ? 'lock' : 'lock_open');
      show();
    },
  };

  ctx.onFrame(() => {
    if (knob) knob.dx = clamp(slide.value, -0.1, 1.1) * travel * (settings.calm ? 1 : 1);
    return {};
  });
  return { open() { show(); } };
}

// ---------- shopping ----------
export function createNotes(ctx) {
  const { screen, settings } = ctx;
  const q = (s) => screen.querySelector(s);
  const list = q('.notes-list');
  const sub = q('#notes-sub');
  const well = q('#note-well');
  const clearEl = q('#notes-clear');
  const tracker = velocityTracker();
  const records = new Map();
  let active = null; // the item under the finger
  let wellFor = null;
  let armed = false;
  let repaint = false;

  const items = () => [...list.querySelectorAll('.note-item')];
  const record = (el) => {
    if (!records.has(el)) {
      records.set(el, {
        el, surface: ctx.surface(el), live: false, ghost: false, width: 1, scale: 1, from: 0,
        sx: new Motion(0, { spring: SPRINGS.release, epsilon: 0.3 }), // the swipe, in px
        fx: new Motion(0, { spring: SPRINGS.smooth, epsilon: 0.3 }), // where a collapse moved it from
        fy: new Motion(0, { spring: SPRINGS.smooth, epsilon: 0.3 }),
        fade: new Motion(1, { spring: SPRINGS.smooth, epsilon: 0.003 }),
      });
    }
    return records.get(el);
  };
  const onList = () => items().filter((el) => !el.hidden && !record(el).ghost);
  const ticked = (el) => el.classList.contains('is-done');

  // Returns whether "clear ticked" appeared or went, so the caller repaints the window.
  function describe() {
    const shown = onList();
    const left = shown.filter((el) => !ticked(el)).length;
    sub.textContent = !shown.length ? 'the list is empty' : left ? `${left} to get` : 'all in the basket';
    ctx.dirty(sub);
    const any = shown.some(ticked);
    if (!clearEl || clearEl.hidden !== any) return false;
    clearEl.hidden = !any;
    return true;
  }

  // While an item moves, the DOM draws it: CSS paints its fill and its words are live text, above everything
  // the renderer draws, so it can pass over its neighbours. At rest it becomes a Material surface again.
  function goLive(r) {
    if (r.live) return;
    r.live = true;
    r.el.dataset.live = '';
    if (r.surface) r.surface.alpha = () => 0;
    ctx.dirty(r.el);
  }
  function goStill(r) {
    r.live = false;
    delete r.el.dataset.live;
    r.el.style.transform = '';
    r.el.style.opacity = '';
    r.el.style.clipPath = '';
    if (r.surface) r.surface.alpha = null;
    ctx.measureWithin(r.el);
    ctx.dirty(r.el);
    if (wellFor === r) hideWell();
  }

  // The well under a swiped item: a tonal cell with a delete glyph that turns signal once letting go would delete.
  function showWell(r) {
    if (!well) return;
    wellFor = r;
    Object.assign(well.style, { left: `${r.el.offsetLeft}px`, top: `${r.el.offsetTop}px`, width: `${r.el.offsetWidth}px`, height: `${r.el.offsetHeight}px` });
    well.hidden = false;
    ctx.measureWithin(well);
    ctx.dirty(well);
  }
  function arm(on) {
    if (!well || armed === on) return;
    armed = on;
    well.classList.toggle('is-armed', on);
    ctx.recolor(well);
    ctx.dirty(well);
  }
  function hideWell() {
    if (!well || !wellFor) return;
    wellFor = null;
    arm(false);
    well.hidden = true;
    ctx.measureWithin(well);
    ctx.dirty(well);
  }

  // What leaves becomes a ghost, taken out of the grid's flow, and everything after it slides into place from
  // where it was (FLIP). The calm version doesn't slide: moved items dip and return where they belong.
  function remove(gone, velocity) {
    const leaving = new Set(gone.map((r) => r.el));
    const first = new Map(onList().filter((el) => !leaving.has(el)).map((el) => [el, el.getBoundingClientRect()]));
    for (const r of gone) {
      goLive(r);
      r.ghost = true;
      const { offsetLeft: x, offsetTop: y, offsetWidth: w, offsetHeight: h } = r.el;
      r.width = w;
      Object.assign(r.el.style, { position: 'absolute', left: `${x}px`, top: `${y}px`, width: `${w}px`, height: `${h}px`, margin: '0', pointerEvents: 'none' });
      list.append(r.el); // new items fill the list from its end, so the emptied slot waits there
    }
    if (wellFor && leaving.has(wellFor.el)) hideWell();
    for (const el of first.keys()) el.style.transform = '';
    const moved = [...first].map(([el, before]) => {
      const after = el.getBoundingClientRect();
      const r = record(el);
      const scale = after.width / Math.max(el.offsetWidth, 1) || 1;
      return { r, dx: (before.left - after.left) / scale - r.sx.value, dy: (before.top - after.top) / scale };
    });
    ctx.measureWithin(list); // surfaces where they now rest, before any offset goes back on
    for (const { r, dx, dy } of moved) {
      const travels = Math.abs(dx) >= 0.5 || Math.abs(dy) >= 0.5;
      if (!travels && !r.live) continue;
      goLive(r);
      if (settings.calm) {
        r.fx.set(0);
        r.fy.set(0);
        if (travels) { r.fade.set(0.3); r.fade.to(1, { spring: SPRINGS.smooth }); }
      } else {
        const vx = r.fx.velocity;
        const vy = r.fy.velocity;
        r.fx.set(dx, vx);
        r.fx.to(0, { spring: SPRINGS.smooth, velocity: vx });
        r.fy.set(dy, vy);
        r.fy.to(0, { spring: SPRINGS.smooth, velocity: vy });
      }
    }
    for (const r of gone) {
      if (velocity === undefined) r.fade.to(0, { spring: SPRINGS.smooth });
      else r.sx.to(-r.width * 1.3, { spring: SPRINGS.release, velocity });
    }
    if (describe()) repaint = true;
  }
  function finish(r) {
    r.ghost = false;
    r.live = false;
    delete r.el.dataset.live;
    for (const key of ['position', 'left', 'top', 'width', 'height', 'margin', 'pointerEvents', 'transform', 'opacity', 'clipPath']) r.el.style[key] = '';
    r.el.hidden = true;
    r.el.classList.remove('is-done');
    r.el.setAttribute('aria-pressed', 'false');
    r.el.querySelector('.note-text').textContent = '';
    r.sx.set(0);
    r.fx.set(0);
    r.fy.set(0);
    r.fade.set(1);
    if (r.surface) r.surface.alpha = null;
    ctx.recolor(r.el);
    ctx.measureWithin(r.el);
    repaint = true;
  }

  // Swipe left to delete: 1:1 with rubber banding, then a projection decides between deleting and springing back.
  const swipe = (el) => ({
    begin() {
      const r = record(el);
      if (el.hidden || r.ghost) return;
      active = r;
      r.width = el.offsetWidth;
      r.scale = el.getBoundingClientRect().width / Math.max(el.offsetWidth, 1) || 1; // screen px per layout px
      r.from = r.sx.value;
      tracker.reset();
      goLive(r);
      showWell(r);
    },
    move(dx, t) {
      const r = active;
      if (!r || r.el !== el) return;
      const w = r.width;
      const raw = r.from + dx / r.scale;
      // left follows the finger across the item's width; right, and beyond that width, resist
      const x = raw > 0 ? rubberBand(raw, w) : raw < -w ? -w - rubberBand(-w - raw, w) : raw;
      tracker.add(t, x);
      r.sx.set(x, tracker.velocity(t));
      arm(x < -w / 2);
    },
    end(t) {
      const r = active;
      active = null;
      if (!r || r.el !== el) return;
      const v = tracker.velocity(t);
      if (r.sx.value + project(v, 0.994) < -r.width / 2) remove([r], v);
      else { arm(false); r.sx.to(0, { spring: SPRINGS.release, velocity: v }); }
    },
  });

  for (const el of items()) {
    pressSurface(ctx, el, 0.8);
    el.addEventListener('click', () => {
      if (record(el).ghost) return;
      const done = !ticked(el);
      el.classList.toggle('is-done', done);
      el.setAttribute('aria-pressed', String(done));
      ctx.recolor(el);
      ctx.dirty(el);
      if (describe()) ctx.appRepaint?.();
    });
    ctx.addDragTarget(el, swipe(el), 'x');
  }
  for (const chip of screen.querySelectorAll('[data-item]')) {
    pressSurface(ctx, chip, 0.7);
    chip.addEventListener('click', () => {
      const text = chip.dataset.item;
      if (onList().some((el) => el.querySelector('.note-text').textContent === text)) { ctx.toast(`${text} is already on the list`, 'checklist'); return; }
      const slot = items().find((el) => el.hidden);
      if (!slot) { ctx.toast('the list is full', 'checklist'); return; }
      slot.querySelector('.note-text').textContent = text;
      slot.classList.remove('is-done');
      slot.setAttribute('aria-pressed', 'false');
      ctx.recolor(slot);
      slot.hidden = false;
      describe();
      ctx.appRepaint?.();
    });
  }
  if (clearEl) {
    pressSurface(ctx, clearEl, 0.7);
    clearEl.addEventListener('click', () => {
      const done = onList().filter(ticked).map(record);
      if (done.length) remove(done);
    });
  }

  ctx.onFrame(() => {
    for (const r of records.values()) {
      if (!r.live) continue;
      const x = r.sx.value + r.fx.value;
      const y = r.fy.value;
      // a swiped item stays inside its own cell, as a row does, so it never slides across its neighbour
      r.radius ??= parseFloat(getComputedStyle(r.el).borderTopLeftRadius) || 0;
      const sx = r.sx.value;
      r.el.style.clipPath = Math.abs(sx) < 0.5 ? '' : `inset(0px ${Math.max(0, sx).toFixed(1)}px 0px ${Math.max(0, -sx).toFixed(1)}px round ${r.radius.toFixed(1)}px)`;
      if (r.ghost) {
        const left = Math.min(r.fade.value, clamp(1 + r.sx.value / r.width, 0, 1));
        if (left < 0.01) { finish(r); continue; }
        r.el.style.transform = `translate3d(${x.toFixed(2)}px, ${y.toFixed(2)}px, 0)${settings.calm ? '' : ` scale(${(0.92 + 0.08 * left).toFixed(4)})`}`;
        r.el.style.opacity = left.toFixed(3);
        continue;
      }
      r.el.style.transform = x || y ? `translate3d(${x.toFixed(2)}px, ${y.toFixed(2)}px, 0)` : '';
      r.el.style.opacity = r.fade.value < 0.999 ? r.fade.value.toFixed(3) : '';
      if (active !== r && !r.sx.moving && !r.fx.moving && !r.fy.moving && !r.fade.moving && Math.abs(x) < 0.5 && Math.abs(y) < 0.5) goStill(r);
    }
    if (repaint) { repaint = false; ctx.appRepaint?.(); }
    return {};
  });

  return { open() { if (describe()) ctx.appRepaint?.(); } };
}

// ---------- settings: the same choices as the page header, on the panel itself, and a few of its own ----------
export function createSettings(ctx, { apply, current }) {
  const { screen, settings } = ctx;
  const groups = [...screen.querySelectorAll('[data-setting]')];
  for (const g of groups) g.dataset.live = ''; // options are live DOM pills, so their selection can be a fill
  const faces = () => {
    const names = [...screen.querySelectorAll('#scapes .scape')].map((s) => s.dataset.scape).filter(Boolean);
    return names.length ? names : ['orbit'];
  };
  // Settings this app keeps itself; the others are the page header's as well, so they go through apply().
  const own = {
    quiet: { get: () => (settings.quiet ? 'on' : 'off'), set: (v) => { settings.quiet = v === 'on'; } },
    autoStandby: { get: () => (settings.autoStandby ? 'on' : 'off'), set: (v) => { settings.autoStandby = v === 'on'; } },
    standbyFace: { get: () => (faces().includes(settings.standbyFace) ? settings.standbyFace : faces()[0]), set: (v) => { settings.standbyFace = v; } },
  };
  settings.autoStandby ??= false;
  settings.standbyFace ??= faces()[0];
  const read = (key) => String(own[key] ? own[key].get() : current(key));
  const mine = () => `${settings.quiet}·${settings.autoStandby}·${settings.standbyFace}`;
  let open = false;
  let seen = '';

  const sync = () => {
    for (const g of groups) {
      const value = read(g.dataset.setting);
      for (const b of g.querySelectorAll('.opt[data-value]')) b.setAttribute('aria-pressed', String(b.dataset.value === value));
      const label = g.querySelector('[data-value-label]');
      if (label && label.textContent !== value) { label.textContent = value; ctx.dirty(label); }
    }
    seen = mine();
  };
  const write = (key, value) => {
    if (!own[key]) { apply(key, value); sync(); return; }
    own[key].set(value);
    sync();
    ctx.settingChanged?.(key, own[key].get());
  };
  // A picker steps on pointer-down, as a stepper does; pills commit on the tap.
  const step = (g, by) => {
    const key = g.dataset.setting;
    const options = key === 'standbyFace' ? faces() : [...g.querySelectorAll('.opt[data-value]')].map((b) => b.dataset.value);
    const i = Math.max(0, options.indexOf(read(key)));
    write(key, options[(i + by + options.length) % options.length]);
  };
  for (const g of groups) {
    g.addEventListener('pointerdown', (e) => {
      const b = e.target.closest('.opt[data-step]');
      if (b && e.button <= 0) step(g, Number(b.dataset.step));
    });
    g.addEventListener('click', (e) => {
      const b = e.target.closest('.opt');
      if (!b) return;
      if (b.dataset.step !== undefined) { if (e.detail === 0) step(g, Number(b.dataset.step)); return; } // keyboard
      write(g.dataset.setting, b.dataset.value);
    });
  }
  for (const row of screen.querySelectorAll('.setting')) ctx.surface(row);
  // Quiet also changes from control center, so an open settings app follows it.
  ctx.onFrame(() => {
    if (open && mine() !== seen) sync();
    return {};
  });
  return { open() { open = true; sync(); }, close() { open = false; }, sync };
}
