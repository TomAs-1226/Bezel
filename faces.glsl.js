// Four more standby faces, drawn in the scene pass after the first four in scapes.glsl.js.
// p is in panel pixels within one 720 × 720 face. Text on them (words, cities, phases) is rasterized DOM.

export const FACES = `
float noise2(vec2 x) {
  vec2 i = floor(x), f = fract(x), u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Word clock: the letters are text; the ground only warms a little behind the block of words.
vec3 faceWords(vec2 p, vec3 ground) {
  vec2 uv = p / 720.0 - vec2(0.5, 0.36);
  return ground + mix(uIce * 0.06, vec3(-0.025), uTone) * exp(-dot(uv, uv) * 5.0);
}

// World clocks: four dials in a row. A dial's face is lighter where it is day in that city; here has signal hands.
vec3 faceWorld(vec2 p, vec3 ground) {
  vec3 c = ground;
  const float R = 64.0;
  const float STEP = 6.2831853 / 12.0;
  for (int i = 0; i < 4; i++) {
    vec2 d = p - vec2(135.0 + float(i) * 150.0, 250.0);
    float r = length(d);
    if (r > R + 20.0) continue;
    float hours = mod(uClock.x + uWorld[i], 24.0);
    float day = smoothstep(6.0, 7.5, hours) * (1.0 - smoothstep(18.5, 20.0, hours));
    vec3 face = mix(mix(vec3(0.085, 0.09, 0.105), vec3(0.2, 0.215, 0.24), day), mix(vec3(0.79, 0.81, 0.85), vec3(0.995, 0.995, 0.985), day), uTone);
    float on = 1.0 - smoothstep(R - 0.8, R + 0.8, r);
    c = mix(c, face, on);
    float k = floor(atan(d.x, -d.y) / STEP + 0.5);
    vec2 dir = vec2(sin(k * STEP), -cos(k * STEP));
    float tick = sdSeg(d, dir * (R - 14.0), dir * (R - 7.0)) - (mod(k, 3.0) < 0.5 ? 1.6 : 0.7);
    c = mix(c, uInk, (1.0 - smoothstep(-0.8, 0.8, tick)) * on * 0.7);
    float ha = mod(hours, 12.0) / 12.0 * 6.2831853;
    float ma = uClock.y / 60.0 * 6.2831853;
    vec2 hd = vec2(sin(ha), -cos(ha)), md = vec2(sin(ma), -cos(ma));
    float hands = min(sdSeg(d, -hd * 6.0, hd * 34.0) - 3.0, sdSeg(d, -md * 8.0, md * 50.0) - 1.8);
    c = mix(c, i == 0 ? uSignal : uInk, 1.0 - smoothstep(-0.8, 0.8, hands));
    c = mix(c, i == 0 ? uSignal : uInk, 1.0 - smoothstep(3.2, 4.4, r));
  }
  return c;
}

// The moon tonight: a lit sphere whose light turns with the phase, darker maria, faint earthshine, a quiet halo.
vec3 faceMoon(vec2 p, vec3 ground) {
  const float R = 132.0;
  vec2 d = (p - vec2(360.0, 250.0)) / R;
  float r = length(d);
  vec3 c = ground + step(0.9982, hash(floor(p / 3.0))) * mix(0.45, 0.0, uTone) * smoothstep(1.3, 2.2, r);
  c += mix(vec3(0.05, 0.055, 0.06), vec3(-0.02), uTone) * exp(-max(r - 1.0, 0.0) * 5.0) * uMoon.y * step(1.0, r);
  if (r < 1.0) {
    vec3 n = vec3(d, sqrt(1.0 - r * r));
    float a = uMoon.x * 6.2831853;
    float lit = smoothstep(-0.05, 0.07, dot(n, vec3(sin(a), 0.0, -cos(a))));
    vec2 sp = d * 3.1 + 2.3;
    float maria = smoothstep(0.42, 0.82, noise2(sp) * 0.7 + noise2(sp * 2.4) * 0.3);
    float craters = step(0.986, hash(floor(d * 24.0 + 9.0)));
    vec3 albedo = vec3(0.87, 0.86, 0.82) - vec3(0.26) * (maria * 0.7 + craters * 0.25);
    vec3 dark = mix(vec3(0.04, 0.045, 0.055) + albedo * 0.03, vec3(0.7, 0.72, 0.76), uTone);
    vec3 bright = mix(albedo, vec3(1.0) - (vec3(0.87) - albedo) * 0.5, uTone);
    c = mix(c, mix(dark, bright, lit), 1.0 - smoothstep(1.0 - 1.6 / R, 1.0, r));
  }
  return mix(c, uInk, (1.0 - smoothstep(0.0, 1.4 / R, abs(r - 1.0))) * 0.22 * uTone);
}

// Tree: a soft ground for the low-poly tree the mesh pass draws, with a shadow where it stands.
vec3 faceTree(vec2 p, vec3 ground) {
  vec3 c = mix(ground + mix(vec3(0.015, 0.03, 0.022), vec3(0.0, 0.012, 0.004), uTone), ground, smoothstep(60.0, 560.0, p.y));
  float shadow = exp(-pow((p.x - 360.0) / 140.0, 2.0) - pow((p.y - 552.0) / 14.0, 2.0));
  return c * (1.0 - shadow * mix(0.55, 0.16, uTone));
}
`;
