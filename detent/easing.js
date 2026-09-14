/**
 * Curves: cubic Béziers and CSS `linear()` easing.
 *
 * Springs are the right model for anything a person touches, but some targets (CSS transitions,
 * design tools, older APIs) only take a curve and a duration. `springToCss` samples a spring into
 * a `linear()` easing that stays within a stated error of the real motion.
 */

import { settlingTime, solve } from './spring.js';

/** The CSS keyword curves, from the CSS Easing Functions spec. */
export const cssKeywords = Object.freeze({
  ease: [0.25, 0.1, 0.25, 1],
  'ease-in': [0.42, 0, 1, 1],
  'ease-out': [0, 0, 0.58, 1],
  'ease-in-out': [0.42, 0, 0.58, 1],
});

/** Evaluates `cubic-bezier(x1, y1, x2, y2)` at progress x, solving x(t) by Newton then bisection. */
export function cubicBezier(x1, y1, x2, y2) {
  if (x1 < 0 || x1 > 1 || x2 < 0 || x2 > 1) throw new RangeError('cubic-bezier x values must be in [0, 1]');
  const cx = 3 * x1;
  const bx = 3 * (x2 - x1) - cx;
  const ax = 1 - cx - bx;
  const cy = 3 * y1;
  const by = 3 * (y2 - y1) - cy;
  const ay = 1 - cy - by;
  const sampleX = (t) => ((ax * t + bx) * t + cx) * t;
  const sampleY = (t) => ((ay * t + by) * t + cy) * t;
  const slopeX = (t) => (3 * ax * t + 2 * bx) * t + cx;

  const solveT = (x) => {
    let t = x;
    for (let i = 0; i < 8; i++) {
      const error = sampleX(t) - x;
      if (Math.abs(error) < 1e-9) return t;
      const d = slopeX(t);
      if (Math.abs(d) < 1e-7) break;
      t -= error / d;
    }
    let lo = 0;
    let hi = 1;
    t = x;
    for (let i = 0; i < 64; i++) {
      const v = sampleX(t);
      if (Math.abs(v - x) < 1e-9) return t;
      if (v < x) lo = t;
      else hi = t;
      t = (lo + hi) / 2;
    }
    return t;
  };

  return (x) => {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return sampleY(solveT(x));
  };
}

/**
 * Ramer–Douglas–Peucker on vertical error: a point is dropped only if the straight line between
 * its neighbours stays within `tolerance` of it, which is the error CSS will actually show.
 */
function simplify(points, tolerance) {
  const keep = new Uint8Array(points.length);
  keep[0] = 1;
  keep[points.length - 1] = 1;
  const stack = [[0, points.length - 1]];
  while (stack.length) {
    const [a, b] = stack.pop();
    const [xa, ya] = points[a];
    const [xb, yb] = points[b];
    let worst = 0;
    let index = -1;
    for (let i = a + 1; i < b; i++) {
      const [x, y] = points[i];
      const error = Math.abs(y - (ya + ((yb - ya) * (x - xa)) / (xb - xa)));
      if (error > worst) {
        worst = error;
        index = i;
      }
    }
    if (index !== -1 && worst > tolerance) {
      keep[index] = 1;
      stack.push([a, index], [index, b]);
    }
  }
  return points.filter((_, i) => keep[i]);
}

const trim = (value, digits) => {
  const rounded = Number(value.toFixed(digits));
  return Object.is(rounded, -0) ? '0' : String(rounded);
};

/**
 * Samples any easing function on [0, 1] into a CSS `linear()` string. The tolerance is loosened
 * until the result fits in `maxPoints` stops.
 */
export function toLinear(fn, { tolerance = 0.002, samples = 1000, maxPoints = 200 } = {}) {
  const points = [];
  for (let i = 0; i <= samples; i++) {
    const x = i / samples;
    points.push([x, fn(x)]);
  }
  let tol = tolerance;
  let kept = simplify(points, tol);
  while (kept.length > maxPoints) {
    tol *= 1.5;
    kept = simplify(points, tol);
  }
  const last = kept.length - 1;
  const stops = kept.map(([x, y], i) => (i === 0 || i === last ? trim(y, 4) : `${trim(y, 4)} ${trim(x * 100, 2)}%`));
  return `linear(${stops.join(', ')})`;
}

/** Parses a `linear()` easing into a function, filling in missing positions as the spec does. */
export function parseLinear(text) {
  const inner = /^\s*linear\((.*)\)\s*$/s.exec(text);
  if (!inner) throw new SyntaxError(`not a linear() easing: ${text}`);
  const stops = [];
  for (const part of inner[1].split(',')) {
    const [value, ...positions] = part.trim().split(/\s+/);
    if (positions.length === 0) stops.push({ y: Number(value), x: null });
    for (const p of positions) stops.push({ y: Number(value), x: Number(p.replace('%', '')) / 100 });
  }
  if (stops[0].x === null) stops[0].x = 0;
  if (stops[stops.length - 1].x === null) stops[stops.length - 1].x = Math.max(1, ...stops.map((s) => s.x ?? 0));
  let floor = 0;
  for (const s of stops) {
    if (s.x !== null) {
      s.x = Math.max(s.x, floor);
      floor = s.x;
    }
  }
  for (let i = 0; i < stops.length; i++) {
    if (stops[i].x !== null) continue;
    let j = i;
    while (stops[j].x === null) j++;
    const a = stops[i - 1].x;
    const b = stops[j].x;
    for (let k = i; k < j; k++) stops[k].x = a + ((b - a) * (k - i + 1)) / (j - i + 1);
    i = j;
  }
  return (x) => {
    if (x <= stops[0].x) return stops[0].y;
    for (let i = 1; i < stops.length; i++) {
      const a = stops[i - 1];
      const b = stops[i];
      if (x <= b.x) return b.x === a.x ? b.y : a.y + ((b.y - a.y) * (x - a.x)) / (b.x - a.x);
    }
    return stops[stops.length - 1].y;
  };
}

/**
 * A spring as a CSS duration plus `linear()` easing. The curve is progress from `from` to `to`, so
 * it overshoots past 1 where the spring does. The initial velocity is baked into its shape.
 */
export function springToCss(s, { from = 0, to = 1, velocity = 0, epsilon = 0.001, tolerance = 0.002, maxPoints = 200 } = {}) {
  if (from === to) throw new RangeError('a CSS easing needs distinct from and to values');
  const duration = settlingTime(s, { from, to, velocity, epsilon });
  if (!Number.isFinite(duration)) throw new RangeError('this spring never settles, so it has no CSS duration');
  const motion = solve(s, { from, to, velocity });
  const progress = (u) => (u >= 1 ? 1 : (motion.value(u * duration) - from) / (to - from));
  return { duration, easing: toLinear(progress, { tolerance, maxPoints }) };
}
