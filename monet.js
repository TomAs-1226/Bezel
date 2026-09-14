// Dynamic colour the way Android's Monet does it: pick a seed colour from content, build tonal palettes in
// HCT (hue, chroma, tone), and take every role from a fixed tone of those palettes, so contrast holds for any
// seed. Bezel keeps its own structure on top: near-neutral surfaces, one vivid signal, an ice fill for "on"
// and amber for lights. Google's material-color-utilities does the colour science; if it can't load, Bezel's
// own tokens stay in place.

const LIBRARY = 'https://cdn.jsdelivr.net/npm/@material/material-color-utilities@0.4.0/+esm';
let lib = null;

export async function loadMonet() {
  try { lib = await import(LIBRARY); } catch { lib = null; }
  return lib !== null;
}

/** The seed Android would pick from a wallpaper: quantize to 128 colours, then score for one that is common and vivid. */
export function seedFromImage(source, fallback = 0xffff5b1f) {
  const size = 112;
  const c = document.createElement('canvas');
  c.width = c.height = size;
  const g = c.getContext('2d', { willReadFrequently: true });
  g.drawImage(source, 0, 0, size, size);
  const data = g.getImageData(0, 0, size, size).data;
  const pixels = [];
  for (let i = 0; i < data.length; i += 4) pixels.push(lib.argbFromRgb(data[i], data[i + 1], data[i + 2]));
  return lib.Score.score(lib.QuantizerCelebi.quantize(pixels, 128), { desired: 1, fallbackColorARGB: fallback })[0];
}

export const hexOf = (argb) => lib.hexFromArgb(argb);

/** Bezel's panel tokens for a seed, as CSS custom properties. Tones are M3's; the chroma choices are Bezel's. */
export function monetTokens(seed, tone) {
  const { Blend, Hct, SchemeTonalSpot, TemperatureCache, hexFromArgb } = lib;
  const dark = tone === 'dark';
  const pick = (d, l) => (dark ? d : l);
  const source = Hct.fromInt(seed);
  const scheme = new SchemeTonalSpot(source, dark, 0);
  const n = (t) => hexFromArgb(scheme.neutralPalette.tone(t));
  const nv = (t) => hexFromArgb(scheme.neutralVariantPalette.tone(t));
  const at = (hue, chroma, t) => hexFromArgb(Hct.from(hue, chroma, t).toInt());
  // The signal keeps the seed's own vividness, as the wall panels' one orange does; M3's tonal spot would mute it.
  const hue = source.hue;
  const chroma = Math.max(source.chroma, 56);
  // Ice is the seed's colour-temperature complement at low chroma: an orange seed gives Bezel's cool blue.
  const ice = new TemperatureCache(source).complement.hue;
  // Amber means a light is on, so it stays amber, only turned a little toward the seed (M3 harmonization).
  const amber = Hct.fromInt(Blend.harmonize(dark ? 0xfff4c542 : 0xffffd24a, seed));
  const leaf = Hct.fromInt(Blend.harmonize(0xff5fae4a, seed));
  return {
    '--d-ground': n(pick(4, 92)), '--d-s1': n(pick(10, 99)), '--d-s2': n(pick(13, 95)), '--d-s3': n(pick(19, 90)),
    '--d-track': n(pick(13, 99)), '--d-meter': n(pick(15, 91)),
    '--d-ink': n(pick(94, 10)), '--d-dim': nv(pick(70, 40)), '--d-faint': nv(pick(42, 70)),
    '--d-signal': at(hue, chroma, pick(62, 56)), '--d-on-signal': at(hue, 16, pick(8, 100)),
    '--d-ice': at(ice, 16, 88), '--d-on-ice': at(ice, 24, 14),
    '--d-amber': hexFromArgb(amber.toInt()), '--d-on-amber': at(amber.hue, 30, 14),
    '--d-amber-container': at(amber.hue, 24, pick(22, 90)), '--d-amber-ink': pick(hexFromArgb(amber.toInt()), at(amber.hue, 60, 45)),
    // Leaf green marks energy saved; like amber it keeps its meaning and only leans toward the seed.
    '--d-leaf': at(leaf.hue, 44, pick(80, 40)), '--d-leaf-container': at(leaf.hue, 24, pick(20, 90)),
    '--case-a': pick(at(hue, chroma, 64), n(98)), '--case-b': pick(at(hue, chroma, 52), n(86)),
  };
}
