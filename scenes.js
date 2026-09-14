// Procedural content for the specimen: an album library with original covers. Every artist and album here
// is invented. Covers are painted once, at 1024 px for now playing and into a shared atlas for Cover Flow.

/** A small seeded PRNG so each cover looks the same on every load. */
function random(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const sky = (seed, colours, sun, ridges, stars) => ({ style: 'sky', seed, sky: colours, sun, ridges, stars });

export const ALBUMS = [
  { title: 'Low Orbit', artist: 'harbor lights', year: 2026, single: 'Low Orbit', length: 247,
    look: sky(1226, ['#0b1633', '#223a86', '#3a2f6e', '#090c1d'], [0.36, 0.63, 0.19, '#ff8a4a', '#d92c1c', '255, 96, 48'], ['rgba(20, 26, 70, 0.9)', 'rgba(10, 14, 40, 0.95)', '#05070f'], '222, 232, 255') },
  { title: 'Night Ferry', artist: 'harbor lights', year: 2026, single: 'Night Ferry', length: 212,
    look: sky(1227, ['#03161a', '#0b4b53', '#1d7169', '#021013'], [0.68, 0.33, 0.1, '#f2fff8', '#9fe8cf', '150, 240, 214'], ['rgba(5, 52, 56, 0.9)', 'rgba(3, 32, 36, 0.95)', '#020c0e'], '214, 255, 244') },
  { title: 'Wires', artist: 'the quiet hours', year: 2025, single: 'Signal Fire', length: 263,
    look: sky(1228, ['#1a0614', '#5e0f3b', '#b4291f', '#12040c'], [0.5, 0.71, 0.24, '#ffd56e', '#ff5a3c', '255, 128, 84'], ['rgba(62, 8, 40, 0.9)', 'rgba(36, 4, 24, 0.95)', '#0c0208'], '255, 222, 234') },
  { title: 'Soft Machines', artist: 'mira vale', year: 2024, single: 'Warm Static', length: 231,
    look: { style: 'orb', seed: 31, ground: ['#f3c7b4', '#7d5fd1'], orb: ['#fff1e0', '#ff7b54'], rings: 'rgba(40, 20, 70, 0.28)' } },
  { title: 'Tidewater', artist: 'north coast radio', year: 2023, single: 'Slack Tide', length: 198,
    look: { style: 'bands', seed: 44, bands: ['#0f3a5f', '#1f7a8c', '#bfdbd4', '#e9d8a6', '#ee9b00'] } },
  { title: 'Paper Lanterns', artist: 'kiko & the ferns', year: 2025, single: 'Lantern Walk', length: 244,
    look: { style: 'orb', seed: 52, ground: ['#2b0a0a', '#8c1c13'], orb: ['#ffe8a3', '#f2a541'], rings: 'rgba(255, 220, 150, 0.22)' } },
  { title: 'Field Recordings', artist: 'ondine', year: 2022, single: 'Morning, Meadow', length: 276,
    look: { style: 'bands', seed: 67, bands: ['#1b3b2f', '#2d6a4f', '#52b788', '#b7e4c7', '#f1faee'] } },
  { title: 'Afterglow', artist: 'lumen park', year: 2026, single: 'Afterglow', length: 219,
    look: sky(1231, ['#140a2e', '#4b2a8a', '#c2548a', '#1a0c24'], [0.62, 0.66, 0.16, '#ffe1f0', '#ff7aa8', '255, 140, 190'], ['rgba(40, 16, 72, 0.9)', 'rgba(24, 8, 44, 0.95)', '#0a0412'], '255, 230, 250') },
];
export const COVERS = ALBUMS.map((album) => album.look);

function paintSky(g, size, look, rand) {
  const grad = g.createLinearGradient(0, 0, 0, size);
  look.sky.forEach((colour, k) => grad.addColorStop([0, 0.5, 0.78, 1][k], colour));
  g.fillStyle = grad;
  g.fillRect(0, 0, size, size);
  for (let i = 0; i < 700; i++) {
    g.fillStyle = `rgba(${look.stars}, ${(rand() * 0.55).toFixed(3)})`;
    const s = rand() < 0.08 ? 2.2 : 1.1;
    g.fillRect(rand() * size, rand() * size * 0.62, s, s);
  }
  const [ux, uy, ur, top, bottom, glow] = look.sun;
  const sx = size * ux, sy = size * uy, sr = size * ur;
  const halo = g.createRadialGradient(sx, sy, sr * 0.4, sx, sy, sr * 2.4);
  halo.addColorStop(0, `rgba(${glow}, 0.45)`);
  halo.addColorStop(1, `rgba(${glow}, 0)`);
  g.fillStyle = halo;
  g.fillRect(0, 0, size, size);
  const sun = g.createLinearGradient(0, sy - sr, 0, sy + sr);
  sun.addColorStop(0, top);
  sun.addColorStop(1, bottom);
  g.fillStyle = sun;
  g.beginPath();
  g.arc(sx, sy, sr, 0, Math.PI * 2);
  g.fill();
  const ridge = (base, amp, freq, colour, phase) => {
    g.fillStyle = colour;
    g.beginPath();
    g.moveTo(0, size);
    for (let x = 0; x <= size; x += 4) {
      const u = x / size;
      g.lineTo(x, size * (base + amp * Math.sin(u * freq + phase) + amp * 0.4 * Math.sin(u * freq * 3.3 + phase * 1.7)));
    }
    g.lineTo(size, size);
    g.closePath();
    g.fill();
  };
  const phase = look.seed % 7;
  ridge(0.68, 0.03, 5.2, look.ridges[0], 1.2 + phase);
  ridge(0.77, 0.028, 7.6, look.ridges[1], 3.8 + phase);
  ridge(0.87, 0.022, 12.4, look.ridges[2], 2.4 + phase);
}

function paintOrb(g, size, look) {
  const grad = g.createLinearGradient(0, 0, size, size);
  grad.addColorStop(0, look.ground[0]);
  grad.addColorStop(1, look.ground[1]);
  g.fillStyle = grad;
  g.fillRect(0, 0, size, size);
  const cx = size * 0.5, cy = size * 0.58, r = size * 0.28;
  g.strokeStyle = look.rings;
  g.lineWidth = size * 0.004;
  for (let i = 1; i <= 7; i++) {
    g.beginPath();
    g.arc(cx, cy, r + i * size * 0.035, 0, Math.PI * 2);
    g.stroke();
  }
  const orb = g.createRadialGradient(cx - r * 0.35, cy - r * 0.4, r * 0.1, cx, cy, r);
  orb.addColorStop(0, look.orb[0]);
  orb.addColorStop(1, look.orb[1]);
  g.fillStyle = orb;
  g.beginPath();
  g.arc(cx, cy, r, 0, Math.PI * 2);
  g.fill();
}

function paintBands(g, size, look, rand) {
  const n = look.bands.length;
  g.save();
  g.translate(size / 2, size / 2);
  g.rotate(-0.42);
  const span = size * 1.6;
  for (let i = 0; i < n * 2; i++) {
    g.fillStyle = look.bands[i % n];
    const h = span / (n * 2);
    g.fillRect(-span / 2, -span / 2 + i * h, span, h + 1 + rand() * 2);
  }
  g.restore();
}

/** An album cover. Painted after fonts load so its type is right. */
export function paintCover(size = 1024, look = COVERS[0], album = ALBUMS[COVERS.indexOf(look)] ?? ALBUMS[0]) {
  const c = document.createElement('canvas');
  c.width = c.height = size;
  const g = c.getContext('2d', { willReadFrequently: true });
  const rand = random(look.seed);
  if (look.style === 'orb') paintOrb(g, size, look);
  else if (look.style === 'bands') paintBands(g, size, look, rand);
  else paintSky(g, size, look, rand);

  // Film grain keeps the gradients from banding once the glass magnifies them.
  const grain = g.getImageData(0, 0, size, size);
  for (let i = 0; i < grain.data.length; i += 4) {
    const n = (rand() - 0.5) * 14;
    grain.data[i] += n;
    grain.data[i + 1] += n;
    grain.data[i + 2] += n;
  }
  g.putImageData(grain, 0, 0);

  // Centred high on the sleeve, so the title survives the round crop of the disc.
  const light = look.style !== 'sky' && look.ground?.[0]?.startsWith('#f');
  g.textAlign = 'center';
  g.fillStyle = light ? 'rgba(30, 20, 40, 0.9)' : 'rgba(238, 240, 242, 0.94)';
  g.font = `500 ${Math.round(size * 0.044)}px "Google Sans Code", ui-monospace, monospace`;
  g.fillText(album.artist, size * 0.5, size * 0.21);
  g.fillStyle = light ? 'rgba(30, 20, 40, 0.6)' : 'rgba(238, 240, 242, 0.58)';
  g.font = `400 ${Math.round(size * 0.03)}px "Google Sans Code", ui-monospace, monospace`;
  g.fillText(`${album.title.toLowerCase()} · ${album.year}`, size * 0.5, size * 0.26);
  return c;
}

/** Every cover in one texture for Cover Flow: four across, two down. */
export function paintAtlas(covers, cell = 512) {
  const c = document.createElement('canvas');
  c.width = cell * 4;
  c.height = cell * 2;
  const g = c.getContext('2d');
  covers.forEach((cover, k) => g.drawImage(cover, (k % 4) * cell, Math.floor(k / 4) * cell, cell, cell));
  return { canvas: c, uv: (k) => [(k % 4) / 4, Math.floor(k / 4) / 2, ((k % 4) + 1) / 4, (Math.floor(k / 4) + 1) / 2] };
}

/** A downscaled copy for small tiles, as a data URL. */
export function thumbnail(source, size = 160) {
  const c = document.createElement('canvas');
  c.width = c.height = size;
  c.getContext('2d', { willReadFrequently: true }).drawImage(source, 0, 0, size, size);
  return c.toDataURL('image/jpeg', 0.86);
}
