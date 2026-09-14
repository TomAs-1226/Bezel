// GLSL for Bezel's renderer: the shared quad, the orbit sphere, the blur pyramid and the glass pass. The scene
// pass is in scene.glsl.js.

export const VS = `#version 300 es
in vec2 p; out vec2 v;
void main() { v = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }`;

const SDF = `
float sdRound(vec2 p, vec4 r, float rad) {
  vec2 h = r.zw * 0.5;
  vec2 q = abs(p - r.xy - h) - h + rad;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
}`;

// The orbit scape's particle sphere: additive light on dark, ink points on light.
export const POINTS_VS = `#version 300 es
in vec3 a;
uniform vec2 uRes; uniform float uDpr, uTime, uSize; uniform vec3 uCenter;
out float vDepth; out float vHue;
void main() {
  float t = uTime * 0.2, tilt = 0.38;
  vec3 q = vec3(a.x * cos(t) + a.z * sin(t), a.y, -a.x * sin(t) + a.z * cos(t));
  q = vec3(q.x, q.y * cos(tilt) - q.z * sin(tilt), q.y * sin(tilt) + q.z * cos(tilt));
  vec2 px = uCenter.xy + q.xy * uCenter.z * (1.0 + 0.03 * sin(uTime * 0.9 + a.y * 5.0));
  vec2 clip = px / uRes * 2.0 - 1.0;
  gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);
  vDepth = q.z * 0.5 + 0.5;
  vHue = clamp(a.y * 0.5 + 0.5, 0.0, 1.0);
  gl_PointSize = uSize * uDpr * (0.5 + 0.8 * vDepth);
}`;

export const POINTS_FS = `#version 300 es
precision highp float;
in float vDepth; in float vHue; out vec4 o;
uniform vec3 uColA, uColB; uniform float uAlpha, uTone;
void main() {
  float m = 1.0 - smoothstep(0.32, 0.5, length(gl_PointCoord - 0.5));
  vec3 c = mix(uColA, uColB, vHue);
  float a = m * (0.2 + 0.8 * vDepth) * uAlpha;
  o = uTone < 0.5 ? vec4(c * a, 1.0) : vec4(c * 0.8, a * 0.9);
}`;

// One level of the blur pyramid: half the size of the level before, low-passed by 13 taps over a 4 × 4 texel
// footprint (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014), so
// content moving underneath never shimmers through the blur.
export const DOWN_FS = `#version 300 es
precision highp float;
in vec2 v; out vec4 o;
uniform sampler2D uSrc; uniform vec2 uTexel;
vec3 at(vec2 d) { return texture(uSrc, v + d * uTexel).rgb; }
void main() {
  vec3 c = at(vec2(0.0)) * 0.125
    + (at(vec2(-1.0, -1.0)) + at(vec2(1.0, -1.0)) + at(vec2(-1.0, 1.0)) + at(vec2(1.0, 1.0))) * 0.125
    + (at(vec2(-2.0, 0.0)) + at(vec2(2.0, 0.0)) + at(vec2(0.0, -2.0)) + at(vec2(0.0, 2.0))) * 0.0625
    + (at(vec2(-2.0, -2.0)) + at(vec2(2.0, -2.0)) + at(vec2(-2.0, 2.0)) + at(vec2(2.0, 2.0))) * 0.03125;
  o = vec4(c, 1.0);
}`;

// Pass B: glass. A slab with a squircle bezel: the flat top passes the scene through, the curved rim
// refracts it (Snell, n = 1.5). Regular glass blurs and evens out the luminosity behind it; light catches
// the cut edge as a thin line. Shapes melt together only inside their own group, like a GlassEffectContainer.
// With the control center open, everything behind it blurs, dims and loses some colour, and its glass sits on that.
export const GLASS_FS = `#version 300 es
precision highp float;
in vec2 v; out vec4 o;
uniform sampler2D uScene, uB1, uB2, uB3, uB4, uB5, uB6;
uniform vec2 uRes; uniform float uDpr, uTone;
uniform vec4 uGlass[12]; uniform float uGlassR[12]; uniform vec4 uGlassK[12]; uniform vec4 uGlassP[12]; uniform float uGlassG[12]; uniform int uGlassN;
uniform float uMerge, uBend, uFrost, uDisp, uSat, uClear, uSolidOnly;
uniform vec3 uLight, uLightColor, uSolid;
uniform vec3 uTints[4];
uniform vec4 uLens; uniform float uLensR, uLensAmt;
uniform float uBackLod, uDim;
uniform vec4 uOver[6]; uniform vec4 uOverC[6]; uniform float uOverR[6]; uniform int uOverN;
uniform vec4 uPing; uniform float uPingR; uniform vec3 uPingK;
${SDF}
const vec3 LUM = vec3(0.2126, 0.7152, 0.0722);
float smin(float a, float b, float k) {
  float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}
float dist(vec2 p) {
  float g0 = 1e5, g1 = 1e5, g2 = 1e5, g3 = 1e5, g4 = 1e5, g5 = 1e5, g6 = 1e5, g7 = 1e5;
  for (int i = 0; i < 12; i++) {
    if (i >= uGlassN) break;
    float d = sdRound(p, uGlass[i], uGlassR[i]);
    int g = int(uGlassG[i] + 0.5);
    if (g == 0) g0 = smin(g0, d, uMerge);
    else if (g == 1) g1 = smin(g1, d, uMerge);
    else if (g == 2) g2 = smin(g2, d, uMerge);
    else if (g == 3) g3 = smin(g3, d, uMerge);
    else if (g == 4) g4 = smin(g4, d, uMerge);
    else if (g == 5) g5 = smin(g5, d, uMerge);
    else if (g == 6) g6 = smin(g6, d, uMerge);
    else g7 = smin(g7, d, uMerge);
  }
  return min(min(min(g0, g1), min(g2, g3)), min(min(g4, g5), min(g6, g7)));
}

// A pyramid level read back through a cubic B-spline in four bilinear taps, so a small level magnifies
// without blocks or diamonds.
vec3 bicubic(sampler2D t, vec2 uv) {
  vec2 size = vec2(textureSize(t, 0));
  vec2 st = uv * size - 0.5;
  vec2 f = fract(st);
  st -= f;
  vec2 f2 = f * f, f3 = f2 * f;
  vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
  vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
  vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
  vec2 w3 = f3 / 6.0;
  vec2 g0 = w0 + w1, g1 = w2 + w3;
  vec2 h0 = (st - 0.5 + w1 / g0) / size;
  vec2 h1 = (st + 1.5 + w3 / g1) / size;
  return g0.y * (g0.x * texture(t, h0).rgb + g1.x * texture(t, vec2(h1.x, h0.y)).rgb)
       + g1.y * (g0.x * texture(t, vec2(h0.x, h1.y)).rgb + g1.x * texture(t, h1).rgb);
}
vec3 level(int i, vec2 uv) {
  if (i <= 0) return texture(uScene, uv).rgb;
  if (i == 1) return bicubic(uB1, uv);
  if (i == 2) return bicubic(uB2, uv);
  if (i == 3) return bicubic(uB3, uv);
  if (i == 4) return bicubic(uB4, uv);
  if (i == 5) return bicubic(uB5, uv);
  return bicubic(uB6, uv);
}
// Blur of any radius: level n holds a Gaussian of about 0.83 × 2^n pixels, and neighbouring levels blend.
vec3 blurred(vec2 p, float lod) {
  vec2 uv = vec2(p.x, uRes.y - p.y) / uRes;
  lod = clamp(lod, 0.0, 6.0);
  float n = floor(lod);
  float f = lod - n;
  vec3 a = level(int(n), uv);
  return f < 0.02 ? a : mix(a, level(int(n) + 1, uv), f);
}
// What any glass sees behind it, and what the page shows between the glass.
vec3 back(vec2 p, float lod) {
  vec3 c = blurred(p, max(lod, uBackLod));
  if (uDim > 0.0) {
    c *= 1.0 - uDim * mix(0.52, 0.3, uTone);
    c = mix(c, vec3(dot(c, LUM)), uDim * 0.35);
  }
  return c;
}
float profile(float x) { return pow(1.0 - pow(1.0 - clamp(x, 0.0, 1.0), 4.0), 0.25); }
void main() {
  vec2 px = vec2(v.x, 1.0 - v.y) * uRes;
  vec3 col = back(px, 0.0);
  float d = dist(px);

  int k = 0; float dn = 1e5;
  for (int i = 0; i < 12; i++) {
    if (i >= uGlassN) break;
    float di = sdRound(px, uGlass[i], uGlassR[i]);
    if (di < dn) { dn = di; k = i; }
  }
  float strength = uGlassN > 0 ? uGlassK[k].x : 0.0;

  // adaptive shadow: deeper under bigger glass and over busy content, faint over a plain ground
  float big = uGlassN > 0 ? clamp(min(uGlass[k].z, uGlass[k].w) / (90.0 * uDpr), 0.5, 1.4) : 1.0;
  vec2 uv = vec2(px.x, uRes.y - px.y) / uRes;
  float busy = clamp(length(texture(uB2, uv).rgb - texture(uB5, uv).rgb) * 5.0, 0.0, 1.0);
  float ds = dist(px - vec2(0.0, 5.0 * uDpr * big));
  col *= 1.0 - (1.0 - smoothstep(-4.0 * uDpr, 24.0 * uDpr * big, ds)) * step(0.0, d) * strength * mix(0.08, 0.2, busy) * big;

  if (d < uDpr && uGlassN > 0) {
    float press = uGlassK[k].y, tint = uGlassK[k].z;
    vec3 tintColor = uTints[int(clamp(uGlassP[k].z, 0.0, 3.0) + 0.5)];
    float e = max(-d, 0.0);
    float bezel = clamp(uGlassR[k] * 0.9, 8.0 * uDpr, 28.0 * uDpr);
    float x = e / bezel;
    float slope = min((profile(x + 0.015) - profile(x)) / 0.015, 12.0);
    vec2 n = normalize(vec2(dist(px + vec2(uDpr, 0.0)) - dist(px - vec2(uDpr, 0.0)),
                            dist(px + vec2(0.0, uDpr)) - dist(px - vec2(0.0, uDpr))) + 1e-6);
    vec3 N = normalize(vec3(n * slope, 1.0));
    vec3 T = refract(vec3(0.0, 0.0, -1.0), N, 1.0 / 1.5);
    float thick = uBend * uDpr * (0.3 + 0.7 * profile(x)) * (1.0 + 0.4 * press) * strength;
    vec2 off = T.xy / max(-T.z, 0.3) * thick;
    // frost is a Gaussian of uFrost × 0.8 points: regular glass is properly frosted, clear glass barely
    float lod = log2(max(uFrost * 0.8 * uDpr * strength * mix(1.0, 0.12, uClear) / 0.83, 1.0));
    // colour only splits where the bend is strong enough to show it; the flat top takes one sample
    vec3 g = length(off) * uDisp < 0.35 * uDpr
      ? back(px + off, lod)
      : vec3(back(px + off * (1.0 + uDisp), lod).r, back(px + off, lod).g, back(px + off * (1.0 - uDisp), lod).b);

    // the dock's droplet magnifies while it is lifted; at rest the selection is a faint platter
    vec2 lc = uLens.xy + uLens.zw * 0.5;
    float inLens = 1.0 - smoothstep(-uDpr, 0.5 * uDpr, sdRound(px, uLens, uLensR));
    float lifted = clamp(uLensAmt, 0.0, 1.0);
    if (inLens * lifted > 0.001) g = mix(g, back(lc + (px - lc) * (1.0 - 0.2 * lifted) + off, lod * 0.5), inLens * lifted);
    g = mix(g, mix(vec3(1.0), vec3(0.0), uTone), inLens * 0.075 * (1.0 - lifted) * strength);

    // regular glass evens toward the panel's own tone, so the labels on it stay legible over any content
    g = mix(g, vec3(mix(0.17, 0.93, uTone)), (1.0 - uClear) * mix(0.22, 0.38, uTone) * strength);
    g = mix(vec3(dot(g, LUM)), g, uSat);
    g *= 1.0 - 0.35 * uClear * strength;
    // tinted glass is coloured glass: vivid, still following the brightness behind it
    g = mix(g, tintColor * (0.82 + 0.35 * dot(g, LUM)) + 0.04, tint * 0.82);

    // light on the cut edge: brightest facing the light, fainter where the light leaves
    vec2 ld = normalize(uLight.xy + 1e-5);
    float facing = dot(n, ld);
    float rim = 1.0 - smoothstep(0.35 * uDpr, 1.5 * uDpr, e);
    float inner = (1.0 - smoothstep(0.0, bezel * 0.6, e)) * (1.0 - rim);
    float lit = rim * (0.16 + 0.62 * pow(max(facing, 0.0), 1.5) + 0.26 * pow(max(-facing, 0.0), 2.0))
              + inner * (0.07 * max(facing, 0.0) + 0.04 * max(-facing, 0.0));
    float shade = rim * uTone * 0.12 * (1.0 - max(facing, 0.0));
    float glow = press * exp(-length(px - uGlassP[k].xy) / (46.0 * uDpr)) * 0.22;
    vec3 glass = mix(g, uSolid, uSolidOnly) * (1.0 - shade);
    // fills that belong to the glass (a slider's level, the chosen scene) sit inside it, crisp, under its light
    for (int i = 0; i < 6; i++) {
      if (i >= uOverN) break;
      float so = sdRound(px, uOver[i], uOverR[i]);
      glass = mix(glass, uOverC[i].rgb, (1.0 - smoothstep(-0.6 * uDpr, 0.6 * uDpr, so)) * uOverC[i].a);
    }
    glass += uLightColor * (lit * uLight.z * mix(1.0, 0.85, uTone) * strength + glow);
    col = mix(col, glass, 1.0 - smoothstep(-0.75 * uDpr, 0.75 * uDpr, d));
  }

  // a ping: one ring of light leaving a glass shape when something it holds needs you
  if (uPingK.y > 0.001) {
    float rr = sdRound(px, uPing, uPingR) - uPingK.x;
    float w = 2.5 * uDpr + uPingK.x * 0.2;
    float ring = exp(-(rr * rr) / (w * w)) * uPingK.y * smoothstep(-uDpr, 2.0 * uDpr, d);
    vec3 pc = uTints[int(clamp(uPingK.z, 0.0, 3.0) + 0.5)];
    col = mix(col + pc * ring * 0.55, mix(col, pc, clamp(ring * 0.65, 0.0, 1.0)), uTone);
  }
  o = vec4(col, 1.0);
}`;
