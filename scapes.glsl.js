// Standby scapes, drawn in the scene pass. p is in panel pixels within one 720 × 720 scape.
// Each is calm by design: slow drift, no blinking, and a light and a dark version of the same idea.

export const SCAPES = `
// 5 × 7 digits, one 5-bit row each, top row first.
const int FONT[70] = int[70](
  14,17,19,21,25,17,14,  4,12,4,4,4,4,14,  14,17,1,2,4,8,31,  31,2,4,2,1,17,14,  2,6,10,18,31,2,2,
  31,16,30,1,1,17,14,  6,8,16,30,17,17,14,  31,1,2,4,8,8,8,  14,17,17,14,17,17,14,  14,17,17,15,1,2,12);

float sdSeg(vec2 p, vec2 a, vec2 b) {
  vec2 pa = p - a, ba = b - a;
  return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
}
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise1(float x) { float i = floor(x), f = fract(x); return mix(hash(vec2(i, 3.0)), hash(vec2(i + 1.0, 3.0)), f * f * (3.0 - 2.0 * f)); }

// Dot matrix: slow metaballs swell the dots; the time is set in the same dots.
vec3 scapeDots(vec2 p, vec3 ground) {
  const float cell = 22.0;
  vec2 origin = vec2(30.0);
  vec2 id = floor((p - origin) / cell);
  vec2 centre = origin + (id + 0.5) * cell;
  float field = 0.0;
  for (int i = 0; i < 5; i++) {
    float fi = float(i);
    vec2 b = vec2(360.0, 380.0) + vec2(sin(uTime * (0.05 + fi * 0.011) + fi * 1.7), cos(uTime * (0.04 + fi * 0.013) + fi * 2.3)) * vec2(250.0, 210.0);
    float r = 64.0 + 14.0 * sin(uTime * 0.07 + fi * 1.3);
    field += r * r / max(dot(centre - b, centre - b), 1.0);
  }
  int col = int(id.x) - 3, row = int(id.y) - 3;
  float lit = 0.0;
  if (row >= 0 && row < 7 && col >= 0 && col < 25) {
    int glyph = -1, lx = 0;
    if (col < 5) { glyph = uDigits.x; lx = col; }
    else if (col >= 6 && col < 11) { glyph = uDigits.y; lx = col - 6; }
    else if (col == 12) { lit = (row == 2 || row == 4) ? 1.0 : 0.0; }
    else if (col >= 14 && col < 19) { glyph = uDigits.z; lx = col - 14; }
    else if (col >= 20) { glyph = uDigits.w; lx = col - 20; }
    if (glyph >= 0) lit = float((FONT[glyph * 7 + row] >> (4 - lx)) & 1);
  }
  // the blobs keep clear of the time and its caption, so both stay readable
  float inTime = (row >= -1 && row <= 7 && col >= -1 && col <= 25) ? 1.0 : 0.0;
  float caption = 1.0 - smoothstep(16.0, 40.0, abs(p.y - 330.0));
  float actions = 1.0 - smoothstep(34.0, 58.0, abs(p.y - 486.0)); // where this face puts the standby actions
  float blob = smoothstep(0.7, 1.9, field) * (1.0 - 0.94 * max(max(inTime, caption), actions));
  float radius = mix(1.7, cell * 0.43, max(blob * 0.92, lit));
  float mask = 1.0 - smoothstep(radius - 0.8, radius + 0.8, length(p - centre));
  vec3 idle = mix(ground + 0.075, ground - 0.1, uTone);
  vec3 swell = mix(uIce * 0.78, vec3(0.36, 0.5, 0.64), uTone);
  vec3 dotColour = mix(mix(idle, swell, blob), uInk, lit);
  return mix(ground, dotColour, mask);
}

// A Braun-style dial: hour and minute ticks, ink hands, a signal second hand that steps into place.
vec3 scapeDial(vec2 p, vec3 ground) {
  vec2 d = p - vec2(360.0, 318.0);
  float r = length(d);
  const float R = 232.0;
  vec3 face = mix(vec3(0.07, 0.075, 0.085), vec3(0.985, 0.985, 0.975), uTone);
  vec3 ink = uInk;
  float onFace = 1.0 - smoothstep(R - 0.8, R + 0.8, r);
  vec3 c = mix(ground, face, onFace);
  c = mix(c, mix(vec3(0.2), vec3(0.8), uTone), (1.0 - smoothstep(0.0, 1.2, abs(r - R))) * 0.6);

  const float STEP = 6.2831853 / 60.0;
  float k = floor(atan(d.x, -d.y) / STEP + 0.5);
  vec2 dir = vec2(sin(k * STEP), -cos(k * STEP));
  bool major = mod(k, 5.0) < 0.5;
  float tick = sdSeg(d, dir * (R - 16.0 - (major ? 30.0 : 12.0)), dir * (R - 16.0)) - (major ? 2.6 : 0.9);
  c = mix(c, ink, (1.0 - smoothstep(-0.8, 0.8, tick)) * (major ? 1.0 : 0.55));

  float ha = uClock.x / 12.0 * 6.2831853, ma = uClock.y / 60.0 * 6.2831853, sa = uClock.z;
  vec2 hd = vec2(sin(ha), -cos(ha)), md = vec2(sin(ma), -cos(ma)), sd = vec2(sin(sa), -cos(sa));
  float hands = min(sdSeg(d, -hd * 18.0, hd * 128.0) - 6.0, sdSeg(d, -md * 22.0, md * 196.0) - 4.0);
  float shadow = min(sdSeg(d - vec2(0.0, 5.0), -hd * 18.0, hd * 128.0) - 6.0, sdSeg(d - vec2(0.0, 5.0), -md * 22.0, md * 196.0) - 4.0);
  c *= 1.0 - mix(0.35, 0.12, uTone) * (1.0 - smoothstep(-2.0, 9.0, shadow)) * onFace;
  c = mix(c, ink, 1.0 - smoothstep(-0.8, 0.8, hands));
  float second = min(sdSeg(d, -sd * 44.0, sd * 214.0) - 1.3, r - 8.5);
  c = mix(c, uSignal, 1.0 - smoothstep(-0.8, 0.8, second));
  return mix(c, face, 1.0 - smoothstep(-0.8, 0.8, r - 2.5));
}

// A horizon that follows the time of day: the sun on its arc, three ridges, stars after dark.
vec3 scapeHorizon(vec2 p, vec3 ground) {
  vec2 uv = p / 720.0;
  float day = (uClock.x - 6.5) / 13.0;
  float up = step(0.0, day) * step(day, 1.0);
  float edge = min(abs(day), abs(1.0 - day));
  float dusk = exp(-edge * edge * 60.0);
  float night = (1.0 - up) * smoothstep(0.0, 0.08, edge);
  vec3 top = mix(mix(vec3(0.24, 0.45, 0.74), vec3(0.62, 0.76, 0.92), uTone), mix(vec3(0.15, 0.16, 0.34), vec3(0.6, 0.62, 0.8), uTone), dusk);
  vec3 low = mix(mix(vec3(0.62, 0.77, 0.9), vec3(0.93, 0.95, 0.97), uTone), mix(vec3(0.95, 0.5, 0.32), vec3(1.0, 0.8, 0.66), uTone), dusk);
  top = mix(top, mix(vec3(0.015, 0.02, 0.05), vec3(0.45, 0.5, 0.64), uTone), night);
  low = mix(low, mix(vec3(0.05, 0.07, 0.14), vec3(0.72, 0.74, 0.82), uTone), night);
  vec3 c = mix(top, low, smoothstep(0.0, 0.7, uv.y));
  c += step(0.9972, hash(floor(p / 2.5))) * night * (1.0 - uv.y) * mix(0.85, 0.25, uTone);

  vec2 sun = vec2(mix(80.0, 640.0, clamp(day, 0.0, 1.0)), 470.0 - sin(3.14159 * clamp(day, 0.0, 1.0)) * 320.0);
  float ds = length(p - sun);
  vec3 sunColour = mix(vec3(1.0, 0.95, 0.82), vec3(1.0, 0.58, 0.32), dusk);
  c += sunColour * exp(-ds * ds / 7000.0) * 0.4 * up;
  c = mix(c, sunColour, (1.0 - smoothstep(34.0, 36.0, ds)) * up);

  for (int i = 0; i < 3; i++) {
    float fi = float(i);
    float x = uv.x * (2.4 + fi * 1.3) + fi * 5.1;
    float h = 0.58 + fi * 0.085 + (noise1(x) * 0.65 + noise1(x * 2.7) * 0.25 + noise1(x * 6.1) * 0.1 - 0.5) * (0.11 - fi * 0.025);
    vec3 near = mix(vec3(0.03, 0.04, 0.07), vec3(0.34, 0.38, 0.44), uTone);
    vec3 far = mix(mix(vec3(0.2, 0.25, 0.36), vec3(0.64, 0.69, 0.76), uTone), low, 0.35);
    vec3 ridge = mix(far, near, fi / 2.0);
    c = mix(c, ridge, smoothstep(h - 0.0015, h + 0.0015, uv.y));
  }
  return c;
}
`;
