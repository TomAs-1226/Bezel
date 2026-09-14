// The home pages past the overview. Rooms: tap to switch, drag up or down to dim, all off with a way back.
// Climate: a dial that turns 1:1 under the finger and clicks into half-degree detents, with four modes.
// Today: a reminder you tick off. The overview's meters mirror the rooms, so every page shows the same house.

import { pressSurface } from './controls.js';
import { Motion, SPRINGS, rubberBand, snapTarget, velocityTracker } from './motion.js';
import { mixRgb } from './panel.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const TAU = Math.PI * 2;
const wrap = (a) => ((((a + Math.PI) % TAU) + TAU) % TAU) - Math.PI;
// As in the scene shader: angles run clockwise from twelve o'clock, a 270° sweep with its gap at the bottom.
const START = -2.3561945;
const SWEEP = 4.712389;
const MIN = 16;
const MAX = 28;
const DETENTS = Array.from({ length: (MAX - MIN) * 2 + 1 }, (_, k) => MIN + k / 2);
const NOW = 22.8;
const MODES = {
  heat: { caption: 'heating to', text: 'heating' },
  cool: { caption: 'cooling to', text: 'cooling' },
  auto: { caption: 'keeping', text: 'auto' },
  off: { caption: 'climate off', text: 'off' },
};
const band = (v, amount) => (v < 0 ? -rubberBand(-v, 1) * amount : v > 1 ? 1 + rubberBand(v - 1, 1) * amount : v);
const none = { x: 0, y: 0, w: 0, h: 0, r: 0 };
const hm = (d) => `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;

export function wirePages(ctx) {
  const { screen, settings, home } = ctx;
  const q = (selector) => screen.querySelector(selector);

  // ---------- rooms: light rises from the bottom of the tile, like water in a glass ----------
  const rooms = [...screen.querySelectorAll('.room')].map((el) => {
    const id = el.id.replace('room-', '');
    const level = home.levels[id];
    return { el, id, level, fill: el.querySelector('.room-fill'), state: el.querySelector('.room-state'), last: level.value || 0.6, pct: -1 };
  });
  const sub = q('#rooms-sub');
  const allOff = q('#all-off');
  let restore = []; // what "all off" switched off, until a room is switched by hand
  let shownSub = '';
  let shownButton = '';

  for (const r of rooms) {
    const fill = ctx.surface(r.fill);
    // The fill runs past the tile's bottom so the tile's clip, which follows its press morph, draws the corners.
    fill.geometry = () => {
      const c = fill.clip;
      const h = band(r.level.value, 0.1) * c.h;
      return h <= 0 ? none : { x: c.x, y: c.y + c.h - h, w: c.w, h: h + 40 * ctx.unit, r: 0 };
    };
    pressSurface(ctx, r.el, 0.86, [r.fill]);
    r.el.addEventListener('click', () => {
      const lit = r.level.target > 0.001;
      if (lit) r.last = r.level.target;
      r.level.to(lit ? 0 : r.last, { spring: SPRINGS.settle });
      restore = [];
    });
    // Dragging tracks 1:1: the whole tile's height is the whole range.
    const track = velocityTracker();
    let from = 0;
    ctx.addDragTarget(r.el, {
      begin() { from = r.level.value; track.reset(); restore = []; },
      move(dy, t) {
        const v = from - dy / fill.clip.h;
        track.add(t, v);
        r.level.set(v, track.velocity(t));
      },
      end() {
        const v = r.level.value;
        const to = v < 0.03 ? 0 : clamp(v, 0, 1);
        if (to > 0) r.last = to;
        if (to !== v) r.level.to(to, { spring: SPRINGS.release, velocity: 0 });
      },
    });
  }

  pressSurface(ctx, allOff, 0.7);
  allOff.addEventListener('click', () => {
    const lit = rooms.filter((r) => r.level.target > 0.001);
    const count = (n) => `${n} ${n === 1 ? 'room' : 'rooms'}`;
    if (lit.length) {
      restore = lit.map((r) => [r, r.level.target]);
      lit.forEach((r, k) => { r.last = r.level.target; setTimeout(() => r.level.to(0, { spring: SPRINGS.settle }), k * 45); });
      ctx.toast(`all off · ${count(lit.length)}`, 'power_settings_new');
    } else if (restore.length) {
      restore.forEach(([r, v], k) => setTimeout(() => r.level.to(v, { spring: SPRINGS.settle }), k * 45));
      ctx.toast(`restored · ${count(restore.length)}`, 'lightbulb');
      restore = [];
    }
  });

  function syncRooms() {
    let lit = 0;
    for (const r of rooms) {
      if (r.level.target > 0.001) lit += 1;
      const pct = Math.round(clamp(r.level.value, 0, 1) * 100);
      if (pct === r.pct) continue;
      const switched = (pct > 0) !== (r.pct > 0);
      r.pct = pct;
      r.state.textContent = pct ? `${pct}%` : 'off';
      r.el.setAttribute('aria-pressed', String(pct > 0));
      ctx.dirty(switched ? r.el.querySelector('.room-top') : r.state); // the icon's colour follows the switch
    }
    const text = lit === 0 ? 'all off' : lit === rooms.length ? 'all lit' : `${lit} of ${rooms.length} lit`;
    if (text !== shownSub) { shownSub = text; sub.textContent = text; ctx.dirty(sub); }
    const button = lit ? 'off' : restore.length ? 'restore' : 'idle';
    if (button !== shownButton) {
      shownButton = button;
      allOff.querySelector('.icon').textContent = button === 'restore' ? 'lightbulb' : 'power_settings_new';
      allOff.querySelector('.mono').textContent = button === 'restore' ? 'restore' : 'all off';
      allOff.disabled = button === 'idle';
      ctx.dirty(allOff);
    }
  }

  // ---------- the overview's meters show the same rooms ----------
  const mirrors = [...screen.querySelectorAll('.big[data-room]')].map((el) => {
    const fill = screen.querySelector(`.meter-fill[data-room="${el.dataset.room}"]`);
    const level = home.levels[el.dataset.room];
    const s = ctx.surface(fill);
    s.geometry = () => {
      const c = s.clip;
      const w = clamp(level.value, 0, 1) * c.w;
      return w < 0.5 ? none : { x: c.x - 20 * ctx.unit, y: c.y, w: w + 20 * ctx.unit, h: c.h, r: 6 * ctx.unit };
    };
    return { el, fill, level, pct: -1 };
  });
  function syncMirrors() {
    for (const m of mirrors) {
      const pct = Math.round(clamp(m.level.value, 0, 1) * 100);
      if (pct === m.pct) continue;
      m.pct = pct;
      m.el.textContent = pct ? `${pct}%` : 'off';
      m.fill.style.width = `${pct}%`;
      ctx.dirty(m.el);
    }
  }

  // ---------- climate ----------
  const { setpoint } = home;
  const dial = q('#dial');
  const knob = q('#knob');
  const knobGlass = ctx.glass(knob);
  const valueEl = q('#dial-value');
  const captionEl = q('#dial-caption');
  const modeEl = q('#mode-text');
  const modeButtons = [...screen.querySelectorAll('.mode')];
  const colour = { from: [1, 0.36, 0.12], to: [1, 0.36, 0.12], fade: new Motion(1, { spring: SPRINGS.effect, epsilon: 0.002 }) };
  let mode = 'heat';
  let centre = { x: 0, y: 0 };
  let R = 1;
  let shownValue = '';

  const fractionOf = (v) => (v - MIN) / (MAX - MIN);
  // a press in the gap at the bottom goes to the nearer end
  const fractionAt = (a) => {
    let rel = (((a - START) % TAU) + TAU) % TAU;
    if (rel > SWEEP) rel = rel - SWEEP < (TAU - SWEEP) / 2 ? SWEEP : 0;
    return rel / SWEEP;
  };
  const knobCentre = () => {
    const a = START + band(fractionOf(setpoint.value), 0.08) * SWEEP;
    return [centre.x + Math.sin(a) * R, centre.y - Math.cos(a) * R];
  };
  knobGlass.geometry = () => {
    const [x, y] = knobCentre();
    const s = 68 * ctx.unit;
    return { x: x - s / 2, y: y - s / 2, w: s, h: s, r: s / 2 };
  };

  // Heat is the signal colour and cooling is ice; auto shows whichever it is doing; off is a quiet grey.
  function colourTarget() {
    const c = settings.rgb;
    const cool = settings.tone === 'light' ? [0.24, 0.5, 0.78] : c.ice;
    if (mode === 'off') return mixRgb(c.ground, c.ink, 0.3);
    return mode === 'cool' || (mode === 'auto' && setpoint.target < NOW) ? cool : c.signal;
  }
  function recolourDial(animate) {
    const to = colourTarget();
    if (!animate) { colour.from = to; colour.to = to; colour.fade.set(1); return; }
    if (to.every((v, k) => Math.abs(v - colour.to[k]) < 1e-3)) return;
    colour.from = mixRgb(colour.from, colour.to, colour.fade.value);
    colour.to = to;
    colour.fade.set(0);
    colour.fade.to(1);
  }

  const dialTrack = velocityTracker();
  let turn = null;
  let buzz = 'vibrate' in navigator;
  dial.addEventListener('pointerdown', (e) => {
    if (e.button > 0) return;
    const [x, y] = ctx.local(e);
    const dx = x - (centre.x + ctx.page), dy = y - centre.y;
    // Only the ring turns; a drag through the middle pages like anywhere else.
    if (Math.abs(Math.hypot(dx, dy) - R) > 58 * ctx.unit) return;
    e.stopPropagation();
    try { dial.setPointerCapture(e.pointerId); } catch { /* synthetic events */ }
    const [kx, ky] = knobCentre();
    const onKnob = Math.hypot(x - (kx + ctx.page), y - ky) < 48 * ctx.unit;
    const a = Math.atan2(dx, -dy);
    turn = { id: e.pointerId, a, f: onKnob ? fractionOf(setpoint.value) : fractionAt(a), chase: !onKnob, detent: Math.round(setpoint.value * 2) };
    // Pressing the ring sends the knob there; grabbing the knob keeps it where it was grabbed.
    if (!onKnob) setpoint.to(MIN + turn.f * (MAX - MIN), { spring: SPRINGS.settle });
    [knobGlass.px, knobGlass.py] = [x, y];
    knobGlass.press.to(1, { spring: SPRINGS.hold });
    dialTrack.reset();
  });
  dial.addEventListener('pointermove', (e) => {
    if (!turn || e.pointerId !== turn.id) return;
    const [x, y] = ctx.local(e);
    const dx = x - (centre.x + ctx.page), dy = y - centre.y;
    if (Math.hypot(dx, dy) < 40 * ctx.unit) return; // through the centre the angle means nothing
    const a = Math.atan2(dx, -dy);
    turn.f += wrap(a - turn.a) / SWEEP;
    turn.a = a;
    const t = e.timeStamp / 1000;
    const v = MIN + turn.f * (MAX - MIN);
    dialTrack.add(t, v);
    // After a press on the ring the knob is still travelling; it catches the finger on the held spring first.
    if (turn.chase && Math.abs(setpoint.value - v) > 0.05) setpoint.to(v, { spring: SPRINGS.hold });
    else { turn.chase = false; setpoint.set(v, dialTrack.velocity(t)); }
    const detent = Math.round(clamp(v, MIN, MAX) * 2);
    if (detent !== turn.detent) {
      turn.detent = detent;
      // a tick under the finger, where the browser allows it; a frame that refuses once is not asked again
      if (buzz && e.pointerType === 'touch' && navigator.userActivation?.hasBeenActive) buzz = navigator.vibrate?.(3) === true;
    }
  });
  const endTurn = (e) => {
    if (!turn || e.pointerId !== turn.id) return;
    turn = null;
    knobGlass.press.to(0, { spring: SPRINGS.release });
    const inside = setpoint.value >= MIN && setpoint.value <= MAX;
    const velocity = inside ? dialTrack.velocity(e.timeStamp / 1000) : 0;
    // Snap to target: project the release a little and land on a detent with the rotation spring.
    const to = snapTarget(clamp(setpoint.value, MIN, MAX), velocity, DETENTS);
    setpoint.to(to, { spring: SPRINGS.detent, velocity: setpoint.moving ? undefined : velocity });
  };
  dial.addEventListener('pointerup', endTurn);
  dial.addEventListener('pointercancel', endTurn);
  dial.addEventListener('keydown', (e) => {
    const step = { ArrowRight: 0.5, ArrowUp: 0.5, ArrowLeft: -0.5, ArrowDown: -0.5, PageUp: 2, PageDown: -2 }[e.key];
    const next = step !== undefined ? clamp(Math.round((setpoint.target + step) * 2) / 2, MIN, MAX) : e.key === 'Home' ? MIN : e.key === 'End' ? MAX : null;
    if (next === null) return;
    e.preventDefault();
    setpoint.set(next);
  });

  // The selected mode morphs into a capsule, as Material 3 Expressive's connected buttons do.
  function setMode(name) {
    mode = name;
    for (const b of modeButtons) {
      const isOn = b.dataset.mode === name;
      if (b.classList.contains('is-on') === isOn) continue;
      b.classList.toggle('is-on', isOn);
      b.setAttribute('aria-pressed', String(isOn));
      ctx.recolor(b);
      ctx.reshape(b);
      ctx.dirty(b);
    }
    dial.classList.toggle('is-off', name === 'off');
    captionEl.textContent = MODES[name].caption;
    modeEl.textContent = MODES[name].text;
    for (const el of [captionEl, modeEl, valueEl]) ctx.dirty(el);
    recolourDial(true);
  }
  for (const b of modeButtons) {
    pressSurface(ctx, b, 0.8);
    b.addEventListener('click', () => setMode(b.dataset.mode));
  }

  // ---------- today ----------
  const reminder = q('#reminder');
  pressSurface(ctx, reminder, 0.85);
  reminder.addEventListener('click', () => {
    const done = !reminder.classList.contains('is-done');
    reminder.classList.toggle('is-done', done);
    reminder.setAttribute('aria-pressed', String(done));
    q('#reminder-icon').textContent = done ? 'check' : 'delete';
    q('#reminder-when').textContent = done ? `done · ${hm(new Date())}` : 'recycling · by 19:00';
    ctx.recolor(reminder);
    ctx.dirty(reminder);
  });

  ctx.onMeasure({
    after(rel) {
      const d = rel(dial);
      centre = { x: d.x + d.w / 2, y: d.y + d.h / 2 };
      R = 190 * ctx.unit; // the knob clears the page title at the ring's top and the modes at its open bottom
      shownValue = '';
      shownSub = '';
      shownButton = '';
      for (const r of rooms) r.pct = -1;
      for (const m of mirrors) m.pct = -1;
      recolourDial(false);
    },
  });
  ctx.onRetone(() => recolourDial(false));

  ctx.onFrame(() => {
    syncRooms();
    syncMirrors();
    const shown = Math.round(clamp(setpoint.value, MIN, MAX) * 2) / 2;
    const text = `${shown.toFixed(1)}°`;
    if (text !== shownValue) {
      shownValue = text;
      valueEl.textContent = text;
      dial.setAttribute('aria-valuenow', String(shown));
      ctx.dirty(valueEl);
    }
    if (mode === 'auto') recolourDial(true);
    const [kx, ky] = knobCentre();
    const s = 34 * ctx.unit;
    if (knobGlass.base) knobGlass.shift = `translate(${(kx - s - knobGlass.base.x).toFixed(2)}px, ${(ky - s - knobGlass.base.y).toFixed(2)}px) `;
    return {
      dial: [centre.x - 2 * ctx.W, centre.y, R, clamp(band(fractionOf(setpoint.value), 0.08), 0, 1)],
      dialColor: mixRgb(colour.from, colour.to, colour.fade.value),
    };
  });
}
