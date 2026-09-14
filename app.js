// Boot, settings, colour, the one light and the frame loop.

import { createActivities } from './activities.js';
import { createApps } from './apps.js';
import { createControlCenter } from './cc.js';
import { wireControls } from './controls.js';
import { createCoverFlow } from './coverflow.js';
import { createCoverPass } from './coverpass.js';
import { createRenderer } from './glass.js';
import { createNotes, createSecurity, createSettings, createTimers } from './household.js';
import { createLauncher } from './launcher.js';
import { createAppliances, createCalendar, createPlants, createWeather } from './lifestyle.js';
import { loadMonet, monetTokens, seedFromImage } from './monet.js';
import { Motion, SPRINGS, cssSpring, setReducedMotion, tickMotions } from './motion.js';
import { wirePages } from './pages.js';
import { STANDBY, createPanel, hex, mixRgb } from './panel.js';
import { ALBUMS, COVERS, paintAtlas, paintCover } from './scenes.js';
import { createStandby } from './standby.js';
import { createEnergy } from './tree.js';
import { createTreePass } from './treepass.js';

const $ = (id) => document.getElementById(id);
const device = $('device');
const screen = $('screen');
const canvas = $('canvas');
const noteEl = document.querySelector('.stage-note');
const noteText = noteEl.textContent;
const query = (q) => window.matchMedia(q).matches;
const clamp1 = (v) => Math.max(-1, Math.min(1, v));

const settings = {
  tone: 'dark',
  variant: 'regular',
  light: 'pointer',
  colour: 'bezel',
  idle: 4000,
  calm: query('(prefers-reduced-motion: reduce)') || query('(prefers-reduced-transparency: reduce)'),
  playing: true,
  rgb: {},
};

// Detent springs as CSS transitions, for the little that CSS still animates.
for (const [name, s] of [['release', SPRINGS.release], ['smooth', SPRINGS.smooth]]) {
  const { duration, easing } = cssSpring(s);
  document.documentElement.style.setProperty(`--dur-${name}`, duration);
  document.documentElement.style.setProperty(`--ease-${name}`, easing);
}

const TOKENS = [
  ['ground', '--d-ground'], ['surface 1', '--d-s1'], ['surface 2', '--d-s2'], ['surface 3', '--d-s3'],
  ['ink', '--d-ink'], ['dim', '--d-dim'], ['signal', '--d-signal'], ['ice', '--d-ice'], ['amber', '--d-amber'],
  ['amber container', '--d-amber-container'], ['leaf', '--d-leaf'],
];

// The renderer's copy of the palette eases from old to new on the effects spring, as the surfaces do.
const palette = { from: null, to: null, fade: new Motion(1, { spring: SPRINGS.effect, epsilon: 0.002 }) };
const shownRgb = () => {
  const k = palette.fade.value;
  return Object.fromEntries(Object.keys(palette.to).map((key) => [key, mixRgb(palette.from[key], palette.to[key], k)]));
};

function readTokens(animate) {
  const style = getComputedStyle(device);
  const value = (name) => style.getPropertyValue(name).trim();
  const ice = hex(value('--d-ice'));
  const to = {
    ground: hex(value('--d-ground')), ink: hex(value('--d-ink')), ice,
    signal: hex(value('--d-signal')), solid: hex(value('--d-s2')),
    amber: hex(value('--d-amber')), leaf: hex(value('--d-leaf')),
    sphereA: hex(value('--d-signal')),
    sphereB: settings.tone === 'light' ? mixRgb(hex(value('--d-on-ice')), ice, 0.3) : ice,
  };
  palette.from = animate && palette.to ? shownRgb() : to;
  palette.to = to;
  palette.fade.set(animate ? 0 : 1);
  if (animate) palette.fade.to(1);
  settings.rgb = to;
  $('swatches').replaceChildren(...TOKENS.map(([label, name]) => {
    const card = document.createElement('div');
    card.className = 'swatch';
    const chip = document.createElement('i');
    chip.style.background = value(name);
    const title = document.createElement('b');
    title.textContent = label;
    const code = document.createElement('code');
    code.textContent = value(name);
    card.append(chip, title, code);
    return card;
  }));
}

let timer = 0;
function note(message) {
  noteEl.textContent = message;
  clearTimeout(timer);
  timer = setTimeout(() => { noteEl.textContent = noteText; }, 6000);
}

function select(id, value) {
  for (const b of $(id).querySelectorAll('button')) b.setAttribute('aria-pressed', String(b.dataset.value === value));
}
function segmented(id, key, onChange) {
  $(id).addEventListener('click', (event) => {
    const button = event.target.closest('button[data-value]');
    if (!button || button.disabled) return;
    select(id, button.dataset.value);
    settings[key] = button.dataset.value;
    onChange?.(button.dataset.value);
  });
}

// One light, as a direction. It rests above and to the left; the pointer or the device's tilt leans it.
const REST = [-0.42, -0.91];
const light = {
  x: new Motion(REST[0], { spring: SPRINGS.light, epsilon: 0.0005 }),
  y: new Motion(REST[1], { spring: SPRINGS.light, epsilon: 0.0005 }),
};
function leanLight(dx, dy) {
  const x = REST[0] + dx * 1.1;
  const y = REST[1] + dy * 0.9;
  const length = Math.hypot(x, y) || 1;
  light.x.to(x / length);
  light.y.to(y / length);
}
window.addEventListener('pointermove', (event) => {
  if (settings.light !== 'pointer') return;
  const box = screen.getBoundingClientRect();
  leanLight(clamp1((event.clientX - box.left - box.width / 2) / box.width), clamp1((event.clientY - box.top - box.height / 2) / box.height));
}, { passive: true });
document.documentElement.addEventListener('pointerleave', () => { if (settings.light === 'pointer') leanLight(0, 0); });

async function enableTilt() {
  const back = (message) => { settings.light = 'pointer'; select('light', 'pointer'); leanLight(0, 0); note(message); };
  try {
    if (typeof DeviceOrientationEvent !== 'undefined' && typeof DeviceOrientationEvent.requestPermission === 'function') {
      if (await DeviceOrientationEvent.requestPermission() !== 'granted') throw new Error('denied');
    }
  } catch {
    back('Motion access was not granted, so your pointer leans the light instead.');
    return;
  }
  let heard = false;
  window.addEventListener('deviceorientation', (event) => {
    if (settings.light !== 'tilt' || event.gamma == null) return;
    heard = true;
    leanLight(clamp1(-event.gamma / 35), clamp1(-(event.beta - 45) / 35));
  });
  setTimeout(() => { if (!heard && settings.light === 'tilt') back('No tilt sensor answered on this device, so your pointer leans the light instead.'); }, 1500);
}

// When auto standby is on, two quiet minutes send the panel to standby, as a wall display should.
let lastInput = performance.now();
for (const type of ['pointerdown', 'keydown', 'wheel']) window.addEventListener(type, () => { lastInput = performance.now(); }, { capture: true, passive: true });

let renderer = null;
let panel = null;
let settingsApp = null;
let ambient = 0;
let lastFrame = 0;

// ---------- colour: Bezel's own tokens, or Monet from the playing cover or a basic seed ----------
const covers = [];
const seeds = [];
let track = 0;
let monet = false;
let applied = [];
const coverFor = (k) => (covers[k] ??= paintCover(1024, COVERS[k], ALBUMS[k]));

function applyColour() {
  let tokens = null;
  if (monet && settings.colour !== 'bezel') {
    const seed = settings.colour === 'cover'
      ? (seeds[track] ??= seedFromImage(coverFor(track)))
      : (0xff000000 | parseInt(settings.colour.slice(1), 16)) >>> 0;
    tokens = monetTokens(seed, settings.tone);
  }
  for (const name of applied) device.style.removeProperty(name);
  applied = tokens ? Object.keys(tokens) : [];
  if (tokens) for (const [name, v] of Object.entries(tokens)) device.style.setProperty(name, v);
  readTokens(Boolean(panel));
  panel?.retone();
}

function showTrack(k) {
  track = k;
  const cover = coverFor(k);
  renderer?.setArt(cover);
  panel.ctx.setCover(cover);
  if (settings.colour === 'cover') applyColour();
}

// The panel's own settings and the page header are two views of the same choices.
function applySetting(key, value) {
  if (key === 'colour' || key === 'tone') {
    if (value !== 'bezel' && value !== 'dark' && value !== 'light' && !monet) return;
    settings[key] = value;
    if (key === 'tone') device.dataset.tone = value;
    select(key, value);
    applyColour();
  } else if (key === 'idle') {
    settings.idle = Number(value);
    panel.ctx.setIdle(settings.idle);
  } else if (key === 'calm') {
    settings.calm = value === 'on';
    $('calm').checked = settings.calm;
    setReducedMotion(settings.calm);
    screen.classList.toggle('calm', settings.calm);
  }
  settingsApp?.sync();
}

function loop() {
  requestAnimationFrame(loop);
  if (document.hidden || !panel) return;
  // The same clock Motion.to() reads, so a frame can never land before the retarget it follows.
  const t = performance.now() / 1000;
  const dt = lastFrame ? Math.min(t - lastFrame, 0.1) : 0;
  lastFrame = t;
  if (!settings.calm) ambient += dt;
  tickMotions(t);
  if (settings.autoStandby && panel.ctx.index !== STANDBY && !panel.ctx.appOpen && performance.now() - lastInput > 120000) {
    lastInput = performance.now();
    panel.ctx.go(STANDBY);
  }
  const state = panel.frame(t, dt);
  if (!renderer || renderer.lost) return;
  for (const upload of panel.drainUploads()) renderer.uploadLayer(upload.name, upload.source, upload.region);
  const dark = settings.tone === 'dark';
  renderer.frame({
    ...state,
    time: ambient,
    tone: dark ? 0 : 1,
    grid: dark ? 0.5 : 0.55,
    parallax: settings.calm ? 0 : 0.25,
    tokens: palette.fade.moving ? shownRgb() : palette.to,
    merge: 16,
    bend: 26,
    frost: 10,
    disp: 0.03,
    sat: 1.12,
    clear: settings.variant === 'clear' ? 1 : 0,
    solidOnly: settings.calm ? 1 : 0,
    light: { x: light.x.value, y: light.y.value, z: 1, color: dark ? [1, 0.97, 0.93] : [1, 1, 1] },
  });
}

async function boot() {
  await Promise.race([document.fonts?.ready, new Promise((resolve) => setTimeout(resolve, 2500))]);
  readTokens(false);
  setReducedMotion(settings.calm);
  $('calm').checked = settings.calm;
  screen.classList.toggle('calm', settings.calm);

  const cover = coverFor(0);
  renderer = createRenderer(canvas);
  if (renderer) {
    renderer.setArt(cover);
    screen.classList.add('gl');
  } else {
    screen.classList.add('no-gl');
    note('This browser has WebGL2 turned off, so the glass falls back to a frosted blur.');
  }
  const coverPass = renderer ? createCoverPass(renderer) : null;
  const treePass = renderer ? createTreePass(renderer) : null;
  if (renderer) { renderer.addPass(coverPass); renderer.addPass(treePass); }

  panel = createPanel(screen, settings, { cover });
  const { ctx } = panel;
  ctx.trackChanged = showTrack;
  ctx.lightDir = () => [light.x.value, light.y.value];
  wireControls(ctx);
  wirePages(ctx);
  const apps = createApps(ctx);
  createLauncher(ctx, { open: apps.open });
  createStandby(ctx);
  ctx.settingChanged = (key, value) => { if (key === 'standbyFace') ctx.showStandbyFace?.(value); };

  const flow = createCoverFlow(ctx, {
    onPlay(k) {
      ctx.playTrack(k);
      ctx.toast(`playing ${ALBUMS[k].title} · ${ALBUMS[k].artist}`, 'album');
    },
  });
  apps.register('music', flow);
  const energy = createEnergy(ctx, { pass: treePass ?? { setMesh() {} } });
  energy.build();
  apps.register('energy', energy);
  apps.register('timers', createTimers(ctx));
  apps.register('security', createSecurity(ctx));
  apps.register('notes', createNotes(ctx));
  settingsApp = createSettings(ctx, { apply: applySetting, current: (key) => (key === 'calm' ? (settings.calm ? 'on' : 'off') : settings[key]) });
  apps.register('settings', settingsApp);
  apps.register('weather', createWeather(ctx));
  apps.register('calendar', createCalendar(ctx));
  apps.register('appliances', createAppliances(ctx));
  apps.register('plants', createPlants(ctx));
  createControlCenter(ctx);
  const openApp = (name) => apps.open(name, screen.querySelector(`.app-icon[data-app="${name}"] .app-tile`));
  createActivities(ctx, { openApp });
  // a handle for inspecting the panel while developing it locally
  if (location.hostname === 'localhost') window.__bezel = { panel, ctx, apps };

  panel.measure();
  new ResizeObserver(() => panel.measure()).observe(screen);
  document.fonts?.addEventListener?.('loadingdone', () => panel.measure());
  requestAnimationFrame(loop);

  // Cover Flow's covers go into one atlas, painted small once the panel is already moving.
  const atlas = paintAtlas(ALBUMS.map((album, k) => paintCover(512, COVERS[k], album)));
  coverPass?.setAtlas(atlas.canvas);
  flow.setAtlasUv(atlas.uv);

  // The colour science loads from a CDN after the panel is up; without it, Bezel's own colours stay.
  monet = await loadMonet();
  if (!monet) {
    for (const b of $('colour').querySelectorAll('button:not([data-value="bezel"])')) b.disabled = true;
  } else if (settings.colour !== 'bezel') {
    applyColour();
  }
}

segmented('tone', 'tone', (value) => applySetting('tone', value));
segmented('variant', 'variant');
segmented('light', 'light', (value) => { if (value === 'tilt') enableTilt(); else leanLight(0, 0); });
segmented('colour', 'colour', (value) => {
  applySetting('colour', value);
  if (value === 'cover') note('Colour now comes from the album art. Play another album in Music to watch the panel follow it.');
});
$('calm').addEventListener('change', (event) => applySetting('calm', event.target.checked ? 'on' : 'off'));

boot();
