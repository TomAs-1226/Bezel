// Pass A of Bezel's renderer: everything under the glass, in device pixels from the top-left.
// Seven slots side by side (four home pages, apps, now playing, standby), then an app window that can open
// over them. Page surfaces and text draw first; the window's own ground, surfaces and text draw inside it.

import { FACES } from './faces.glsl.js';
import { FACES2 } from './faces2.glsl.js';
import { SCAPES } from './scapes.glsl.js';

const SDF = `
float sdRound(vec2 p, vec4 r, float rad) {
  vec2 h = r.zw * 0.5;
  vec2 q = abs(p - r.xy - h) - h + rad;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
}`;

export const SCENE_FS = `#version 300 es
precision highp float;
in vec2 v; out vec4 o;
uniform vec2 uRes; uniform float uDpr, uUnit, uTime, uTone, uGrid, uPage, uScroll, uParallax, uEdge;
uniform vec3 uGround, uInk, uIce, uSignal, uGlowA, uGlowB;
uniform sampler2D uArt; uniform vec4 uDisc, uRing;
uniform vec4 uDial; uniform vec3 uDialColor;
uniform float uScapeF; uniform vec4 uClock; uniform ivec4 uDigits; uniform vec4 uWorld; uniform vec4 uMoon;
// surfaces: rect, colour, clip rect, (radius, clip radius or -1, state layer, takes ink), touch point
uniform vec4 uSurf[40]; uniform vec4 uSurfC[40]; uniform vec4 uSurfClip[40]; uniform vec4 uSurfM[40]; uniform vec4 uSurfL[40];
uniform int uSurfN, uSurfSplit, uSurfSplit2;
uniform vec4 uWin; uniform float uWinR, uWinShadow; uniform vec3 uWinColor;
uniform sampler2D uTex0, uTex1, uTex2, uTex3, uTex4;
uniform vec4 uRect0, uRect1, uRect2, uRect3, uRect4;
${SDF}
${SCAPES}
${FACES}
${FACES2}
vec4 layer(sampler2D t, vec4 r, vec2 px) {
  vec2 uv = (px - r.xy) / r.zw;
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec4(0.0);
  return texture(t, uv);
}
const float SWEEP = 4.712389;
const float START = -2.3561945;
vec2 onRing(vec2 c, float r, float t) { float a = START + t * SWEEP; return c + vec2(sin(a), -cos(a)) * r; }

void surface(int i, vec2 px, inout vec3 col, inout vec3 ink, inout float inked, float clip) {
  float s = sdRound(px, uSurf[i], uSurfM[i].x);
  if (uSurfM[i].y >= 0.0) s = max(s, sdRound(px, uSurfClip[i], uSurfM[i].y));
  float m = (1.0 - smoothstep(-0.6 * uDpr, 0.6 * uDpr, s)) * uSurfC[i].a * clip;
  if (m <= 0.0) return;
  vec3 sc = uSurfC[i].rgb;
  bool light = dot(sc, vec3(0.2126, 0.7152, 0.0722)) > 0.5;
  // a fill's own on-colour: a deep shade of its hue on light fills, near white on dark ones
  if (uSurfM[i].w > 0.0) ink = light ? pow(sc, vec3(2.2)) * 0.26 : mix(sc, vec3(1.0), 0.9);
  inked = mix(inked, uSurfM[i].w, m);
  if (uSurfM[i].z > 0.0) {
    // a state layer in the surface's on-colour, strongest where it was touched
    sc = mix(sc, light ? vec3(0.0) : vec3(1.0), uSurfM[i].z * (0.6 + 0.4 * exp(-length(px - uSurfL[i].xy) / (80.0 * uDpr))));
  }
  col = mix(col, sc, m);
}
void text(sampler2D t, vec4 r, vec2 px, inout vec3 col, vec3 ink, float inked, float amount) {
  vec4 c = layer(t, r, px) * amount;
  c.rgb = mix(c.rgb, ink * c.a, inked);
  col = col * (1.0 - c.a) + c.rgb;
}

void main() {
  vec2 px = vec2(v.x, 1.0 - v.y) * uRes;
  float slot = clamp(floor((px.x - uPage) / uRes.x), 0.0, 6.0);
  vec2 q = vec2(px.x - uPage - slot * uRes.x, px.y);
  vec3 col = uGround;

  if (slot < 4.5) {
    // home pages and apps: the overview's ground drifts a quarter as fast as its content
    vec2 bq = q - vec2(0.0, slot < 0.5 ? uScroll * uParallax : 0.0);
    vec2 uv = bq / uRes;
    float t = uTime * 0.05 + slot * 1.7;
    vec2 q1 = uv - vec2(0.28 + 0.07 * sin(t * 3.1), 0.95 + 0.03 * cos(t * 2.3));
    vec2 q2 = uv - vec2(0.74 + 0.06 * cos(t * 2.7), 1.04 + 0.03 * sin(t * 1.9));
    col += (uGlowA * exp(-dot(q1, q1) * 9.0) * 0.34 + uGlowB * exp(-dot(q2, q2) * 6.0) * 0.42) * mix(1.0, 0.4, uTone);
    vec2 cell = mod(bq, 24.0 * uUnit) - 12.0 * uUnit;
    float dotm = 1.0 - smoothstep(0.5 * uDpr, 1.1 * uDpr, length(cell));
    col = mix(col, mix(col + 0.05, col * 0.93, uTone), dotm * uGrid);

    if (abs(slot - 2.0) < 0.5 && uDial.z > 0.0) {
      // climate: a 270° track, half-degree ticks inside it, the set range lit from the start
      vec2 d = q - uDial.xy;
      float r = length(d);
      float tt = (atan(d.x, -d.y) - START) / SWEEP;
      float inSweep = step(0.0, tt) * step(tt, 1.0);
      float w = 9.0 * uUnit;
      vec3 track = mix(uGround + 0.07, uGround - 0.07, uTone);
      float ends = min(length(q - onRing(uDial.xy, uDial.z, 0.0)), length(q - onRing(uDial.xy, uDial.z, 1.0)));
      col = mix(col, track, max((1.0 - smoothstep(w - uDpr, w, abs(r - uDial.z))) * inSweep, 1.0 - smoothstep(w - uDpr, w, ends)));
      float lit = (1.0 - smoothstep(w - uDpr, w, abs(r - uDial.z))) * inSweep * step(tt, uDial.w);
      float caps = min(length(q - onRing(uDial.xy, uDial.z, 0.0)), length(q - onRing(uDial.xy, uDial.z, uDial.w)));
      col = mix(col, uDialColor, max(lit, 1.0 - smoothstep(w - uDpr, w, caps)));
      float k = floor(tt * 24.0 + 0.5);
      float major = mod(k, 4.0) < 0.5 ? 1.0 : 0.0;
      float ta = START + (k / 24.0) * SWEEP;
      vec2 dir = vec2(sin(ta), -cos(ta));
      float tick = sdSeg(d, dir * (uDial.z - (24.0 + major * 8.0) * uUnit), dir * (uDial.z - 17.0 * uUnit)) - (0.8 + 0.5 * major) * uDpr;
      float inTicks = step(-0.02, tt) * step(tt, 1.02);
      col = mix(col, uInk, (1.0 - smoothstep(-0.7 * uDpr, 0.7 * uDpr, tick)) * inTicks * mix(0.28, 0.6, major));
    }
  } else if (slot < 5.5) {
    // now playing: the cover washed across the screen, a turning disc, ticks and progress around it
    vec3 wash = textureLod(uArt, q / uRes, 5.5).rgb;
    col = mix(mix(uGround, wash, 0.6), mix(uGround, wash, 0.24) + 0.02, uTone);
    vec2 dd = q - uDisc.xy;
    float ang = uDisc.w;
    vec2 local = mat2(cos(ang), sin(ang), -sin(ang), cos(ang)) * dd / uDisc.z;
    col = mix(col, texture(uArt, local * 0.5 + 0.5).rgb, 1.0 - smoothstep(uDisc.z - 1.2 * uDpr, uDisc.z, length(dd)));
    vec2 rd = q - uRing.xy;
    float rr = length(rd);
    float a = atan(rd.x, -rd.y);
    const float TICK = 6.2831853 / 60.0;
    float along = abs(fract(a / TICK + 0.5) - 0.5) * TICK * rr;
    float band = 1.0 - smoothstep(0.0, uDpr, abs(rr - uRing.z) - 5.0 * uUnit);
    col = mix(col, uInk, (1.0 - smoothstep(0.4 * uDpr, 1.2 * uDpr, along)) * band * 0.42);
    float arc = 1.0 - smoothstep(0.0, uDpr, abs(rr - uRing.z - 13.0 * uUnit) - 1.2 * uDpr);
    col = mix(col, uSignal, arc * step(fract(a / 6.2831853 + 1.0), uRing.w));
  } else {
    // standby: twelve faces stacked vertically, paged by uScapeF
    vec2 pp = q / uUnit;
    float s = uScapeF + pp.y / 720.0;
    float k = clamp(floor(s), 0.0, 11.0);
    vec2 lp = vec2(pp.x, (s - k) * 720.0);
    if (k < 0.5) { vec2 vg = lp / 720.0 - vec2(0.5, 0.42); col = uGround * (1.0 - dot(vg, vg) * mix(0.9, 0.1, uTone)); }
    else if (k < 1.5) col = scapeDots(lp, uGround);
    else if (k < 2.5) col = scapeDial(lp, uGround);
    else if (k < 3.5) col = scapeHorizon(lp, uGround);
    else if (k < 4.5) col = faceWords(lp, uGround);
    else if (k < 5.5) col = faceWorld(lp, uGround);
    else if (k < 6.5) col = faceMoon(lp, uGround);
    else if (k < 7.5) col = faceTree(lp, uGround);
    else if (k < 8.5) col = faceFlip(lp, uGround);
    else if (k < 9.5) col = faceAlbum(lp, uGround);
    else if (k < 10.5) col = faceOrrery(lp, uGround);
    else col = faceNext(lp, uGround);
  }

  // the pages' surfaces and text; under the dock text dims a little, a soft scroll edge that leaves with the dock
  vec3 ink = vec3(0.0);
  float inked = 0.0;
  bool hasWin = uWin.z > 0.5;
  float wd = hasWin ? sdRound(px, uWin, uWinR) : 1e5;
  float win = hasWin ? 1.0 - smoothstep(-0.6 * uDpr, 0.6 * uDpr, wd) : 0.0;
  for (int i = 0; i < 40; i++) {
    if (i >= uSurfSplit || i >= uSurfN) break;
    surface(i, px, col, ink, inked, 1.0);
  }
  float edge = 1.0 - smoothstep(uRes.y - 170.0 * uUnit, uRes.y - 60.0 * uUnit, px.y) * 0.3 * uEdge;
  text(uTex0, uRect0, px, col, ink, inked, edge);
  text(uTex1, uRect1, px, col, ink, inked, edge);
  text(uTex2, uRect2, px, col, vec3(0.0), 0.0, 1.0);
  text(uTex3, uRect3, px, col, vec3(0.0), 0.0, 1.0);

  // an app window grows out of its icon: a shadow while it travels, then its own ground, surfaces and text
  if (hasWin) {
    col *= 1.0 - (1.0 - smoothstep(0.0, 40.0 * uUnit, wd)) * step(0.0, wd) * uWinShadow;
    col = mix(col, uWinColor, win);
    ink = vec3(0.0);
    inked = 0.0;
    for (int i = 0; i < 40; i++) {
      if (i >= uSurfN || i >= uSurfSplit2) break;
      if (i < uSurfSplit) continue;
      surface(i, px, col, ink, inked, win);
    }
    text(uTex4, uRect4, px, col, ink, inked, win);
  }

  o = vec4(col, 1.0);
}`;
