// Standby: twelve faces stacked vertically (orbit, dots, dial, horizon, words, world, moon, tree, flip, album, orrery, next up), paged with
// Detent's paging pattern. Backgrounds are drawn in the scene shader; each face's text is its own rasterized
// layer, painted only when that face is near. The standby actions glide to wherever the face leaves room.

import { ACTIONS2, flipLeft, skyNow, updateFaces2 } from './faces2.js';
import { Motion, SPRINGS, pageFor, rubberBand, velocityTracker } from './motion.js';
import { STANDBY } from './panel.js';
import { createRaster } from './raster.js';

const TAU = Math.PI * 2;
const SYNODIC = 29.530588853;
const pad = (n) => String(n).padStart(2, '0');

// Where each face leaves room for the actions, in panel pixels: a centred row at a height, or split to the corners.
const ACTIONS = [
  { row: 470 }, //   orbit: under the sphere, above the departures
  { row: 486 }, //   dots: in the band the blobs keep clear of
  { split: 520 }, // dial: the dial fills the middle, so the corners beside it
  { row: 486 }, //   horizon: over the far ridges, clear of the time and the sun
  { row: 516 }, //   words: under the letters
  { row: 486 }, //   world: under the cities
  { row: 540 }, //   moon: under the phase
  { split: 506 }, // tree: either side of the trunk
].concat(ACTIONS2);
const CITIES = ['local', 'Europe/London', 'America/New_York', 'Asia/Tokyo'];
const PHASES = [[0.03, 'new moon'], [0.22, 'waxing crescent'], [0.28, 'first quarter'], [0.47, 'waxing gibbous'], [0.53, 'full moon'],
  [0.72, 'waning gibbous'], [0.78, 'last quarter'], [0.97, 'waning crescent'], [1.01, 'new moon']];

// Everything that follows the clock: the time, dates, hour labels and the next departures.
function clockText(el, now, hm, day, date) {
  const { clock: kind, offset = '0', at = '0', nth = '0' } = el.dataset;
  if (kind === 'hm') return hm;
  if (kind === 'date') return `${day} ${date}`;
  if (kind === 'short') return `${day.slice(0, 3)} ${date}`;
  if (kind === 'hour') return pad((now.getHours() + Number(offset)) % 24);
  // a line that leaves at the same minutes past every quarter hour
  const wait = ((((Number(at) - now.getMinutes()) % 15) + 15) % 15) || 15;
  return String(wait + 15 * Number(nth));
}

// A word clock reads in fives: "it is twenty five to ten".
function litWords(now) {
  let hour = now.getHours();
  const m = Math.floor(now.getMinutes() / 5) * 5;
  const words = ['it', 'is'];
  if (m === 0) words.push('oclock');
  else if (m <= 30) words.push(...{ 5: ['five'], 10: ['ten'], 15: ['quarter'], 20: ['twenty'], 25: ['twenty', 'five'], 30: ['half'] }[m], 'past');
  else { words.push(...{ 35: ['twenty', 'five'], 40: ['twenty'], 45: ['quarter'], 50: ['ten'], 55: ['five'] }[m], 'to'); hour += 1; }
  words.push(`h${hour % 12 || 12}`);
  return words;
}

const formats = new Map();
// Minutes past midnight in a city, and how far ahead of here it is, across midnight and daylight saving.
function zoneMinutes(zone, now) {
  if (zone === 'local') return now.getHours() * 60 + now.getMinutes();
  if (!formats.has(zone)) {
    try { formats.set(zone, new Intl.DateTimeFormat('en-GB', { timeZone: zone, year: 'numeric', month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit', hourCycle: 'h23' })); } catch { formats.set(zone, null); }
  }
  const format = formats.get(zone);
  if (!format) return null;
  const parts = Object.fromEntries(format.formatToParts(now).map((p) => [p.type, p.value]));
  return (Number(parts.hour) % 24) * 60 + Number(parts.minute);
}
function zoneAhead(zone, now) {
  const format = formats.get(zone);
  if (zone === 'local' || !format) return 0;
  const p = Object.fromEntries(format.formatToParts(now).map((x) => [x.type, Number(x.value)]));
  const there = Date.UTC(p.year, p.month - 1, p.day, p.hour % 24, p.minute);
  const here = Date.UTC(now.getFullYear(), now.getMonth(), now.getDate(), now.getHours(), now.getMinutes());
  return Math.round((there - here) / 60000);
}

function moonNow(now) {
  const days = now.getTime() / 86400000 + 2440587.5 - 2451550.1;
  const phase = (((days / SYNODIC) % 1) + 1) % 1;
  const lit = (1 - Math.cos(phase * TAU)) / 2;
  const name = PHASES.find(([limit]) => phase < limit)[1];
  const until = (target) => Math.round(((((target - phase) % 1) + 1) % 1) * SYNODIC);
  const [event, days2] = phase < 0.5 ? ['full moon', until(0.5)] : ['new moon', until(1)];
  const when = days2 === 0 ? 'tonight' : `in ${days2} ${days2 === 1 ? 'day' : 'days'}`;
  return { phase, lit, name, text: `${Math.round(lit * 100)}% lit · ${event} ${when}` };
}

export function createStandby(ctx) {
  const { screen, settings } = ctx;
  const q = (selector) => screen.querySelector(selector);
  const scapesEl = q('#scapes');
  const faces = [...scapesEl.querySelectorAll('.scape')].map((root) => ({ raster: createRaster(root), fresh: false }));
  const COUNT = faces.length;
  const marks = [...screen.querySelectorAll('#scape-index i')];
  const clockEls = [...screen.querySelectorAll('[data-clock]')];
  const cityEls = [...screen.querySelectorAll('[data-city]')];
  const diffEls = [...screen.querySelectorAll('[data-city-diff]')];
  const words = [...screen.querySelectorAll('#words i[data-w]')];
  const statusTime = q('#status-time');
  const pills = ['#pill-back', '#pill-out'].map((id) => ctx.glass(q(id)));
  const f = new Motion(0, { spring: SPRINGS.page, epsilon: 0.0005 });
  const second = new Motion(0, { spring: SPRINGS.tick, epsilon: 0.0005 });
  const tracker = velocityTracker();
  let index = 0;
  let drag = null;
  let lastSecond = -1;
  let lastMinute = '';
  let world = [0, 0, 0, 0];
  let moon = { phase: 0.5, lit: 1 };

  // A face's text is painted when the face comes near, and again after anything on it changed.
  function ensure(k) {
    const face = faces[k];
    if (!face || face.fresh) return;
    face.raster.draw();
    ctx.upload(`scape${k}`, face.raster.canvas, null);
    face.fresh = true;
  }
  const stale = () => { for (const face of faces) face.fresh = false; };
  function mark() {
    const k = Math.round(Math.max(0, Math.min(COUNT - 1, f.target)));
    marks.forEach((m, i) => m.classList.toggle('is-on', i === k));
  }

  // The face chosen in settings: standby opens on it, and moves to it at once if standby is showing.
  const names = [...scapesEl.querySelectorAll('.scape')].map((el) => el.dataset.scape);
  function showFace(name, animate) {
    const k = names.indexOf(name);
    if (k < 0 || k === index) return;
    index = k;
    if (animate) f.to(k, { spring: SPRINGS.page }); else f.set(k);
    mark();
  }
  ctx.onIndex((i, from) => { if (i === STANDBY && from !== STANDBY && settings.standbyFace) showFace(settings.standbyFace, false); });
  ctx.showStandbyFace = (name) => showFace(name, ctx.index === STANDBY);

  function updateClock(now) {
    const hm = `${pad(now.getHours())}:${pad(now.getMinutes())}`;
    if (hm === lastMinute) return;
    lastMinute = hm;
    const day = now.toLocaleDateString('en-GB', { weekday: 'long' }).toLowerCase();
    const date = now.toLocaleDateString('en-GB', { day: 'numeric', month: 'short' }).toLowerCase();
    for (const el of clockEls) {
      el.textContent = clockText(el, now, hm, day, date);
      if (!scapesEl.contains(el)) ctx.dirty(el);
    }
    statusTime.textContent = `${day.slice(0, 3)} ${date} · ${hm}`;
    ctx.dirty(statusTime);

    const lit = new Set(litWords(now));
    for (const w of words) w.classList.toggle('is-lit', lit.has(w.dataset.w));

    const here = now.getHours() * 60 + now.getMinutes();
    world = CITIES.map((zone, i) => {
      const minutes = zoneMinutes(zone, now) ?? here;
      const diff = zoneAhead(zone, now);
      if (cityEls[i]) cityEls[i].textContent = `${pad(Math.floor(minutes / 60))}:${pad(minutes % 60)}`;
      if (diffEls[i]) diffEls[i].textContent = i === 0 ? 'here' : diff === 0 ? 'same time' : `${diff > 0 ? '+' : '−'}${Math.abs(diff) / 60}h`;
      return (((diff / 60) % 24) + 36) % 24 - 12;
    });

    moon = moonNow(now);
    q('#moon-phase').textContent = moon.name;
    q('#moon-lit').textContent = moon.text;
    const energy = ctx.energy?.summary();
    if (energy) {
      q('#tree-day').textContent = `day ${energy.days} of growing`;
      q('#tree-note').textContent = `${energy.today.toFixed(1)} kWh saved today`;
    }
    stale();
  }

  // The actions follow the page spring between two faces' rooms, so they move exactly as the faces do.
  function placeActions(fv) {
    const u = ctx.unit;
    const k0 = Math.max(0, Math.min(COUNT - 1, Math.floor(fv)));
    const k1 = Math.min(COUNT - 1, k0 + 1);
    const t = Math.max(0, Math.min(1, fv - k0));
    pills.forEach((g, i) => {
      if (!g?.base) return;
      const w = g.base.w / u;
      const baseX = (g.base.x - STANDBY * ctx.W) / u + w / 2;
      const baseY = (g.base.y + g.base.h / 2) / u;
      const at = (k) => {
        const room = ACTIONS[k] ?? ACTIONS[0];
        if (room.split === undefined) return [baseX, room.row];
        return [i === 0 ? 30 + w / 2 : 690 - w / 2, room.split];
      };
      const [ax, ay] = at(k0);
      const [bx, by] = at(k1);
      g.dx = (ax + (bx - ax) * t - baseX) * u;
      g.dy = (ay + (by - ay) * t - baseY) * u;
    });
  }

  ctx.onMeasure({
    before() { scapesEl.style.transform = 'none'; },
    after() { lastMinute = ''; updateClock(new Date()); },
  });
  ctx.onRetone(stale);

  ctx.setVerticalPan(STANDBY, {
    quiet: true,
    begin() { drag = { f0: f.value, from: index }; tracker.reset(); },
    move(dy, t) {
      const raw = drag.f0 - dy / ctx.H;
      const last = COUNT - 1;
      const shown = raw < 0 ? -rubberBand(-raw, 1) : raw > last ? last + rubberBand(raw - last, 1) : raw;
      tracker.add(t, shown * ctx.H);
      f.set(shown, tracker.velocity(t) / ctx.H);
    },
    end(t) {
      const v = tracker.velocity(t);
      index = pageFor(-f.value * ctx.H, -v, ctx.H, drag.from, COUNT);
      f.to(index, { spring: SPRINGS.page, velocity: v / ctx.H });
      drag = null;
      mark();
    },
  });

  ctx.onFrame(() => {
    const now = new Date();
    updateClock(now);
    if (updateFaces2(scapesEl, now, ctx)) stale();
    // The second hand steps into each second on a spring, like a quartz movement settling into its detent.
    const secs = Math.floor(now.getTime() / 1000);
    if (secs !== lastSecond) {
      if (lastSecond < 0 || secs - lastSecond > 3) second.set((now.getSeconds() / 60) * TAU);
      else second.to(second.target + ((secs - lastSecond) * TAU) / 60, { spring: SPRINGS.tick });
      lastSecond = secs;
    }
    const hours = now.getHours() + now.getMinutes() / 60 + now.getSeconds() / 3600;
    const minutes = now.getMinutes() + now.getSeconds() / 60;

    const fv = f.value;
    scapesEl.style.transform = `translate3d(0, ${-fv * ctx.H}px, 0)`;
    const x = STANDBY * ctx.W + ctx.page;
    const onScreen = x < ctx.W && x + ctx.W > 0;
    const k0 = Math.max(0, Math.min(COUNT - 1, Math.floor(fv)));
    const scapeDraws = [];
    if (onScreen) {
      for (const k of [k0, k0 + 1]) {
        if (k >= COUNT) continue;
        ensure(k);
        scapeDraws.push({ name: `scape${k}`, x, y: (k - fv) * ctx.H, w: faces[k].raster.width, h: faces[k].raster.height });
      }
      placeActions(fv);
    }
    const u = ctx.unit;
    const tree = 7;
    return {
      scapeF: fv,
      clock: [hours, minutes, second.value % TAU, flipLeft(now, settings.calm)],
      digits: [Math.floor(now.getHours() / 10), now.getHours() % 10, Math.floor(now.getMinutes() / 10), now.getMinutes() % 10],
      scapeDraws,
      world,
      moon: [moon.phase, moon.lit, ...skyNow(now)],
      sphere: {
        x: x + 360 * u, y: -fv * ctx.H + 292 * u, r: 128 * u, size: 3.1,
        alpha: onScreen ? Math.max(0, 1 - Math.abs(fv)) : 0, colA: settings.rgb.sphereA, colB: settings.rgb.sphereB,
      },
      treeFace: onScreen && Math.abs(fv - tree) < 1 ? { x, y: (tree - fv) * ctx.H, w: ctx.W, h: ctx.H } : null,
    };
  });

  mark();
}
