// Energy: a low-poly tree that grows with what the house saves. Each day's saving against a usual day adds
// growth steps; a branch or a leaf cluster appears at each step, so a good week shows as new wood and leaves.
// Today's saving is live: dim a room or lower the heating and the estimate, and the tree, respond.
// The crown follows the month and sways in the wind. Every 7 days in a row hang a fruit on it, and caring for
// the plants adds blossoms. Hold or drag across the week to see the tree as it was on that day.

import { Motion, SPRINGS, project, velocityTracker } from './motion.js';
import { KIND, TREE_FLOATS } from './treepass.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const mix = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t);
const USUAL = 11.2; // kWh on a usual day for this example house
const PER_KWH = 2.2; // growth steps for each kWh saved on a day that counts
// The last three weeks, oldest first. Example data; a day counts when it saved at least 1 kWh.
const HISTORY = [2.1, 2.6, 0.4, 3.1, 2.2, 1.8, 2.9, 3.3, 0.6, 2.4, 2.8, 3.6, 1.9, 2.7, 3.0, 2.2, 1.4, 2.5, 3.2, 2.9];
const FRUIT = 6; // the crown keeps room for this many fruit, one for every 7 days in a row
const BLOSSOMS = 12; // and for this many blossoms, three for each plant cared for today
const DAY = 86400000;
const GLOW = [0.1, 0.13, 0.05, 1];
const LOOK_BACK_GLOW = [0.1, 0.13, 0.05, 2.4];

// The crown month by month, for the northern hemisphere: the share of leaf clusters in leaf, their size, and
// the tones they take. Winter is bare except for pale tips at the ends of the twigs.
const MONTHS = [
  { tips: true }, // january
  { tips: true }, // february
  { keep: 0.6, size: 0.72, tones: { bud: 0.45, blossom: 0.55 } }, // march
  { keep: 1, size: 0.92, tones: { blossom: 0.7, fresh: 0.3 } }, // april
  { keep: 1, size: 1, tones: { fresh: 0.65, blossom: 0.35 } }, // may
  { keep: 1, size: 1.08, tones: { leaf: 0.65, deep: 0.35 } }, // june
  { keep: 1, size: 1.1, tones: { leaf: 0.55, deep: 0.45 } }, // july
  { keep: 1, size: 1.08, tones: { leaf: 0.5, deep: 0.4, gold: 0.1 } }, // august
  { keep: 1, size: 1, tones: { leaf: 0.35, gold: 0.4, red: 0.25 } }, // september
  { keep: 0.9, size: 0.95, tones: { gold: 0.45, red: 0.45, leaf: 0.1 } }, // october
  { keep: 0.45, size: 0.85, tones: { red: 0.5, rust: 0.5 } }, // november
  { tips: true }, // december
];

const ICO = (() => {
  const t = (1 + Math.sqrt(5)) / 2;
  return [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t], [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]];
})();
const ICO_FACES = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8],
  [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1]];
const OCTA_FACES = [[0, 2, 4], [2, 1, 4], [1, 3, 4], [3, 0, 4], [2, 0, 5], [1, 2, 5], [3, 1, 5], [0, 3, 5]];

function random(seed) {
  let a = seed >>> 0;
  return () => { a = (a * 1664525 + 1013904223) >>> 0; return a / 4294967296; };
}

/** Colours for the tree, from the panel's own tokens, so Monet and the tone carry into it. */
function palette(rgb, dark) {
  const leaf = rgb('--d-leaf');
  const amber = rgb('--d-amber');
  return {
    wood: dark ? [0.36, 0.25, 0.18] : [0.46, 0.33, 0.24],
    leaf,
    deep: leaf.map((v) => v * 0.74),
    fresh: mix(leaf, [0.84, 0.95, 0.6], 0.3),
    bud: mix(leaf, [0.9, 0.94, 0.8], 0.5),
    blossom: dark ? [0.96, 0.8, 0.86] : [0.93, 0.68, 0.77],
    gold: amber.map((v) => v * 0.95),
    red: mix(amber, [0.76, 0.2, 0.12], 0.7),
    rust: mix(amber, [0.52, 0.25, 0.13], 0.72),
    tip: dark ? [0.84, 0.88, 0.83] : [0.7, 0.76, 0.7],
    fruit: mix(amber, [0.96, 0.5, 0.08], 0.4),
    flower: dark ? [0.99, 0.9, 0.94] : [0.97, 0.8, 0.87],
  };
}

const pickTone = (tones, r) => {
  const entries = Object.entries(tones);
  let sum = 0;
  for (const [name, weight] of entries) { sum += weight; if (r < sum) return name; }
  return entries[entries.length - 1][0];
};

/** Spreads n picks around a list sorted by angle, so consecutive picks land on different sides. */
function spread(list, n) {
  const used = new Set();
  const out = [];
  for (let i = 0; out.length < Math.min(n, list.length); i++) {
    let k = Math.floor(((i * 0.618034) % 1) * list.length);
    while (used.has(k)) k = (k + 1) % list.length;
    used.add(k);
    out.push(list[k]);
  }
  return out;
}

// Builds the whole grown tree once: prisms for wood, faceted icosahedra for leaves, each tagged with its step.
// The shape comes from one seeded sequence and the month's look from another, so the tree keeps its shape and
// its steps all year. `ready` is the growth before this week: fruit hangs only on wood that already exists.
function buildTree(c, look, ready) {
  const rand = random(1226);
  const roll = random(4711);
  const out = [];
  const clusters = [];
  let step = 0;
  const push = (a, b, d, colour, pivot, order, kind = KIND.wood, index = 0) => {
    const n = [(b[1] - a[1]) * (d[2] - a[2]) - (b[2] - a[2]) * (d[1] - a[1]), (b[2] - a[2]) * (d[0] - a[0]) - (b[0] - a[0]) * (d[2] - a[2]), (b[0] - a[0]) * (d[1] - a[1]) - (b[1] - a[1]) * (d[0] - a[0])];
    const len = Math.hypot(...n) || 1;
    for (const p of [a, b, d]) out.push(p[0], p[1], p[2], n[0] / len, n[1] / len, n[2] / len, colour[0], colour[1], colour[2], pivot[0], pivot[1], pivot[2], order, kind, index);
  };
  const basis = (dir) => {
    const up = Math.abs(dir[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
    const u = [dir[1] * up[2] - dir[2] * up[1], dir[2] * up[0] - dir[0] * up[2], dir[0] * up[1] - dir[1] * up[0]];
    const ul = Math.hypot(...u);
    const x = u.map((v) => v / ul);
    const y = [dir[1] * x[2] - dir[2] * x[1], dir[2] * x[0] - dir[0] * x[2], dir[0] * x[1] - dir[1] * x[0]];
    return [x, y];
  };
  const prism = (base, dir, length, r0, r1, colour, order) => {
    const [bx, by] = basis(dir);
    const top = base.map((v, i) => v + dir[i] * length);
    const ring = (centre, r, k) => { const a = (k / 5) * Math.PI * 2; return centre.map((v, i) => v + (bx[i] * Math.cos(a) + by[i] * Math.sin(a)) * r); };
    const shade = 0.9 + rand() * 0.2;
    const col = colour.map((v) => v * shade);
    for (let k = 0; k < 5; k++) {
      const a = ring(base, r0, k), b = ring(base, r0, k + 1), d = ring(top, r1, k), e = ring(top, r1, k + 1);
      push(a, b, e, col, base, order);
      push(a, e, d, col, base, order);
    }
    return top;
  };
  // A leaf cluster's shape is drawn here whether or not the month shows it.
  const cluster = (centre, r, order, terminal) => {
    const dirs = ICO.map((p) => { const l = Math.hypot(...p); return p.map((x) => (x / l) * (0.85 + rand() * 0.3)); });
    const shades = ICO_FACES.map(() => rand());
    clusters.push({ centre, r, order, terminal, dirs, shades });
  };
  const grow = (base, dir, length, radius, depth) => {
    const order = step++;
    const top = prism(base, dir, length, radius, radius * 0.68, c.wood, order);
    if (depth >= 4) { cluster(top, 0.2 + rand() * 0.1, step++, true); return; }
    const children = depth === 0 ? 3 : 2 + (rand() < 0.4 ? 1 : 0);
    for (let k = 0; k < children; k++) {
      const spin = (k / children) * Math.PI * 2 + rand() * 1.2 + depth;
      const lean = 0.42 + rand() * 0.3;
      const d = [Math.sin(lean) * Math.cos(spin) * 0.9 + dir[0] * 0.5, Math.cos(lean) + dir[1] * 0.3, Math.sin(lean) * Math.sin(spin) * 0.9 + dir[2] * 0.5];
      const l = Math.hypot(...d);
      grow(top, d.map((x) => x / l), length * (0.72 + rand() * 0.1), radius * 0.66, depth + 1);
    }
    if (depth >= 2 && rand() < 0.6) cluster(top, 0.16 + rand() * 0.08, step++, false);
  };
  grow([0, 0, 0], [0, 1, 0], 0.62, 0.11, 0);

  // Fruit and blossoms hang from twig ends spread around the crown.
  const byAngle = (list) => list.slice().sort((a, b) => Math.atan2(a.centre[2], a.centre[0]) - Math.atan2(b.centre[2], b.centre[0]));
  const ends = clusters.filter((k) => k.terminal);
  const early = ends.filter((k) => k.order < ready);
  const fruitHosts = spread(byAngle(early.length >= FRUIT ? early : ends), FRUIT);
  const blossomHosts = spread(byAngle(ends.filter((k) => !fruitHosts.includes(k))), BLOSSOMS);
  const hosts = new Set([...fruitHosts, ...blossomHosts]);
  const size = look.tips ? 0.34 : look.size;

  for (const k of clusters) {
    let tone = null;
    if (look.tips) tone = k.terminal ? 'tip' : null;
    else if (roll() < look.keep || hosts.has(k)) tone = pickTone(look.tones, roll());
    if (!tone) continue;
    const v = k.dirs.map((d) => k.centre.map((x, i) => x + d[i] * k.r * size));
    ICO_FACES.forEach(([a, b, d], f) => push(v[a], v[b], v[d], c[tone].map((x) => x * (0.82 + k.shades[f] * 0.32)), k.centre, k.order, KIND.leaf));
  }

  const octa = (centre, r, stretch, colour, order, kind, index) => {
    const p = [[r, 0, 0], [-r, 0, 0], [0, r * stretch, 0], [0, -r * stretch, 0], [0, 0, r], [0, 0, -r]].map((o) => centre.map((x, i) => x + o[i]));
    for (const [a, b, d] of OCTA_FACES) push(p[a], p[b], p[d], colour, centre, order, kind, index);
  };
  const outward = (k) => { const l = Math.hypot(k.centre[0], k.centre[2]); return l > 1e-3 ? [k.centre[0] / l, 0, k.centre[2] / l] : [1, 0, 0]; };
  // Fruit hangs on a short stalk below the outer edge of its cluster, where the crown's shade sets it off.
  fruitHosts.forEach((k, i) => {
    const o = outward(k);
    const r = k.r * size;
    const top = [k.centre[0] + o[0] * r * 0.55, k.centre[1] - r * 0.95, k.centre[2] + o[2] * r * 0.55];
    const centre = [top[0], top[1] - 0.14, top[2]];
    const end = [top[0], centre[1] + 0.065, top[2]];
    const ring = (p, a) => [p[0] + Math.cos(a) * 0.008, p[1], p[2] + Math.sin(a) * 0.008];
    for (let s = 0; s < 3; s++) {
      const a = (s / 3) * Math.PI * 2;
      const b = ((s + 1) / 3) * Math.PI * 2;
      push(ring(top, a), ring(end, a), ring(end, b), c.wood, top, k.order, KIND.fruit, i);
      push(ring(top, a), ring(end, b), ring(top, b), c.wood, top, k.order, KIND.fruit, i);
    }
    octa(centre, 0.062, 1.15, c.fruit, k.order, KIND.fruit, i);
  });
  // Blossoms sit just outside the top of their cluster, so the leaves never swallow them.
  blossomHosts.forEach((k, i) => {
    const o = outward(k);
    const d = [o[0], 0.9, o[2]];
    const l = Math.hypot(...d);
    const reach = k.r * size * 1.12 + (look.tips ? 0.01 : 0.035);
    octa(k.centre.map((x, j) => x + (d[j] / l) * reach), 0.05, 0.8, i % 3 === 2 ? c.blossom : c.flower, k.order, KIND.blossom, i);
  });

  return { data: new Float32Array(out), steps: step, leaves: clusters.map((k) => k.order).sort((a, b) => a - b) };
}

export function createEnergy(ctx, { pass }) {
  const { screen, settings } = ctx;
  const q = (selector) => screen.querySelector(selector);
  const stage = q('#tree-stage');
  const weekEl = q('#energy-week');
  const bars = [...screen.querySelectorAll('#energy-week .day-bar')];
  const whenEl = q('#energy-saved')?.previousElementSibling ?? null; // the "saved today" label
  const nextText = q('#energy-next-text');
  const nextTrack = q('#energy-next-track');
  const nextFill = q('#energy-next-fill');
  const yaw = new Motion(0.6, { spring: SPRINGS.smooth, epsilon: 0.0005 });
  const growth = new Motion(0, { spring: SPRINGS.smooth, epsilon: 0.01 });
  const fruit = new Motion(0, { spring: SPRINGS.smooth, epsilon: 0.005 });
  const bloom = new Motion(0, { spring: SPRINGS.smooth, epsilon: 0.005 });
  const wind = new Motion(settings.calm ? 0 : 1, { spring: SPRINGS.smooth, epsilon: 0.002 });
  const tracker = velocityTracker();
  const scrubTracker = velocityTracker();
  let tree = null;
  let rect = null;
  let from = 0;
  let dragging = false;
  let shownToday = '';
  let builtFor = -1;
  let windTime = 0;
  let checked = -Infinity;
  let scrub = null;

  function today() {
    const { levels, setpoint } = ctx.home;
    const lightsOn = Object.values(levels).reduce((sum, m) => sum + Math.max(0, m.target), 0);
    const lights = clamp(2.2 - lightsOn * 0.45, 0, 2.2);
    const heat = clamp((22.5 - setpoint.target) * 0.7, 0, 3);
    return { lights, heat, standby: 0.4, total: lights + heat + 0.4 };
  }
  const stepsFor = (days) => days.reduce((sum, saved) => sum + (saved >= 1 ? Math.round(saved * PER_KWH) : 0), 0);
  const days = () => HISTORY.concat(today().total);
  // Days in a row that saved at least 1 kWh, up to day i. Today doesn't break a run while it is still under way.
  function streakAt(list, i) {
    let k = list[i] >= 1 ? i : i === list.length - 1 ? i - 1 : -1;
    let n = 0;
    for (; k >= 0 && list[k] >= 1; k--) n++;
    return n;
  }
  // One fruit for every 7 days in a row, up to day i. A broken run keeps the fruit it earned.
  function fruitAt(list, i) {
    let n = 0;
    let run = 0;
    for (let k = 0; k <= i; k++) {
      if (list[k] >= 1) { run++; if (run % 7 === 0) n++; } else if (k < list.length - 1) run = 0;
    }
    return Math.min(FRUIT, n);
  }
  const fruitToday = () => { const list = days(); return fruitAt(list, list.length - 1); };
  function summary() {
    const list = days();
    return { today: list[list.length - 1], days: list.filter((d) => d >= 1).length, streak: streakAt(list, list.length - 1), fruit: fruitAt(list, list.length - 1) };
  }
  ctx.energy = { summary };

  // Growth at the end of each day in the week chart (the six days before today, then today).
  function weekGrowth() {
    const list = days();
    const before = stepsFor(HISTORY.slice(0, -6));
    let g = before;
    const ends = list.slice(-7).map((saved) => Math.min(tree.steps, (g += saved >= 1 ? Math.round(saved * PER_KWH) : 0)));
    return { list, before: Math.min(tree.steps, before), ends };
  }
  const target = () => weekGrowth().ends[6];
  const freshToday = () => { const { before, ends, list } = weekGrowth(); return list[list.length - 1] >= 1 ? [ends[5] ?? before, ends[6] - 0.5] : null; };

  function build() {
    const c = getComputedStyle(screen.closest('.device'));
    const rgb = (name) => { const h = c.getPropertyValue(name).trim().replace('#', ''); const n = parseInt(h, 16); return [(n >> 16 & 255) / 255, (n >> 8 & 255) / 255, (n & 255) / 255]; };
    builtFor = new Date().getMonth();
    tree = buildTree(palette(rgb, settings.tone === 'dark'), MONTHS[builtFor], stepsFor(HISTORY.slice(0, -6)));
    pass.setMesh(tree.data);
  }

  // How much more today has to save before the next leaf cluster comes out, and how far along it is.
  function nextLeaf(total) {
    const base = stepsFor(HISTORY);
    const have = base + (total >= 1 ? Math.round(total * PER_KWH) : 0);
    // a piece is out once growth passes its step, and today's steps are round(kWh × 2.2) from 1 kWh up
    const kwhFor = (order) => Math.max(1, (order + 0.5 - base) / PER_KWH);
    const next = tree.leaves.find((o) => o >= have);
    if (next === undefined) return { text: 'every branch is in leaf', progress: 1 };
    const prev = tree.leaves.filter((o) => o >= base && o < have).pop();
    const at = kwhFor(next);
    const since = prev === undefined ? 0 : kwhFor(prev);
    return { text: `next leaf in ${Math.max(0.1, at - total).toFixed(1)} kWh`, progress: clamp((total - since) / Math.max(at - since, 0.01), 0, 1) };
  }

  function setWhen(text) {
    const node = whenEl && [...whenEl.childNodes].reverse().find((n) => n.nodeType === 3 && n.data.trim());
    if (!node || node.data === text) return;
    node.data = text;
    ctx.dirty(whenEl);
  }

  function update() {
    if (scrub) return;
    const t = today();
    const s = summary();
    const next = nextLeaf(t.total);
    const key = `${t.total.toFixed(2)}·${s.streak}·${new Date().getDate()}`;
    if (key === shownToday) return;
    shownToday = key;
    const text = t.total.toFixed(1);
    setWhen('saved today');
    q('#energy-saved').textContent = `${text} kWh`;
    q('#energy-usual').textContent = `of ${USUAL} kWh on a usual ${new Date().toLocaleDateString('en-GB', { weekday: 'long' }).toLowerCase()}`;
    q('#save-lights').textContent = t.lights.toFixed(1);
    q('#save-heat').textContent = t.heat.toFixed(1);
    q('#save-standby').textContent = t.standby.toFixed(1);
    q('#energy-streak').textContent = `${s.streak} ${s.streak === 1 ? 'day' : 'days'} in a row`;
    q('#energy-sub').textContent = `day ${s.days} of your tree · it grows on what you save`;
    ctx.dirty(q('#energy-sub'));
    const week = HISTORY.slice(-6).concat(t.total);
    bars.forEach((bar, i) => {
      bar.style.height = `${Math.round(clamp(week[i] / 4, 0.08, 1) * 100)}%`;
      bar.classList.toggle('is-miss', week[i] < 1);
      bar.classList.toggle('is-today', i === 6);
      bar.classList.remove('is-scrub');
      // the letters under the bars follow the real days, today last
      const letter = bar.parentElement?.querySelector('.mono');
      const day = new Date(Date.now() - (6 - i) * DAY).toLocaleDateString('en-GB', { weekday: 'narrow' }).toLowerCase();
      if (letter && letter.textContent !== day) letter.textContent = day;
    });
    if (nextText) nextText.textContent = next.text;
    if (nextFill) nextFill.style.width = `${(next.progress * 100).toFixed(1)}%`;
    for (const id of ['#energy-saved', '#energy-usual', '#save-lights', '#save-heat', '#save-standby', '#energy-streak', '#energy-week', '#energy-next-text', '#energy-next-track']) {
      const el = q(id);
      if (el) ctx.dirty(el);
    }
  }

  // ---------- looking back: hold or drag across the week, and the tree becomes that day's ----------
  function scrubAt(clientX) {
    const c = scrub.centres;
    if (clientX <= c[0]) return 0;
    for (let i = 0; i < c.length - 1; i++) if (clientX <= c[i + 1]) return i + (clientX - c[i]) / Math.max(c[i + 1] - c[i], 1);
    return c.length - 1;
  }
  function growthAt(p) {
    const { ends } = weekGrowth();
    const i = Math.floor(p);
    return i >= 6 ? ends[6] : ends[i] + (ends[i + 1] - ends[i]) * (p - i);
  }
  function showDay(d) {
    if (scrub.day === d) return;
    scrub.day = d;
    const { list, before, ends } = weekGrowth();
    const i = list.length - 7 + d;
    const ago = 6 - d;
    const saved = list[i];
    const weekday = new Date(Date.now() - ago * DAY).toLocaleDateString('en-GB', { weekday: 'long' }).toLowerCase();
    scrub.fresh = saved >= 1 ? [d ? ends[d - 1] : before, ends[d] - 0.5] : null;
    const t = today();
    setWhen(ago === 0 ? 'saved today' : ago === 1 ? 'saved yesterday' : `saved on ${weekday}`);
    q('#energy-saved').textContent = `${saved.toFixed(1)} kWh`;
    q('#energy-usual').textContent = `of ${USUAL} kWh on a usual ${weekday}`;
    // the breakdown is only known for today
    q('#save-lights').textContent = ago ? '–' : t.lights.toFixed(1);
    q('#save-heat').textContent = ago ? '–' : t.heat.toFixed(1);
    q('#save-standby').textContent = ago ? '–' : t.standby.toFixed(1);
    const streak = streakAt(list, i);
    q('#energy-streak').textContent = `${streak} ${streak === 1 ? 'day' : 'days'} in a row`;
    bars.forEach((bar, k) => bar.classList.toggle('is-scrub', k === d));
    fruit.to(fruitAt(list, i), { spring: SPRINGS.smooth });
    for (const id of ['#energy-saved', '#energy-usual', '#save-lights', '#save-heat', '#save-standby', '#energy-streak', '#energy-week']) ctx.dirty(q(id));
  }
  function release(t) {
    if (!scrub) return;
    scrub = null;
    growth.to(target(), { spring: SPRINGS.smooth, velocity: scrubTracker.velocity(t) });
    fruit.to(fruitToday(), { spring: SPRINGS.smooth });
    shownToday = '';
    update();
  }
  if (weekEl) {
    weekEl.setAttribute('aria-label', 'This week. Hold or drag across a day to see the tree as it was.');
    weekEl.addEventListener('pointerdown', (e) => {
      if (e.button > 0 || !tree || scrub) return;
      scrub = { id: e.pointerId, x0: e.clientX, y0: e.clientY, along: null, day: -1, fresh: null, centres: bars.map((b) => { const r = b.getBoundingClientRect(); return r.left + r.width / 2; }) };
      scrubTracker.reset();
      const p = scrubAt(e.clientX);
      // the press answers at once: the tree heads for that day on the interactive spring
      growth.to(growthAt(p), { spring: SPRINGS.hold });
      showDay(Math.round(p));
    });
    // Sideways drags on the week are the week's own, so the page never takes them.
    ctx.addDragTarget(weekEl, { begin() {}, move() {}, end() {} }, 'x');
  }
  window.addEventListener('pointermove', (e) => {
    if (!scrub || e.pointerId !== scrub.id) return;
    const dx = e.clientX - scrub.x0, dy = e.clientY - scrub.y0;
    if (!scrub.along && Math.hypot(dx, dy) >= 10) scrub.along = Math.abs(dx) >= Math.abs(dy) ? 'x' : 'y';
    const t = e.timeStamp / 1000;
    if (scrub.along === 'y') { release(t); return; } // a vertical drag belongs to the window
    const p = scrubAt(e.clientX);
    const g = growthAt(p);
    scrubTracker.add(t, g);
    growth.set(g, scrubTracker.velocity(t));
    showDay(Math.round(p));
  });
  for (const type of ['pointerup', 'pointercancel']) {
    window.addEventListener(type, (e) => { if (scrub && e.pointerId === scrub.id) release(e.timeStamp / 1000); });
  }

  ctx.addDragTarget(stage, {
    begin() { from = yaw.value; dragging = true; tracker.reset(); },
    move(dx, t) {
      const value = from + dx / (180 * ctx.unit);
      tracker.add(t, value);
      yaw.set(value, tracker.velocity(t));
    },
    end(t) {
      dragging = false;
      const v = tracker.velocity(t);
      yaw.to(yaw.value + project(v, 0.994), { spring: SPRINGS.smooth, velocity: v });
    },
  }, 'x');

  // Measured with the window's transform set aside: the layer still carries the last close's scale on reopen.
  function measure() {
    const layer = stage.closest('.app-layer');
    const saved = layer ? layer.style.transform : '';
    if (layer) layer.style.transform = 'none';
    const s = screen.getBoundingClientRect();
    const r = stage.getBoundingClientRect();
    if (layer) layer.style.transform = saved;
    rect = { x: r.left - s.left, y: r.top - s.top, w: r.width, h: r.height };
  }
  ctx.onMeasure({ after() { if (rect) measure(); } });

  // Blossoms follow ctx.plants.careToday(), which may be missing or return true, a count or a list.
  function careBlossoms() {
    let care = 0;
    try { care = ctx.plants?.careToday?.(); } catch { care = 0; }
    const n = typeof care === 'number' ? care : Array.isArray(care) ? care.length : care && typeof care === 'object' ? Number(care.count ?? 1) : care ? 1 : 0;
    return clamp(Math.round(n || 0) * 3, 0, BLOSSOMS);
  }

  const lightDir = () => { const l = ctx.lightDir?.() ?? [-0.42, -0.91]; const v = [l[0], -l[1], 0.9]; const n = Math.hypot(...v); return v.map((x) => x / n); };

  ctx.onRetone(() => { if (tree) build(); shownToday = ''; });
  ctx.onFrame((t, dt) => {
    if (!tree) return {};
    if (!dragging && !yaw.moving && !settings.calm) yaw.set(yaw.value + dt * 0.12);
    // the calm version has no wind: the tree eases to rest and stays there
    if (!settings.calm) windTime += dt;
    const breeze = settings.calm ? 0 : 1;
    if (wind.target !== breeze) wind.to(breeze, { spring: SPRINGS.smooth });
    if (t - checked > 1) {
      checked = t;
      const blossoms = careBlossoms();
      if (blossoms !== bloom.target) bloom.to(blossoms, { spring: SPRINGS.smooth });
      if (new Date().getMonth() !== builtFor) build();
    }
    const ripe = fruitToday();
    if (!scrub && ripe !== fruit.target) fruit.to(ripe, { spring: SPRINGS.smooth });
    // standby's tree face shows the same tree at its grown size; standby supplies where the face is
    return {
      treeFaceParams: {
        yaw: yaw.value * 0.5 + t * (settings.calm ? 0 : 0.05), pitch: -0.12, growth: target(), light: lightDir(),
        wind: [windTime, wind.value], counts: [ripe, bloom.value],
      },
    };
  });

  return {
    build,
    summary,
    open() {
      measure();
      if (new Date().getMonth() !== builtFor) build();
      shownToday = '';
      update();
      // the tree grows from last week's size to today's each time you look, a cascade of new wood and leaves
      growth.set(Math.max(0, target() - 14));
      growth.to(target(), { spring: SPRINGS.smooth });
    },
    close() { if (scrub) release(performance.now() / 1000); },
    frame(t, dt, { win }) {
      if (!tree || !rect) return {};
      update();
      if (!scrub && Math.abs(growth.target - target()) > 0.5) growth.to(target(), { spring: SPRINGS.smooth });
      const view = { x: win.x + rect.x * win.sx, y: win.y + rect.y * win.sy, w: rect.w * win.sx, h: rect.h * win.sy };
      return {
        tree: {
          rect: view, yaw: yaw.value, pitch: -0.12, growth: growth.value, light: lightDir(),
          fresh: scrub ? scrub.fresh : freshToday(), glow: scrub ? LOOK_BACK_GLOW : GLOW,
          wind: [windTime, wind.value], counts: [fruit.value, bloom.value],
        },
      };
    },
  };
}
