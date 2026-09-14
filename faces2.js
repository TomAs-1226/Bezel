// Live text for standby faces 8–11 (flip clock, album, orrery, next up), and the spare uniform components their
// shaders read. updateFaces2 is cheap enough for every frame: it touches the DOM only when something changed and
// returns true then, so the caller can mark the faces' text stale.

const pad = (n) => String(n).padStart(2, '0');
const minutesOf = (hm) => { const [h, m] = hm.split(':').map(Number); return h * 60 + m; };

/** Where faces 8–11 leave room for the standby actions, in panel pixels: a centred row at a height. */
export const ACTIONS2 = [
  { row: 520 }, // flip clock: under the cards
  { row: 548 }, // album: under the track
  { row: 548 }, // orrery: under the outer orbit
  { row: 520 }, // next up: under the day's rows
];

/** Seconds at the top of each minute in which the flip clock's flaps fall and settle. */
export const FLIP_WINDOW = 0.6;

/** uClock.w: seconds left in the flip window, 0 once settled. Calm mode passes 0, so numbers change without motion. */
export function flipLeft(now, calm) {
  if (calm) return 0;
  return Math.max(0, FLIP_WINDOW - (now.getSeconds() + now.getMilliseconds() / 1000));
}

/** uMoon.zw for the orrery: weekday + 1 (Sunday = 1; 0 means unknown) and days since J2000.0. */
export function skyNow(now) {
  return [now.getDay() + 1, now.getTime() / 86400000 - 10957.5];
}

// The weekdays keep the old planetary names: Sunday the sun, Monday the moon, Tuesday Mars (dies Martis)…
const NAMESAKES = ['the sun', 'the moon', 'mars', 'mercury', 'jupiter', 'venus', 'saturn'];

// The next-up face's day arc: midnight at (40, 368), noon at (360, 296), midnight again at (680, 368).
// faces2.glsl.js draws the same circle; the event ticks are raster dots placed on it here.
const ARC = { cx: 360, cy: 1043.1, r: 747.1, half: 0.44263 };
const onArc = (fraction) => {
  const a = -ARC.half + 2 * ARC.half * fraction;
  return [ARC.cx + ARC.r * Math.sin(a), ARC.cy - ARC.r * Math.cos(a)];
};
// An event reads "now" for its first ten minutes, then the countdown moves on.
const LINGER = 10;

const found = new WeakMap();
function parts(root) {
  if (found.has(root)) return found.get(root);
  const q = (selector) => root.querySelector(selector);
  const rows = [...root.querySelectorAll('.next-row[data-at]')]
    .map((el) => ({ el, at: el.dataset.at, start: minutesOf(el.dataset.at), title: el.querySelector('.next-row-title')?.textContent ?? '', tick: null }))
    .sort((a, b) => a.start - b.start);
  const p = {
    minute: -1,
    flip: q('[data-flip]'),
    title: q('[data-np="title"]'), by: q('[data-np="by"]'),
    day: q('[data-orrery="day"]'), month: q('[data-orrery="month"]'), namesake: q('[data-orrery="namesake"]'),
    count: q('[data-next="count"]'), h: q('[data-next="h"]'), hu: q('[data-next="hu"]'), m: q('[data-next="m"]'), mu: q('[data-next="mu"]'),
    what: q('[data-next="title"]'), when: q('[data-next="when"]'), ticks: q('[data-next="ticks"]'),
    rows,
  };
  found.set(root, p);
  return p;
}

const setText = (el, text) => { if (!el || el.textContent === text) return false; el.textContent = text; return true; };
const setHidden = (el, hidden) => { if (!el || el.hidden === hidden) return false; el.hidden = hidden; return true; };
const setClass = (el, name, on) => { if (!el || el.classList.contains(name) === on) return false; el.classList.toggle(name, on); return true; };
// Labels for assistive tech are not painted, so they never make the text stale.
const setLabel = (el, label) => { if (el && el.getAttribute('aria-label') !== label) el.setAttribute('aria-label', label); };
const plural = (n, word) => `${n} ${word}${n === 1 ? '' : 's'}`;

function placeTicks(p) {
  if (!p.ticks || p.ticks.children.length || !p.rows.length) return false;
  for (const row of p.rows) {
    const tick = document.createElement('i');
    tick.className = 'next-tick';
    tick.dataset.raster = '';
    const [x, y] = onArc(row.start / 1440);
    tick.style.left = `calc(${x.toFixed(1)} * var(--px))`;
    tick.style.top = `calc(${y.toFixed(1)} * var(--px))`;
    p.ticks.append(tick);
    row.tick = tick;
  }
  return true;
}

function nextUp(p, now) {
  if (!p.rows.length) return false;
  let changed = placeTicks(p);
  const here = now.getHours() * 60 + now.getMinutes() + now.getSeconds() / 60;
  let next = p.rows.find((row) => row.start + LINGER > here);
  const tomorrow = !next;
  if (tomorrow) next = p.rows[0];
  const wait = next.start + (tomorrow ? 1440 : 0) - here;

  let label;
  if (wait <= 0) {
    changed = setText(p.h, 'now') || changed;
    changed = setHidden(p.h, false) || changed;
    for (const el of [p.hu, p.m, p.mu]) changed = setHidden(el, true) || changed;
    label = `${next.title} now`;
  } else {
    const whole = Math.ceil(wait - 1e-6);
    const hours = Math.floor(whole / 60);
    const mins = whole % 60;
    changed = setText(p.h, String(hours)) || changed;
    changed = setText(p.m, String(mins)) || changed;
    changed = setHidden(p.h, hours === 0) || changed;
    changed = setHidden(p.hu, hours === 0) || changed;
    changed = setHidden(p.m, hours > 0 && mins === 0) || changed;
    changed = setHidden(p.mu, hours > 0 && mins === 0) || changed;
    label = `${next.title} in ${[hours ? plural(hours, 'hour') : '', mins ? plural(mins, 'minute') : ''].filter(Boolean).join(' ')}`;
  }
  setLabel(p.count, label);
  changed = setText(p.what, next.title) || changed;
  changed = setText(p.when, `${wait <= 0 ? 'now' : tomorrow ? 'tomorrow' : 'today'} · ${next.at}`) || changed;

  for (const row of p.rows) {
    const isNext = row === next;
    const isPast = !isNext && (tomorrow || row.start + LINGER <= here);
    for (const el of [row.el, row.tick]) {
      changed = setClass(el, 'is-next', isNext) || changed;
      changed = setClass(el, 'is-past', isPast) || changed;
    }
  }
  return changed;
}

/**
 * Sets the live text on faces 8–11: the flip clock's spoken time, the playing track, the orrery's date and
 * namesake, and the next-up countdown. Clock-driven text is recomputed once a minute; the track is checked each call.
 * @param {Element} root  #scapes, or any element containing the four faces
 * @param {Date} now
 * @param {{ nowPlaying?: () => { title: string, by: string, album?: string, playing?: boolean } }} ctx
 * @returns {boolean} true when painted text changed
 */
export function updateFaces2(root, now, ctx) {
  const p = parts(root);
  let changed = false;
  const minute = Math.floor(now.getTime() / 60000);
  if (p.minute !== minute) {
    p.minute = minute;
    setLabel(p.flip, `${pad(now.getHours())}:${pad(now.getMinutes())}`);
    const weekday = now.toLocaleDateString('en-GB', { weekday: 'long' }).toLowerCase();
    const month = now.toLocaleDateString('en-GB', { month: 'long' }).toLowerCase();
    changed = setText(p.day, String(now.getDate())) || changed;
    changed = setText(p.month, `${weekday} · ${month}`) || changed;
    changed = setText(p.namesake, `day of ${NAMESAKES[now.getDay()]}`) || changed;
    changed = nextUp(p, now) || changed;
  }
  const track = ctx?.nowPlaying?.();
  if (track) {
    changed = setText(p.title, track.title) || changed;
    changed = setText(p.by, [track.by, track.album, track.playing === false ? 'paused' : ''].filter(Boolean).join(' · ')) || changed;
  }
  return changed;
}
