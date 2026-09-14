// Music: Cover Flow. The album in front faces you; the rest stand turned toward it, stacked to either side,
// with reflections on a dark floor. Drag tracks 1:1, a flick coasts on Detent's projection and settles on an
// album with the paging spring, and tapping a turned cover brings it round. Tapping the album in front turns it
// over: its back is a tonal card, and its track list fades in once it has turned past 90°. Tap again and it turns
// back. Play album hands it to now playing; shuffle and repeat decide what follows.

import { pressSurface } from './controls.js';
import { Motion, SPRINGS, project, rubberBand, velocityTracker } from './motion.js';
import { hex } from './panel.js';
import { ALBUMS } from './scenes.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const clock = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
const TILT = 1.2; // about 69°, the classic resting angle of a turned cover

// Invented track lists for the invented albums, as [title, seconds]. Each album's single is the track the panel plays.
const TRACKLISTS = {
  'Low Orbit': [['Launch Window', 204], ['Low Orbit', 247], ['Perigee', 231], ['Blue Hour', 258], ['Tether', 182], ['Reentry', 310]],
  'Night Ferry': [['Night Ferry', 212], ['Foghorn', 178], ['Slow Wake', 251], ['Lantern Deck', 225], ['Crossing', 280], ['Last Boat Home', 199]],
  Wires: [['Copper', 192], ['Signal Fire', 263], ['Relay', 227], ['Static Bloom', 242], ['Dead Air', 161], ['Wires', 305]],
  'Soft Machines': [['Warm Static', 231], ['Idle Hands', 208], ['Soft Machines', 254], ['Low Battery', 183], ['Screen Glow', 219], ['Sleep Mode', 286]],
  Tidewater: [['Shoreline', 216], ['Slack Tide', 198], ['Undertow', 245], ['Salt Marsh', 224], ['Neap', 172], ['Tidewater', 271]],
  'Paper Lanterns': [['Lantern Walk', 244], ['Festival Rain', 202], ['Paper Boats', 228], ['Moth Song', 179], ['Riverside', 237], ['Embers', 265]],
  'Field Recordings': [['Dawn Chorus', 190], ['Morning, Meadow', 276], ['Hedgerow', 221], ['Bees in the Clover', 242], ['Long Grass', 213], ['Evening Field', 312]],
  Afterglow: [['Golden Hour', 209], ['Afterglow', 219], ['Neon Dusk', 248], ['Pink Noise', 195], ['Late Train', 232], ['Streetlights', 260]],
};

export function createCoverFlow(ctx, { onPlay }) {
  const { screen, settings } = ctx;
  const q = (selector) => screen.querySelector(selector);
  const el = q('#coverflow');
  const titleEl = q('#album-title');
  const byEl = q('#album-by');
  const backEl = q('#album-tracks');
  const backHead = q('#album-tracks-head');
  const backList = q('#album-tracks-list');
  const toggles = [['shuffle', q('#album-shuffle')], ['repeat', q('#album-repeat')]].filter(([, b]) => b);
  const count = ALBUMS.length;
  const position = new Motion(0, { spring: SPRINGS.page, epsilon: 0.0005 });
  const flip = new Motion(0, { spring: SPRINGS.release, epsilon: 0.001 }); // 0 facing you, 1 turned over
  const press = new Motion(0, { spring: SPRINGS.hold, epsilon: 0.001 });
  const tracker = velocityTracker();
  let rect = null;
  let shown = -1;
  let from = 0;
  let uvs = null;
  let card = null;
  let turning = 0; // the album the flip belongs to
  let listed = -1;
  let marked = '';
  let pressed = -1;
  settings.shuffle ??= false;
  settings.repeat ??= false;

  // Measured with the window's transform set aside: the layer still carries the last close's scale on reopen.
  const measure = () => {
    const layer = el.closest('.app-layer');
    const saved = layer ? layer.style.transform : '';
    if (layer) layer.style.transform = 'none';
    const s = screen.getBoundingClientRect();
    const r = el.getBoundingClientRect();
    if (layer) layer.style.transform = saved;
    rect = { x: r.left - s.left, y: r.top - s.top, w: r.width, h: r.height };
  };
  const readCard = () => {
    const style = getComputedStyle(screen.closest('.device') ?? screen);
    card = hex(style.getPropertyValue(settings.tone === 'light' ? '--d-s1' : '--d-s2'));
  };
  const settle = (target, velocity) => position.to(clamp(Math.round(target), 0, count - 1), { spring: SPRINGS.page, velocity });

  function describe(k) {
    const album = ALBUMS[k];
    titleEl.textContent = album.title;
    byEl.textContent = `${album.artist} · ${album.year}`;
    el.setAttribute('aria-activedescendant', '');
    el.setAttribute('aria-label', `Albums, ${album.title} by ${album.artist}`);
    ctx.dirty(titleEl);
    ctx.dirty(byEl);
  }

  // The back of the sleeve: live DOM over the card the cover pass draws, so it can fade and turn with it.
  function list(k) {
    listed = k;
    marked = '';
    if (!backList) return;
    const album = ALBUMS[k];
    const tracks = TRACKLISTS[album.title] ?? [[album.single, album.length]];
    if (backHead) backHead.textContent = `${tracks.length} tracks · ${Math.round(tracks.reduce((sum, [, s]) => sum + s, 0) / 60)} min`;
    backList.replaceChildren(...tracks.map(([title, seconds], i) => {
      const row = document.createElement('li');
      if (title === album.single) row.className = 'is-single';
      const n = document.createElement('span');
      n.className = 'track-n mono';
      n.textContent = String(i + 1);
      const name = document.createElement('span');
      name.className = 'track-title';
      name.textContent = title;
      const length = document.createElement('span');
      length.className = 'track-len mono num';
      length.textContent = clock(seconds);
      row.append(n, name, length);
      return row;
    }));
  }
  // The single's number becomes a note while its album is the one playing.
  function mark() {
    const np = ctx.nowPlaying?.();
    const playing = Boolean(np && np.index === listed && np.playing);
    const key = `${listed}:${playing}`;
    if (!backList || key === marked) return;
    marked = key;
    const rows = [...backList.children];
    const i = rows.findIndex((row) => row.classList.contains('is-single'));
    const n = rows[i]?.querySelector('.track-n');
    if (!n) return;
    n.textContent = playing ? 'music_note' : String(i + 1);
    n.classList.toggle('icon', playing);
    n.classList.toggle('mono', !playing);
  }

  /** Turns album k over, or back. Every call retargets the same motion, so a tap mid-turn reverses it. */
  function turn(k, over) {
    if (over && turning !== k) {
      flip.set(0);
      turning = k;
    }
    if (over && listed !== k) list(k);
    flip.to(over ? 1 : 0, { spring: SPRINGS.release });
  }

  ctx.addDragTarget(el, {
    begin() {
      from = position.value;
      tracker.reset();
      press.to(0, { spring: SPRINGS.release });
      if (flip.target > 0) turn(turning, false);
    },
    move(dx, t) {
      const u = ctx.unit;
      const raw = from - dx / (150 * u);
      const last = count - 1;
      const value = raw < 0 ? -rubberBand(-raw, 1) : raw > last ? last + rubberBand(raw - last, 1) : raw;
      tracker.add(t, value);
      position.set(value, tracker.velocity(t));
    },
    end(t) {
      const v = tracker.velocity(t);
      settle(position.value + project(v, 0.994), v);
    },
  }, 'x');

  const offsetOf = (e) => (ctx.local(e)[0] - (rect.x + rect.w / 2)) / ctx.unit;
  // The album in front dips under the finger on pointer-down; the turn itself waits for the tap.
  el.addEventListener('pointerdown', (e) => {
    if (e.button > 0 || !rect || Math.abs(offsetOf(e)) >= 125) return;
    pressed = Math.round(position.target);
    press.to(1, { spring: SPRINGS.hold });
  });
  for (const type of ['pointerup', 'pointercancel', 'pointerleave']) el.addEventListener(type, () => press.to(0, { spring: SPRINGS.release }));

  // Tapping a turned cover brings it to the front; tapping the front one turns it over, or back.
  el.addEventListener('click', (e) => {
    if (!rect) return;
    const dx = offsetOf(e);
    const current = Math.round(position.target);
    if (Math.abs(dx) < 125) { turn(current, !(flip.target > 0.5 && turning === current)); return; }
    if (flip.target > 0) turn(turning, false);
    const steps = 1 + Math.max(0, Math.floor((Math.abs(dx) - 190) / 58));
    settle(current + Math.sign(dx) * steps, 0);
  });
  // Keyboard changes land at once.
  el.addEventListener('keydown', (e) => {
    const step = { ArrowRight: 1, ArrowLeft: -1 }[e.key];
    const current = Math.round(position.target);
    if (step) { e.preventDefault(); flip.set(0); position.set(clamp(current + step, 0, count - 1)); }
    if (e.key === 'Enter') onPlay(current);
    if (e.key === ' ') {
      e.preventDefault();
      const over = !(flip.target > 0.5 && turning === current);
      turning = current;
      if (over && listed !== current) list(current);
      flip.set(over ? 1 : 0);
    }
  });
  q('#album-play').addEventListener('click', () => onPlay(Math.round(position.target)));

  // ---------- shuffle and repeat: tonal toggles that fill and morph when on ----------
  function showToggle(b, on) {
    if (b.getAttribute('aria-pressed') === String(on)) return;
    b.setAttribute('aria-pressed', String(on));
    ctx.recolor(b);
    ctx.reshape(b);
    ctx.dirty(b);
  }
  for (const [key, b] of toggles) {
    pressSurface(ctx, b, 0.7);
    b.addEventListener('click', () => { settings[key] = !settings[key]; showToggle(b, settings[key]); });
  }
  ctx.music = {
    get shuffle() { return Boolean(settings.shuffle); },
    get repeat() { return Boolean(settings.repeat); },
    /** The track to play when track i ends: i again on repeat, another at random on shuffle, else the next. */
    next(i, total = count) {
      if (settings.repeat) return i;
      if (settings.shuffle && total > 1) return (i + 1 + Math.floor(Math.random() * (total - 1))) % total;
      return (i + 1) % total;
    },
  };

  ctx.onMeasure({ after() { if (rect) measure(); } });
  ctx.onRetone(readCard);

  return {
    setAtlasUv(uv) { uvs = ALBUMS.map((_, k) => uv(k)); },
    showAlbum(k) { flip.set(0); position.set(clamp(k, 0, count - 1)); },
    open() {
      measure();
      readCard();
      shown = -1;
      for (const [key, b] of toggles) showToggle(b, Boolean(settings[key]));
    },
    close() {
      press.set(0);
      if (flip.target > 0) turn(turning, false);
    },
    frame(t, dt, { win }) {
      if (!rect || !uvs) return {};
      const k = Math.round(clamp(position.value, 0, count - 1));
      if (k !== shown) { shown = k; describe(k); }
      const calm = settings.calm;
      const u = ctx.unit;
      const W = ctx.W;
      const H = ctx.H;
      const F = 760 * u; // focal length: how strongly the turned covers recede
      const s = 125 * u;
      const cx = rect.x + rect.w / 2;
      const cy = rect.y + rect.h * 0.4;
      const floor = cy + s;
      const turned = flip.value;
      const out = [];
      const vertex = (X, Y, Z, tu, tv, amount, back) => {
        const w = (F - Z) / F;
        const px = win.x + (cx + X / w) * win.sx;
        const py = win.y + (cy + Y / w) * win.sy;
        out.push(((px / W) * 2 - 1) * w, (1 - (py / H) * 2) * w, 0, w, tu, tv, amount, back);
      };
      const place = (i) => {
        const d = i - position.value;
        const ad = Math.abs(d);
        const near = clamp(ad, 0, 1);
        return {
          ad,
          tilt: Math.sign(d) * near * TILT * (calm ? 0.6 : 1),
          X: ad < 1 ? d * 190 * u : Math.sign(d) * (190 + (ad - 1) * 58) * u,
          Z: -near * 150 * u,
          shade: (1 - 0.3 * near) * clamp(5.5 - ad, 0, 1),
          // a pressed sleeve sinks toward the floor, so its reflection stays joined to it
          k: i === pressed && !calm ? 1 - 0.035 * press.value : 1,
        };
      };
      const order = ALBUMS.map((_, i) => i).sort((a, b) => Math.abs(b - position.value) - Math.abs(a - position.value));
      for (const i of order) {
        const { ad, tilt, X, Z, shade, k: sink } = place(i);
        if (ad > 5.5) continue;
        const flipping = i === turning && turned !== 0;
        // the turn is a real rotation about the sleeve's upright axis; the calm version cross-fades instead
        const angle = tilt + (flipping && !calm ? turned * Math.PI : 0);
        const cos = Math.cos(angle);
        const sin = Math.sin(angle);
        const back = flipping ? (calm ? clamp(turned, 0, 1) : cos < 0 ? 1 : 0) : 0;
        const [u0, v0, u1, v1] = uvs[i];
        const corner = (lx, ly, tu, tv, amount, mirror) => {
          const x = X + lx * sink * cos;
          const z = Z - lx * sink * sin;
          const y = s - (s - ly) * sink;
          vertex(x, mirror ? 2 * s - y : y, z, tu, tv, amount, back);
        };
        // the cover: top-left, top-right, bottom-right, bottom-left
        const quad = (mirror) => {
          const top = mirror ? shade * 0.3 : shade;
          const bottom = mirror ? 0 : shade;
          const tl = [-s, -s, u0, v0, top], tr = [s, -s, u1, v0, top], br = [s, s, u1, v1, bottom], bl = [-s, s, u0, v1, bottom];
          if (mirror) { tl[4] = 0; tr[4] = 0; br[4] = shade * 0.3; bl[4] = shade * 0.3; }
          for (const c of [tl, tr, br, tl, br, bl]) corner(c[0], c[1], c[2], c[3], c[4], mirror);
        };
        quad(true);
        quad(false);
      }

      // The track list appears once the card has turned past 90°, turned and placed exactly as the card is.
      if (backEl) {
        const reveal = clamp((turned - 0.5) / (calm ? 0.35 : 0.25), 0, 1);
        if (reveal > 0.001) {
          mark();
          const { tilt, X, Z, k: sink } = place(turning);
          const psi = tilt + turned * Math.PI - Math.PI; // the back's own turn: 0 when it faces you
          backEl.style.opacity = reveal.toFixed(3);
          backEl.style.transform = calm ? ''
            : `perspective(${F.toFixed(1)}px) translate3d(${X.toFixed(2)}px, 0px, ${Z.toFixed(2)}px) rotateY(${psi.toFixed(4)}rad) translateY(${(s * (1 - sink)).toFixed(2)}px) scale(${sink.toFixed(4)})`;
          backEl.setAttribute('aria-hidden', 'false');
        } else if (backEl.style.opacity !== '0') {
          backEl.style.opacity = '0';
          backEl.setAttribute('aria-hidden', 'true');
        }
      }

      const clip = { x: win.x + rect.x * win.sx, y: win.y + rect.y * win.sy, w: rect.w * win.sx, h: rect.h * win.sy };
      return { covers: { vertices: new Float32Array(out), clip, floor, card } };
    },
  };
}
