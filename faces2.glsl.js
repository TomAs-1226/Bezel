// Faces 8–11 of standby, drawn in the scene pass after faces.glsl.js: flip clock, album, orrery, next up.
// p is in panel pixels within one 720 × 720 face. Text on them is rasterized DOM, except the flip clock's
// numerals, which are drawn here because they fold with the flaps.
// Uses sdRound (scene.glsl.js) and sdSeg, hash, noise1 (scapes.glsl.js).
// Spare uniform components it reads, filled by faces2.js helpers:
//   uClock.w  seconds left in the flip window at the top of a minute, 0 when settled (flipLeft)
//   uMoon.z   weekday + 1, Sunday = 1, 0 when unknown (skyNow)
//   uMoon.w   days since J2000.0 (skyNow)

export const FACES2 = `
const float F2PI = 3.14159265;
const float F2TAU = 6.2831853;
const float F2DEG = 0.017453293;

// Distance to a circular arc from a0 to a1 (radians, y down, a0 < a1), with round ends.
float f2Arc(vec2 q, vec2 c, float r, float a0, float a1) {
  vec2 d = q - c;
  float a = a0 + mod(atan(d.y, d.x) - a0, F2TAU);
  if (a <= a1) return abs(length(d) - r);
  return min(length(d - r * vec2(cos(a0), sin(a0))), length(d - r * vec2(cos(a1), sin(a1))));
}
// Distance to an ellipse outline, good near the curve (enough for a stroke).
float f2Oval(vec2 q, vec2 ab) {
  float k1 = length(q / (ab * ab));
  if (k1 < 1e-4) return min(ab.x, ab.y);
  float k0 = length(q / ab);
  return abs(k0 * (k0 - 1.0) / k1);
}

// Numerals as strokes, after Google Sans Flex: round bowls, a flagged 1, a closed 4, a curved stem on 6 and 9.
// q is in units of the numeral's height, centred, y down. Returns the distance to the stroke's centreline.
float f2Glyph(vec2 q, int n) {
  const float T = -0.455;
  const float B = 0.455;
  if (n == 9) { q = -q; n = 6; }
  if (n == 0) return f2Oval(q, vec2(0.232, 0.455));
  if (n == 1) return min(sdSeg(q, vec2(0.05, T), vec2(0.05, B)), sdSeg(q, vec2(0.05, T), vec2(-0.13, T + 0.12)));
  if (n == 2) {
    float bowl = f2Arc(q, vec2(0.0, -0.235), 0.215, -165.0 * F2DEG, 32.0 * F2DEG);
    return min(min(bowl, sdSeg(q, vec2(0.182, -0.121), vec2(-0.232, B))), sdSeg(q, vec2(-0.232, B), vec2(0.236, B)));
  }
  if (n == 3) {
    float top = f2Arc(q, vec2(0.0, -0.245), 0.21, -160.0 * F2DEG, 90.0 * F2DEG);
    float bottom = f2Arc(q, vec2(0.0, 0.2075), 0.2425, -90.0 * F2DEG, 160.0 * F2DEG);
    return min(min(top, bottom), sdSeg(q, vec2(-0.08, -0.035), vec2(0.0, -0.035)));
  }
  if (n == 4) {
    float stem = sdSeg(q, vec2(0.13, T), vec2(0.13, B));
    return min(min(stem, sdSeg(q, vec2(0.13, T), vec2(-0.236, 0.17))), sdSeg(q, vec2(-0.236, 0.17), vec2(0.25, 0.17)));
  }
  if (n == 5) {
    float bar = sdSeg(q, vec2(0.215, T), vec2(-0.17, T));
    float stem = sdSeg(q, vec2(-0.17, T), vec2(-0.209, 0.054));
    return min(min(bar, stem), f2Arc(q, vec2(0.0, 0.2), 0.255, -145.0 * F2DEG, 150.0 * F2DEG));
  }
  if (n == 6) {
    float bowl = abs(length(q - vec2(0.0, 0.2175)) - 0.2375);
    return min(bowl, f2Arc(q, vec2(0.55028, 0.2175), 0.78778, F2PI, 238.6 * F2DEG));
  }
  if (n == 7) return min(sdSeg(q, vec2(-0.236, T), vec2(0.236, T)), sdSeg(q, vec2(0.236, T), vec2(-0.06, B)));
  return min(abs(length(q - vec2(0.0, -0.2425)) - 0.2125), abs(length(q - vec2(0.0, 0.2125)) - 0.2425));
}

// Ink of a two-digit number on a flip card. c is card-local (y down, hinge at 0), aa in card pixels.
float f2Number(vec2 c, int v, float aa) {
  const float G = 196.0;
  bool left = c.x < 0.0;
  int n = left ? v / 10 : v - (v / 10) * 10;
  vec2 q = (c - vec2(left ? -66.0 : 66.0, 0.0)) / G;
  return 1.0 - smoothstep(-aa, aa, (f2Glyph(q, n) - 0.046) * G);
}

// A card's silhouette: a rounded rect with a notch at each end of the hinge.
float f2Card(vec2 u, vec2 hs) {
  return max(sdRound(u, vec4(-hs, 2.0 * hs), 26.0), 6.0 - length(vec2(abs(u.x) - hs.x, u.y)));
}

// The flap's angle t seconds after it lets go: a damped spring from up (0) to down (pi) that bounces off the stop.
float f2Fall(float t) {
  if (t <= 0.0) return 0.0;
  const float W0 = 15.0;
  const float Z = 0.72;
  float wd = W0 * sqrt(1.0 - Z * Z);
  float a = F2PI * (1.0 - exp(-Z * W0 * t) * (cos(wd * t) + Z * W0 / wd * sin(wd * t)));
  return F2PI - abs(F2PI - a);
}

// Flip clock: two split-flap cards, hours and minutes, after Gino Valle's Cifra 3. When a card's number changes,
// its top flap falls about the hinge in perspective, lit by the panel's one light, and settles on a damped spring.
vec3 faceFlip(vec2 p, vec3 ground) {
  const vec2 HS = vec2(152.0, 168.0);
  const float D = 2000.0;
  const vec3 LIGHT = vec3(-0.36, 0.56, 0.75);
  float aa = 0.7 / max(uUnit, 0.3);
  vec2 uv = p / 720.0 - vec2(0.5, 0.38);
  vec3 c = ground * (1.0 - dot(uv, uv) * mix(0.55, 0.06, uTone));
  vec3 card = mix(mix(ground, uInk, 0.085), mix(ground, vec3(1.0), 0.84), uTone);
  vec3 ink = mix(card, uInk, 0.94);

  // one soft shadow for both cards, so neither darkens the other
  float sh = min(f2Card(p - vec2(196.0, 290.0), HS), f2Card(p - vec2(524.0, 290.0), HS));
  c *= 1.0 - (1.0 - smoothstep(-16.0, 44.0, sh)) * mix(0.46, 0.13, uTone);

  int hh = uDigits.x * 10 + uDigits.y;
  int mm = uDigits.z * 10 + uDigits.w;
  float since = uClock.w > 0.0 ? 0.6 - uClock.w : 99.0;

  for (int k = 0; k < 2; k++) {
    vec2 u = p - vec2(k == 0 ? 196.0 : 524.0, 276.0);
    if (abs(u.x) > HS.x + 40.0 || abs(u.y) > HS.y + 12.0) continue;
    int now = k == 0 ? hh : mm;
    int before = k == 0 ? (mm == 0 ? (hh + 23) % 24 : hh) : (mm + 59) % 60;
    float theta = before != now ? f2Fall(since - (k == 0 ? 0.06 : 0.0)) : F2PI;
    bool moving = theta < F2PI - 0.001;

    // the two fixed halves: the new top is already there behind the flap, the old bottom waits to be covered
    float dc = f2Card(u, HS);
    if (dc < aa) {
      bool top = u.y < 0.0;
      vec3 face = card * (top ? 1.03 : 0.97);
      vec3 s = mix(face, ink, f2Number(u, top || !moving ? now : before, aa));
      if (!top && moving && theta > 0.0) {
        // the flap's shadow runs down the lower half while the flap stands out from the card
        float reach = HS.y * (0.75 * sin(theta) - cos(theta));
        s *= 1.0 - (1.0 - smoothstep(reach - 26.0, reach + 26.0, u.y)) * sqrt(sin(theta)) * mix(0.38, 0.18, uTone);
      }
      s = mix(s, card * mix(0.32, 0.7, uTone), 1.0 - smoothstep(1.0, 1.0 + 2.0 * aa, abs(u.y)));
      s += (1.0 - smoothstep(0.0, 2.0 * aa, abs(u.y - 2.4))) * 0.035;
      c = mix(c, s, 1.0 - smoothstep(-aa, aa, dc));
    }

    // the falling flap: invert the perspective to find where on the flap this pixel lies
    if (moving && theta > 0.001) {
      float st = sin(theta);
      float ct = cos(theta);
      float den = u.y * st - D * ct;
      if (abs(den) > 1e-3) {
        float h = u.y * D / den;
        if (h > 0.0 && h < HS.y + 2.0) {
          bool front = theta < F2PI * 0.5;
          vec2 cp = vec2(u.x * (D - h * st) / D, front ? -h : h);
          float squash = max(abs(ct) * D / (D - h * st), 0.25);
          float caa = aa / squash;
          vec3 n = front ? vec3(0.0, -st, ct) : vec3(0.0, st, -ct);
          float lit = 1.0 + mix(0.34, 0.24, uTone) * (clamp(dot(n, LIGHT), -0.25, 1.0) / 0.75 - 1.0);
          vec3 face = card * (front ? 1.03 : 0.97) * lit;
          vec3 s = mix(face, ink * lit, f2Number(cp, front ? before : now, caa));
          c = mix(c, s, 1.0 - smoothstep(-caa, caa, f2Card(cp, HS)));
        }
      }
    }
  }
  return c;
}

// Album: the playing cover as a large soft square on a wash of its own colours. A slow Ken Burns drift moves
// inside the square while the wash drifts the other way, and the cover's light spills onto the wash around it.
vec3 faceAlbum(vec2 p, vec3 ground) {
  const vec4 SQ = vec4(180.0, 50.0, 360.0, 360.0);
  const float RAD = 28.0;
  float aa = 0.7 / max(uUnit, 0.3);
  // value noise, so the drift never quite repeats and stays far slower than breathing
  float zoom = 1.05 + 0.08 * noise1(uTime * 0.021 + 3.7);
  vec2 room = vec2(0.5 - 0.5 / zoom);
  vec2 pan = (vec2(noise1(uTime * 0.017 + 11.3), noise1(uTime * 0.015 + 27.1)) * 2.0 - 1.0) * room * 0.9;

  vec2 uv = p / 720.0;
  vec2 w = (uv - 0.5) * 0.9 + 0.5 - pan * 0.6;
  vec3 wash = textureLod(uArt, w, 6.0).rgb * 0.4
    + (textureLod(uArt, w + vec2(0.06, 0.0), 6.0).rgb + textureLod(uArt, w - vec2(0.06, 0.0), 6.0).rgb
    + textureLod(uArt, w + vec2(0.0, 0.06), 6.0).rgb + textureLod(uArt, w - vec2(0.0, 0.06), 6.0).rgb) * 0.15;
  float low = smoothstep(0.5, 0.95, uv.y);
  vec3 dark = mix(ground, wash / (1.0 + dot(wash, vec3(0.2126, 0.7152, 0.0722)) * 1.4), 0.6) * (1.0 - 0.34 * low);
  vec3 light = mix(mix(ground, wash, 0.28) + 0.035, ground + 0.03, 0.4 * low);
  vec3 c = mix(dark, light, uTone);

  // the cover's light spills onto the wash, and a soft shadow sits under the square
  float d = sdRound(p, SQ, RAD);
  vec2 cuv = (p - SQ.xy) / SQ.zw;
  float outside = step(0.0, d);
  c += textureLod(uArt, clamp(cuv, 0.0, 1.0), 6.5).rgb * exp(-max(d, 0.0) / 70.0) * mix(0.12, 0.05, uTone) * outside;
  c *= 1.0 - (1.0 - smoothstep(-24.0, 56.0, sdRound(p - vec2(0.0, 20.0), SQ, RAD))) * mix(0.5, 0.2, uTone) * outside;

  if (d < aa) {
    vec2 k = (cuv - 0.5) / zoom + 0.5 + pan;
    float lod = max(log2(float(textureSize(uArt, 0).x) / (SQ.z * uUnit * zoom)), 0.0);
    vec3 art = textureLod(uArt, k, lod).rgb;
    // a hairline inside the edge keeps a dark cover's shape on a dark wash, and a light one's on light
    art = mix(art, art + mix(vec3(0.07), vec3(-0.05), uTone), (1.0 - smoothstep(0.0, 1.5, -d)) * 0.8);
    c = mix(c, art, 1.0 - smoothstep(-aa, aa, d));
  }
  return c;
}

// Orrery elements (J2000 mean elements): display radius, eccentricity, longitude of perihelion and mean longitude
// in degrees, sidereal period in days, ball radius and colour. Radii are spaced for the panel, not to scale.
const float F2A[8] = float[8](56.0, 86.0, 116.0, 148.0, 194.0, 238.0, 282.0, 322.0);
const float F2E[8] = float[8](0.2056, 0.0068, 0.0167, 0.0934, 0.0484, 0.0539, 0.0473, 0.0086);
const float F2W[8] = float[8](77.46, 131.53, 102.94, 336.04, 14.73, 92.6, 170.95, 44.96);
const float F2L[8] = float[8](252.25, 181.98, 100.46, 355.45, 34.4, 49.94, 313.23, 304.88);
const float F2P[8] = float[8](87.969, 224.701, 365.256, 686.98, 4332.59, 10759.22, 30688.5, 60182.0);
const float F2R[8] = float[8](3.2, 5.0, 5.4, 4.2, 10.5, 8.8, 6.6, 6.4);
const vec3 F2C[8] = vec3[8](vec3(0.66, 0.64, 0.62), vec3(0.93, 0.87, 0.74), vec3(0.42, 0.62, 0.9), vec3(0.84, 0.5, 0.36),
  vec3(0.88, 0.78, 0.64), vec3(0.92, 0.84, 0.64), vec3(0.64, 0.84, 0.88), vec3(0.4, 0.54, 0.9));
const float F2TY = 0.46;   // the ecliptic seen from about 27° above it: depth shrinks to 0.46
const float F2CE = 0.8879; // and its cosine, for light toward the viewer

// A planet's offset from the sun on screen, and its heliocentric longitude, from Kepler's equation of centre.
vec2 f2Planet(int i, float days, out float lambda) {
  float e = F2E[i];
  float m = fract((F2L[i] - F2W[i]) / 360.0 + days / F2P[i]) * F2TAU;
  float nu = m + (2.0 * e - 0.25 * e * e * e) * sin(m) + 1.25 * e * e * sin(2.0 * m);
  lambda = nu + F2W[i] * F2DEG;
  float r = F2A[i] * (1.0 - e * e) / (1.0 + e * cos(nu));
  return r * vec2(cos(lambda), -sin(lambda) * F2TY);
}
// Screen distance from s (an offset from the sun) to planet i's orbit: the ellipse's implicit function over its gradient.
float f2Orbit(vec2 s, int i) {
  float e = F2E[i];
  float a = F2A[i];
  float b = a * sqrt(1.0 - e * e);
  float cw = cos(F2W[i] * F2DEG);
  float sw = sin(F2W[i] * F2DEG);
  vec2 pl = vec2(s.x, -s.y / F2TY);
  vec2 k = vec2((pl.x * cw + pl.y * sw + a * e) / a, (pl.y * cw - pl.x * sw) / b);
  float len = max(length(k), 1e-4);
  float gx = k.x / (a * len);
  float gy = k.y / (b * len);
  vec2 grad = vec2(gx * cw - gy * sw, -(gx * sw + gy * cw) / F2TY);
  return abs(len - 1.0) / max(length(grad), 1e-5);
}

// Orrery: the eight planets on their real ellipses, seen at a slant, starting from today's real mean longitudes
// (uMoon.w) and sped up so Mercury laps in a minute. The sun lights each ball, so the near ones show crescents.
// Today's namesake (Sunday the sun, Monday the moon, Tuesday Mars…) wears a thin signal ring on a tinted orbit.
vec3 faceOrrery(vec2 p, vec3 ground) {
  const vec2 SUN = vec2(360.0, 348.0);
  float aa = 0.7 / max(uUnit, 0.3);
  vec2 s = p - SUN;
  float rs = length(s);
  vec2 uv = p / 720.0 - vec2(0.5, 0.48);
  vec3 c = ground * (1.0 - dot(uv, uv) * mix(0.8, 0.1, uTone));
  c += step(0.9984, hash(floor(p / 3.0))) * mix(0.3, 0.0, uTone) * smoothstep(60.0, 160.0, rs);
  c += vec3(1.0, 0.7, 0.38) * (exp(-rs / 30.0) * 0.28 + exp(-rs / 150.0) * 0.05) * mix(1.0, 0.3, uTone);

  int wd = int(uMoon.z + 0.5) - 1;
  int mark = wd == 0 ? 8 : wd == 1 ? 9 : wd == 2 ? 3 : wd == 3 ? 0 : wd == 4 ? 4 : wd == 5 ? 1 : wd == 6 ? 5 : -1;
  float days = uMoon.w + uTime * 1.46615;
  float ring = 1.4 * aa;

  vec2 pos[8];
  float lam[8];
  vec3 lineC = mix(ground, uInk, mix(0.16, 0.24, uTone));
  for (int i = 0; i < 8; i++) {
    float l;
    pos[i] = f2Planet(i, days, l);
    lam[i] = l;
    float line = 1.0 - smoothstep(0.35, 0.35 + ring, f2Orbit(s, i));
    c = mix(c, i == mark ? mix(lineC, uSignal, 0.6) : lineC, line * (0.6 + 0.4 * smoothstep(-24.0, 24.0, s.y)));
  }

  // far half of the system, then the sun, then the near half
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      float t = min(rs / 13.0, 1.0);
      vec3 sunC = mix(mix(vec3(1.0, 0.97, 0.9), vec3(1.0, 0.74, 0.4), t * t), mix(vec3(1.0, 0.85, 0.52), vec3(0.95, 0.6, 0.25), t), uTone);
      c = mix(c, sunC, 1.0 - smoothstep(13.0 - aa, 13.0 + aa, rs));
      if (mark == 8) c = mix(c, uSignal, 1.0 - smoothstep(0.6, 0.6 + ring, abs(rs - 20.0)));
    }
    for (int i = 0; i < 8; i++) {
      if ((pos[i].y < 0.0) != (pass == 0)) continue;
      float rp = F2R[i] * (1.0 + 0.1 * pos[i].y / (F2A[i] * F2TY));
      vec2 d = s - pos[i];
      float reach = rp * 2.6 + 12.0;
      if (dot(d, d) > reach * reach) continue;
      vec3 col = F2C[i];
      vec3 L = vec3(-cos(lam[i]), -sin(lam[i]) * F2TY, sin(lam[i]) * F2CE);
      float dl = length(d);
      // Saturn's rings, the far half behind the ball and the near half over it
      float rr = length(vec2(d.x, d.y / 0.36)) / rp;
      float band = i == 5 ? smoothstep(1.45, 1.6, rr) * (1.0 - smoothstep(2.05, 2.25, rr)) : 0.0;
      vec3 bandC = mix(mix(ground, col, 0.62), mix(ground, col * 0.62, 0.7), uTone);
      c = mix(c, bandC, band * step(d.y, 0.0));
      if (dl < rp + aa) {
        vec3 n = vec3(d.x / rp, -d.y / rp, sqrt(max(1.0 - dl * dl / (rp * rp), 0.0)));
        vec3 day = mix(col, col * 0.8, uTone) * (0.78 + 0.22 * n.z);
        if (i == 4) day *= 1.0 - 0.09 * smoothstep(0.35, 0.85, abs(sin(n.y * 7.0)));
        vec3 night = mix(ground + col * 0.06, mix(ground, col * 0.45, 0.55), uTone);
        c = mix(c, mix(night, day, smoothstep(-0.15, 0.2, dot(n, L))), 1.0 - smoothstep(rp - aa, rp + aa, dl));
      }
      c = mix(c, bandC, band * step(0.0, d.y));
      if (i == mark) c = mix(c, uSignal, 1.0 - smoothstep(0.6, 0.6 + ring, abs(dl - rp - 6.0)));
      if (i == 2) {
        // the moon turns about the earth in about nineteen seconds
        float lm = fract(218.316 / 360.0 + days / 27.321661) * F2TAU;
        vec2 dm = d - 13.0 * vec2(cos(lm), -sin(lm) * F2TY);
        float rm = length(dm);
        vec3 nm = vec3(dm / 1.9 * vec2(1.0, -1.0), sqrt(max(1.0 - rm * rm / 3.61, 0.0)));
        vec3 moonC = mix(mix(ground, vec3(0.8), 0.12), mix(vec3(0.84), vec3(0.45), uTone), smoothstep(-0.15, 0.2, dot(nm, L)));
        c = mix(c, moonC, 1.0 - smoothstep(1.9 - aa, 1.9 + aa, rm));
        if (mark == 9) c = mix(c, uSignal, 1.0 - smoothstep(0.6, 0.6 + ring, abs(rm - 6.0)));
      }
    }
  }
  return c;
}

// Next up: the day as an arc from midnight over noon to midnight, filled up to now, with hour ticks above it.
// The countdown, the rows and the event dots on the arc are raster text that faces2.js places on the same circle.
vec3 faceNext(vec2 p, vec3 ground) {
  const vec2 C = vec2(360.0, 1043.1);
  const float R = 747.1;
  const float HALF = 0.44263;
  float aa = 0.7 / max(uUnit, 0.3);
  vec2 uv = p / 720.0 - vec2(0.5, 0.35);
  vec3 c = ground * (1.0 - dot(uv, uv) * mix(0.6, 0.08, uTone));
  vec3 bg = c;

  vec2 d = p - C;
  float band = abs(length(d) - R);
  float f = (atan(d.x, -d.y) + HALF) / (2.0 * HALF);
  float now = clamp(uClock.x / 24.0, 0.0, 1.0);
  float na = -HALF + 2.0 * HALF * now;
  vec2 e0 = C + R * vec2(-sin(HALF), -cos(HALF));
  vec2 e1 = C + R * vec2(sin(HALF), -cos(HALF));
  vec2 head = C + R * vec2(sin(na), -cos(na));
  float track = (f >= 0.0 && f <= 1.0 ? band : min(length(p - e0), length(p - e1))) - 3.0;
  float fill = (f >= 0.0 && f <= now ? band : min(length(p - e0), length(p - head))) - 3.0;
  c = mix(c, mix(ground + 0.07, ground - 0.075, uTone), 1.0 - smoothstep(-aa, aa, track));
  c = mix(c, mix(mix(ground, uIce, 0.7), mix(uIce * 0.5, uInk, 0.35), uTone), 1.0 - smoothstep(-aa, aa, fill));

  // hour ticks above the arc, longer every six hours
  float k = floor(clamp(f, 0.0, 1.0) * 24.0 + 0.5);
  float ta = -HALF + 2.0 * HALF * k / 24.0;
  vec2 dir = vec2(sin(ta), -cos(ta));
  float major = mod(k, 6.0) < 0.5 ? 1.0 : 0.0;
  float tick = sdSeg(d, dir * (R + 9.0), dir * (R + 13.0 + 5.0 * major)) - (0.6 + 0.35 * major);
  c = mix(c, uInk, (1.0 - smoothstep(-aa, aa, tick)) * mix(0.22, 0.5, major));

  // now: an ink dot on the arc, with a little room cleared around it
  float dh = length(p - head);
  c = mix(c, bg, 1.0 - smoothstep(10.5 - aa, 10.5 + aa, dh));
  return mix(c, uInk, 1.0 - smoothstep(7.0 - aa, 7.0 + aa, dh));
}
`;
