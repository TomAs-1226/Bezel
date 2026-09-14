// Live activities: whatever is running right now (timers, a cycle, the music, an unlocked door) on a small stack
// of glass cards floating above everything. Drag the stack anywhere; let go and it glides to the corner your throw
// points at, on the WWDC18 picture-in-picture pattern: project the velocity, pick the corner, hand the velocity to
// the spring. Throw it past a side edge and it stashes: the cards melt into one glass orb at that edge. The orb
// keeps the count and the soonest progress, warms to amber and beats once a second while something is about to
// finish, and sends out a ring of light when something needs you. Tap it, or drag it out, and the cards grow back
// out of it. Open apps and standby get the orb, so nothing floats over what they show.

import { Motion, SPRINGS, project, velocityTracker } from './motion.js';
import { PLAYING, STANDBY } from './panel.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const mix = (a, b, t) => a + (b - a) * t;
const smooth = (a, b, v) => { const t = clamp((v - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
const clock = (s) => `${Math.floor(Math.max(0, s) / 60)}:${String(Math.floor(Math.max(0, s) % 60)).padStart(2, '0')}`;
const CARD_W = 224;
const CARD_H = 64;
const GAP = 26; // wider than the glass merge distance, so neighbouring cards stay separate
const ORB = 64; // as tall as a card: a card only has to lose its width to become the orb
const EDGE = 18; // the stashed orb's distance from the side
const TOP = 96;
const BOTTOM = 552;
const SOON = 10; // seconds: inside this a countdown beats once a second and warms the glass
const RING_EVERY = 2.4; // seconds between rings while something that finished has not been seen
const PING_LIFE = 1.1;
const HUE = { signal: 0, amber: 1, leaf: 2, ice: 3 };

export function createActivities(ctx, { openApp }) {
  const { screen, settings } = ctx;
  const stack = screen.querySelector('#activities');
  const orb = {
    el: stack.querySelector('#activity-orb'),
    touch: new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 }),
    beat: new Motion(0, { spring: SPRINGS.tick, epsilon: 0.002 }),
    text: '',
  };
  orb.icon = orb.el.querySelector('.icon');
  orb.fill = orb.el.querySelector('.orb-fill');
  orb.pips = [...orb.el.querySelectorAll('.orb-pips i')];
  const cards = [...stack.querySelectorAll('.activity')].map((el) => ({
    el, g: ctx.glass(el),
    icon: el.querySelector('.icon'), label: el.querySelector('.activity-label'), value: el.querySelector('.activity-value'), bar: el.querySelector('.activity-bar i'),
    show: new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 }),
    slot: new Motion(0, { spring: SPRINGS.release, epsilon: 0.05 }),
    beat: new Motion(0, { spring: SPRINGS.tick, epsilon: 0.002 }),
    item: null, text: '', rect: null, pressed: false,
  }));
  for (const c of cards) c.g.geometry = () => c.rect ?? { x: 0, y: 0, w: 0, h: 0, r: 0 };

  const x = new Motion(720 - 30 - CARD_W, { spring: SPRINGS.settle, epsilon: 0.05 });
  const y = new Motion(TOP, { spring: SPRINGS.settle, epsilon: 0.05 });
  const ox = new Motion(720 - EDGE - ORB, { spring: SPRINGS.settle, epsilon: 0.05 });
  const oy = new Motion(TOP, { spring: SPRINGS.settle, epsilon: 0.05 });
  const fold = new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 });
  const track = { x: velocityTracker(), y: velocityTracker() };
  let corner = 'top-right';
  let side = 'right';
  let anchorTop = true;
  let count = 0;
  let drag = null;
  let stashed = false; // put away on purpose
  let peeking = false; // opened on purpose where it would otherwise stay folded
  let booted = false;
  let now = 0;
  let ping = null;
  let lastRing = 0;
  let lastBeat = -1;
  const seen = new Map();
  const unseen = new Set(); // finished things nobody has looked at yet

  const heightFor = (n) => Math.max(1, n) * CARD_H + Math.max(0, n - 1) * GAP;
  const corners = (n) => ({
    'top-left': [30, TOP], 'top-right': [720 - 30 - CARD_W, TOP],
    'bottom-left': [30, BOTTOM - heightFor(n)], 'bottom-right': [720 - 30 - CARD_W, BOTTOM - heightFor(n)],
  });
  const nearestCorner = (px, py) => `${py < 324 ? 'top' : 'bottom'}-${px < 360 ? 'left' : 'right'}`;
  /** Where the orb sits inside a stack at (sx, sy): the end of the card nearest the orb's side. */
  const homeOf = (sx, sy) => [side === 'left' ? sx : sx + CARD_W - ORB, anchorTop ? sy : sy + (Math.max(1, count) - 1) * (CARD_H + GAP)];
  // In an app or on standby the orb tucks half past the side, clear of whatever the app puts near its edges.
  const tucked = () => autoFold() && !peeking;
  const orbX = () => (side === 'left' ? (tucked() ? -ORB / 2 : EDGE) : tucked() ? 720 - ORB / 2 : 720 - EDGE - ORB);
  const stashSpot = (top) => [orbX(), clamp(top, TOP, BOTTOM - ORB)];
  const autoFold = () => (ctx.appProgress ?? 0) > 0.5 || ctx.index === STANDBY;

  Object.defineProperty(ctx, 'activitiesRect', {
    get: () => {
      if (!count) return null;
      const u = ctx.unit;
      return fold.value > 0.5
        ? { x: ox.value * u, y: oy.value * u, w: ORB * u, h: ORB * u }
        : { x: x.value * u, y: y.value * u, w: CARD_W * u, h: heightFor(count) * u };
    },
    configurable: true,
  });

  /** Folds the stack into the orb at its side, carrying a throw's velocity there. */
  function toOrb(vx = 0, vy = 0, top = null) {
    if (!stashed) { side = corner.endsWith('left') ? 'left' : 'right'; anchorTop = corner.startsWith('top'); }
    const [hx, hy] = homeOf(x.value, y.value);
    if (fold.value < 0.02) { ox.set(hx); oy.set(hy); }
    const [sx, sy] = stashSpot(top ?? hy);
    ox.to(sx, { spring: SPRINGS.settle, velocity: vx });
    oy.to(sy, { spring: SPRINGS.settle, velocity: vy });
    fold.to(1, { spring: SPRINGS.release });
  }
  /** Grows the cards back out of the orb into a corner. Opening it on purpose counts as looking. */
  function toStack(name, vx = 0, vy = 0, looked = false) {
    stashed = false;
    corner = name;
    side = name.endsWith('left') ? 'left' : 'right';
    anchorTop = name.startsWith('top');
    const [cx, cy] = corners(count)[name];
    if (fold.value > 0.98) { x.set(cx); y.set(cy); } else { x.to(cx, { spring: SPRINGS.settle }); y.to(cy, { spring: SPRINGS.settle }); }
    const [hx, hy] = homeOf(cx, cy);
    ox.to(hx, { spring: SPRINGS.settle, velocity: vx });
    oy.to(hy, { spring: SPRINGS.settle, velocity: vy });
    fold.to(0, { spring: SPRINGS.release });
    if (looked) unseen.clear();
  }

  function gather() {
    const list = [];
    for (const t of ctx.timersList?.() ?? []) {
      list.push({
        key: `timer:${t.id ?? t.label}`, icon: 'timer', label: t.label, urgent: t.done, ringing: t.done, left: t.done || t.paused ? null : t.left,
        value: t.done ? 'done' : t.paused ? 'paused' : clock(t.left), progress: t.total ? 1 - t.left / t.total : 0, open: () => openApp('timers'),
      });
    }
    for (const a of ctx.appliancesList?.() ?? []) {
      list.push({
        key: `appliance:${a.label}`, icon: a.icon ?? 'local_laundry_service', label: a.label, urgent: a.done, ringing: a.done, left: a.done || a.paused ? null : a.left,
        value: a.done ? 'done' : a.paused ? 'paused' : a.value ?? clock(a.left), progress: a.progress ?? 0, open: () => openApp('appliances'),
      });
    }
    const np = ctx.nowPlaying?.();
    if (np?.playing && ctx.index !== PLAYING) {
      list.push({ key: 'music', icon: 'music_note', label: np.title.toLowerCase(), value: `−${clock(np.length - np.elapsed)}`, progress: np.elapsed / np.length, open: () => ctx.go(PLAYING) });
    }
    if (ctx.security && !ctx.security.locked) list.push({ key: 'door', icon: 'lock_open', label: 'door', value: 'unlocked', urgent: true, progress: 0, open: () => openApp('security') });
    // what needs you comes first
    return list.sort((a, b) => Number(!!b.urgent) - Number(!!a.urgent)).slice(0, cards.length);
  }

  /** A ring of light from the orb, or from the card that holds the key while the stack is open. */
  function call(hue, key, onlyFolded) {
    if (onlyFolded && fold.target !== 1) return;
    ping = { t0: now, hue, key };
    lastRing = now;
  }

  // ---------- drag the stack, or the orb, like a picture-in-picture window ----------
  stack.addEventListener('pointerdown', (e) => {
    if (e.button > 0 || !count) return;
    const onOrb = orb.el.contains(e.target);
    const card = onOrb ? null : cards.find((c) => c.el.contains(e.target));
    if (onOrb ? fold.target !== 1 : !card?.item || fold.target !== 0) return;
    e.stopPropagation();
    const [px, py] = ctx.local(e);
    const mx = onOrb ? ox : x;
    const my = onOrb ? oy : y;
    drag = { id: e.pointerId, px, py, x0: mx.value, y0: my.value, moved: false, card, onOrb };
    mx.set(mx.value);
    my.set(my.value);
    track.x.reset();
    track.y.reset();
    if (onOrb) orb.touch.to(1, { spring: SPRINGS.hold });
  });
  window.addEventListener('pointermove', (e) => {
    if (!drag || e.pointerId !== drag.id) return;
    const [px, py] = ctx.local(e);
    if (!drag.moved && Math.hypot(px - drag.px, py - drag.py) < 10) return;
    drag.moved = true;
    const t = e.timeStamp / 1000;
    const nx = drag.x0 + (px - drag.px) / ctx.unit;
    const ny = drag.y0 + (py - drag.py) / ctx.unit;
    track.x.add(t, nx);
    track.y.add(t, ny);
    (drag.onOrb ? ox : x).set(nx, track.x.velocity(t));
    (drag.onOrb ? oy : y).set(ny, track.y.velocity(t));
    if (drag.onOrb || settings.calm) return;
    // past a side edge the cards start to condense under the finger, hinting at the orb they would become
    const centre = nx + CARD_W / 2;
    const over = Math.max(40 - centre, centre - 680, 0);
    if (over > 0 || fold.value > 0) {
      side = centre < 360 ? 'left' : 'right';
      anchorTop = ny + heightFor(count) / 2 < 324;
      const [hx, hy] = homeOf(nx, ny);
      ox.set(hx);
      oy.set(hy);
      fold.set(clamp(over / 260, 0, 0.3));
    }
  });
  const release = (e) => {
    if (!drag || e.pointerId !== drag.id) return;
    const d = drag;
    drag = null;
    orb.touch.to(0, { spring: SPRINGS.release });
    if (!d.moved) {
      if (d.onOrb) {
        peeking = autoFold();
        toStack(nearestCorner(ox.value + ORB / 2, oy.value + ORB / 2), 0, 0, true);
      } else {
        unseen.delete(d.card.item.key);
        d.card.item.open();
      }
      return;
    }
    ctx.suppressClick();
    const t = e.timeStamp / 1000;
    const vx = track.x.velocity(t);
    const vy = track.y.velocity(t);
    if (d.onOrb) {
      const aimX = ox.value + ORB / 2 + project(vx, 0.99);
      const aimY = oy.value + ORB / 2 + project(vy, 0.99);
      if (aimX > 170 && aimX < 550) {
        peeking = autoFold();
        toStack(nearestCorner(aimX, aimY), vx, vy, true);
      } else {
        side = aimX < 360 ? 'left' : 'right';
        const [sx, sy] = stashSpot(aimY - ORB / 2);
        ox.to(sx, { spring: SPRINGS.settle, velocity: vx });
        oy.to(sy, { spring: SPRINGS.settle, velocity: vy });
      }
      return;
    }
    const aimX = x.value + project(vx, 0.99);
    const aimY = y.value + project(vy, 0.99);
    const centre = aimX + CARD_W / 2;
    if (centre < 40 || centre > 680) {
      // thrown past a side: put it away
      stashed = true;
      side = centre < 360 ? 'left' : 'right';
      anchorTop = y.value + heightFor(count) / 2 < 324;
      toOrb(vx, vy, homeOf(x.value, y.value)[1] + project(vy, 0.99));
      return;
    }
    const options = corners(count);
    const name = Object.keys(options).reduce((best, key) => {
      const [cx, cy] = options[key];
      const [bx, by] = options[best];
      return Math.hypot(cx - aimX, cy - aimY) < Math.hypot(bx - aimX, by - aimY) ? key : best;
    }, corner);
    corner = name;
    side = name.endsWith('left') ? 'left' : 'right';
    anchorTop = name.startsWith('top');
    x.to(options[name][0], { spring: SPRINGS.settle, velocity: vx });
    y.to(options[name][1], { spring: SPRINGS.settle, velocity: vy });
    if (fold.target !== 0) fold.to(0, { spring: SPRINGS.release });
  };
  window.addEventListener('pointerup', release);
  window.addEventListener('pointercancel', release);

  ctx.activities = {
    get folded() { return fold.target === 1; },
    stash(to = 'right') { if (!count) return; stashed = true; side = to; anchorTop = corner.startsWith('top'); toOrb(); },
    expand() { if (!count) return; peeking = autoFold(); toStack(corner, 0, 0, true); },
  };

  ctx.onFrame((t) => {
    now = t;
    const items = gather();
    // keep each activity on the card it already has, so nothing jumps when another arrives
    for (const c of cards) if (c.item && !items.some((i) => i.key === c.item.key)) { c.item = null; c.show.to(0, { spring: SPRINGS.smooth }); }
    for (const item of items) {
      const holder = cards.find((c) => c.item?.key === item.key) ?? cards.find((c) => !c.item && c.show.value < 0.02);
      if (!holder) continue;
      const arriving = !holder.item;
      holder.item = item;
      if (arriving) { holder.slot.set(items.indexOf(item)); holder.show.to(1, { spring: SPRINGS.release }); }
    }
    const live = cards.filter((c) => c.item).sort((a, b) => items.findIndex((i) => i.key === a.item.key) - items.findIndex((i) => i.key === b.item.key));
    live.forEach((c, i) => { if (c.slot.target !== i) c.slot.to(i, { spring: SPRINGS.release }); });
    if (live.length !== count) {
      count = live.length;
      if (!drag && corner.startsWith('bottom')) y.to(corners(count)[corner][1], { spring: SPRINGS.settle });
    }

    // ---------- attention: what arrived, what finished, what is about to ----------
    const keys = new Set(items.map((i) => i.key));
    for (const key of [...seen.keys()]) if (!keys.has(key)) { seen.delete(key); unseen.delete(key); }
    for (const item of items) {
      const before = seen.get(item.key);
      if (booted && item.ringing && !before?.ringing) unseen.add(item.key);
      if (booted && (!before || (item.urgent && !before.urgent))) call(item.urgent ? HUE.signal : HUE.ice, item.key, !item.urgent);
      seen.set(item.key, { urgent: !!item.urgent, ringing: !!item.ringing });
    }
    booted = true;
    if (unseen.size && now - lastRing > RING_EVERY) call(HUE.signal, [...unseen][0], false);
    const soon = items.filter((i) => i.left != null && i.left > 0 && i.left <= SOON).sort((a, b) => a.left - b.left)[0] ?? null;
    if (soon) {
      const second = Math.ceil(soon.left);
      if (second !== lastBeat) {
        lastBeat = second;
        const holder = fold.target === 1 ? null : cards.find((c) => c.item?.key === soon.key);
        if (!settings.calm) (holder ?? orb).beat.to(0, { spring: SPRINGS.tick, velocity: 7 });
      }
    } else {
      lastBeat = -1;
    }

    // ---------- folded or not ----------
    const auto = autoFold();
    if (!auto) peeking = false;
    if (!drag && count) {
      const wantOrb = stashed || (auto && !peeking);
      if (wantOrb && fold.target !== 1) toOrb();
      else if (!wantOrb && fold.target !== 0) toStack(corner);
      if (fold.target === 1 && Math.abs(ox.target - orbX()) > 0.5) ox.to(orbX(), { spring: SPRINGS.settle });
    }

    const u = ctx.unit;
    const away = 1 - (ctx.ccProgress ?? 0);
    const f = clamp(fold.value, 0, 1);
    const primary = anchorTop ? live[0] : live[live.length - 1];
    const nearSlot = anchorTop ? 0 : Math.max(0, count - 1);
    const spread = 0.08 * Math.max(0, count - 1);
    const ringing = items.some((i) => i.ringing && unseen.has(i.key));
    const alert = items.some((i) => i.urgent);
    const warm = soon ? 1 - soon.left / SOON : 0;
    // the glass says what kind of attention: amber while something is about to finish, the signal colour while
    // something has finished unseen or a door is open
    const orbHue = ringing || alert ? HUE.signal : soon ? HUE.amber : HUE.ice;
    const orbTint = ringing ? 0.9 : alert ? 0.6 : soon ? 0.2 + 0.45 * warm : 0;

    for (const c of cards) {
      const s = clamp(c.show.value, 0, 1.1);
      const item = c.item;
      if (item) {
        const text = `${item.icon}|${item.label}|${item.value}|${item.urgent}`;
        if (text !== c.text) {
          c.text = text;
          c.icon.textContent = item.icon;
          c.label.textContent = item.label;
          c.value.textContent = item.value;
          c.el.classList.toggle('is-urgent', !!item.urgent);
          c.el.setAttribute('aria-label', `${item.label}, ${item.value}`);
        }
        c.bar.style.width = `${Math.round(clamp(item.progress, 0, 1) * 100)}%`;
      }
      // cards nearest the orb fold first; the others follow and melt into it
      const fi = clamp((f - 0.08 * Math.abs(c.slot.value - nearSlot)) / (1 - spread), 0, 1);
      const absorb = c === primary || !item ? 0 : smooth(0.55, 1, fi);
      const w0 = mix(CARD_W, ORB, fi);
      const cx = mix(x.value, ox.value, fi) + w0 / 2;
      const cy = mix(y.value + c.slot.value * (CARD_H + GAP), oy.value, fi) + CARD_H / 2;
      const beat = c === primary && f > 0.5 ? 0 : 0.28 * c.beat.value;
      const scale = (settings.calm ? 1 : 0.86 + 0.14 * s) * (1 + beat) * (1 - absorb);
      const w = w0 * scale;
      const h = CARD_H * scale;
      c.rect = { x: (cx - w / 2) * u, y: (cy - h / 2) * u, w: w * u, h: h * u, r: (Math.min(w, h) / 2) * u };
      c.g.strength.set(clamp(s, 0, 1) * away * (absorb > 0.97 ? 0 : 1));

      const own = item?.ringing && unseen.has(item.key) ? [HUE.signal, 0.35] : item && soon?.key === item.key ? [HUE.amber, 0.12 + 0.25 * warm] : [HUE.ice, 0];
      const tint = mix(own[1], orbTint, f);
      c.g.hue = orbTint * f >= own[1] * (1 - f) ? orbHue : own[0];
      if (fold.moving) c.g.tint.set(tint);
      else if (Math.abs(c.g.tint.target - tint) > 0.005) c.g.tint.to(tint, { spring: SPRINGS.effect });
      // the orb's glass swells and glows with each beat and under a finger
      if (c === primary && f > 0.5) {
        c.g.px = cx * u;
        c.g.py = cy * u;
        c.g.press.set(Math.max(orb.touch.value, clamp(orb.beat.value * 2.4, 0, 0.8)));
        c.pressed = true;
      } else if (c.pressed) {
        c.pressed = false;
        c.g.press.to(0, { spring: SPRINGS.release });
      }

      // words leave before the glass has lost much width, blurring out as it condenses
      const words = 1 - smooth(0, 0.42, fi);
      c.el.style.opacity = (clamp(s * 1.4, 0, 1) * away * words).toFixed(3);
      const blur = settings.calm ? 0 : fi * 9 * u;
      c.el.style.filter = blur > 0.05 && words > 0.001 ? `blur(${blur.toFixed(2)}px)` : '';
      c.g.shift = `translate(${((cx - CARD_W / 2) * u).toFixed(2)}px, ${((cy - CARD_H / 2) * u).toFixed(2)}px) scale(${scale.toFixed(4)}) `;
      c.el.classList.toggle('is-live', !!item && away > 0.5 && f < 0.3);
    }

    // ---------- the orb's own face: the lead activity's icon, its progress, a pip per activity ----------
    const shown = primary ? smooth(0.5, 1, f) * clamp(primary.show.value, 0, 1) * away : 0;
    orb.el.style.opacity = shown.toFixed(3);
    orb.el.classList.toggle('is-live', shown > 0.5);
    if (primary?.rect) {
      const r = primary.rect;
      const k = (r.h * (1 + primary.g.gel * primary.g.press.value)) / (ORB * u);
      orb.el.style.transform = `translate(${(r.x + r.w / 2 - (ORB * u) / 2).toFixed(2)}px, ${(r.y + r.h / 2 - (ORB * u) / 2).toFixed(2)}px) scale(${k.toFixed(4)})`;
      orb.el.style.filter = !settings.calm && shown > 0.02 && shown < 0.98 ? `blur(${((1 - shown) * 6 * u).toFixed(2)}px)` : '';
      const lead = soon ?? items[0];
      const text = `${lead.icon}|${count}|${ringing || alert}|${!!soon}|${items.map((i) => i.label).join(',')}`;
      if (text !== orb.text) {
        orb.text = text;
        orb.icon.textContent = lead.icon;
        orb.pips.forEach((p, i) => { p.hidden = count < 2 || i >= count; });
        orb.el.classList.toggle('is-alert', ringing || alert);
        orb.el.classList.toggle('is-soon', !!soon && !(ringing || alert));
        orb.el.setAttribute('aria-label', `Live activities: ${items.map((i) => i.label).join(', ')}. Tap to open.`);
      }
      orb.fill.style.strokeDashoffset = (100 - clamp(lead.progress, 0, 1) * 100).toFixed(1);
    }

    // ---------- the ring of light ----------
    if (!ping) return {};
    const age = now - ping.t0;
    const holder = f > 0.5 ? primary : cards.find((c) => c.item?.key === ping.key) ?? primary;
    if (age > PING_LIFE || !holder?.rect) { ping = null; return {}; }
    const r = holder.rect;
    const grow = settings.calm ? 3 * u : 30 * u * (1 - Math.exp(-age * 3.4));
    const alpha = (settings.calm ? Math.sin((Math.PI * age) / PING_LIFE) * 0.6 : (1 - age / PING_LIFE) ** 1.6 * 0.85) * away;
    return { ping: { x: r.x, y: r.y, w: r.w, h: r.h, r: r.r, grow, alpha, hue: ping.hue } };
  });
}
