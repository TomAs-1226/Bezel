/**
 * Damped springs, solved in closed form.
 *
 * A spring is plain data, `{ mass, stiffness, damping }`. Every platform's way of describing a
 * spring converts to and from that form, so a spring tuned in one vocabulary can be read in any
 * other. Time is in seconds and velocity in units per second. Motion runs from `from` to `to`.
 */

const TAU = 2 * Math.PI;
const CRITICAL_TOLERANCE = 1e-6;

/** Validates and freezes a physical spring. */
export function spring({ mass = 1, stiffness, damping }) {
  const finite = [mass, stiffness, damping].every(Number.isFinite);
  if (!finite || mass <= 0 || stiffness <= 0 || damping < 0) {
    throw new RangeError(`invalid spring: mass ${mass}, stiffness ${stiffness}, damping ${damping}`);
  }
  return Object.freeze({ mass, stiffness, damping });
}

/** ω₀ in radians per second. */
export const naturalFrequency = (s) => Math.sqrt(s.stiffness / s.mass);

/** ζ: 1 is critically damped, below 1 overshoots, above 1 creeps. */
export const dampingRatio = (s) => s.damping / (2 * Math.sqrt(s.stiffness * s.mass));

/** The undamped period 2π/ω₀. This is what SwiftUI calls `duration` and the older API `response`. */
export const period = (s) => TAU / naturalFrequency(s);

// ---------------------------------------------------------------------------------------------
// Parameterizations. Each pair converts one vocabulary to the physical form and back.
// ---------------------------------------------------------------------------------------------

/**
 * Duration and bounce: SwiftUI `Spring(duration:bounce:)`, UIKit `animate(springDuration:bounce:)`,
 * `CASpringAnimation(perceptualDuration:bounce:)`. Bounce runs from -1 (overdamped) to 1 (undamped).
 *
 * Checked against Apple's documented example: duration 0.5, bounce 0.3 gives stiffness 157.9 and
 * damping 17.6. For negative bounce this uses 4π / (duration · (1 + bounce)), the forum-corrected
 * form. The slide in WWDC23 "Animate with springs" shows 4π / (duration + 4π · bounce), which
 * diverges at bounce = -duration/4π and cannot be right.
 */
export function fromDurationBounce({ duration = 0.5, bounce = 0 } = {}) {
  if (!(duration > 0) || !(bounce > -1) || !(bounce <= 1)) {
    throw new RangeError(`invalid duration/bounce: ${duration}, ${bounce}`);
  }
  const stiffness = (TAU / duration) ** 2;
  const damping = bounce >= 0
    ? (4 * Math.PI * (1 - bounce)) / duration
    : (4 * Math.PI) / (duration * (1 + bounce));
  return spring({ mass: 1, stiffness, damping });
}

export function toDurationBounce(s) {
  const zeta = dampingRatio(s);
  return { duration: period(s), bounce: zeta <= 1 ? 1 - zeta : 1 / zeta - 1 };
}

/**
 * Response and damping fraction: SwiftUI `.spring(response:dampingFraction:)`, whose documented
 * defaults are 0.5 and 0.825. WWDC18's UIKit examples use the same pair.
 */
export function fromResponse({ response = 0.5, dampingFraction = 0.825 } = {}) {
  if (!(response > 0) || !(dampingFraction >= 0)) {
    throw new RangeError(`invalid response/dampingFraction: ${response}, ${dampingFraction}`);
  }
  return spring({
    mass: 1,
    stiffness: (TAU / response) ** 2,
    damping: (4 * Math.PI * dampingFraction) / response,
  });
}

export function toResponse(s) {
  return { response: period(s), dampingFraction: dampingRatio(s) };
}

/** Stiffness and damping ratio with unit mass: Android `SpringForce`, Compose `spring()`. */
export function fromStiffnessRatio({ stiffness, dampingRatio: zeta } = {}) {
  if (!(stiffness > 0) || !(zeta >= 0)) {
    throw new RangeError(`invalid stiffness/dampingRatio: ${stiffness}, ${zeta}`);
  }
  return spring({ mass: 1, stiffness, damping: 2 * zeta * Math.sqrt(stiffness) });
}

/** Android and Compose have no mass, so the equivalent spring divides it out. */
export function toStiffnessRatio(s) {
  return { stiffness: s.stiffness / s.mass, dampingRatio: dampingRatio(s) };
}

/** Tension and friction with mass: react-spring, Rebound. Tension is stiffness, friction is damping. */
export function fromTensionFriction({ tension, friction, mass = 1 } = {}) {
  return spring({ mass, stiffness: tension, damping: friction });
}

export function toTensionFriction(s) {
  return { mass: s.mass, tension: s.stiffness, friction: s.damping };
}

/** Mass, stiffness and damping as given: CASpringAnimation, `interpolatingSpring`, Flutter, Motion. */
export const fromPhysical = (p) => spring(p);
export const toPhysical = (s) => ({ mass: s.mass, stiffness: s.stiffness, damping: s.damping });

/** The same motion with mass 1, for vocabularies that have no mass. */
const unitMass = (s) => spring({ mass: 1, stiffness: s.stiffness / s.mass, damping: s.damping / s.mass });

/**
 * Tension and friction on Origami Studio's scale. Rebound converts these to physical tension and
 * friction before simulating (OrigamiValueConverter).
 */
export function fromOrigami({ origamiTension = 40, origamiFriction = 7 } = {}) {
  return fromTensionFriction({
    tension: (origamiTension - 30) * 3.62 + 194,
    friction: (origamiFriction - 8) * 3 + 25,
  });
}

export function toOrigami(s) {
  const { tension, friction } = toTensionFriction(unitMass(s));
  return { origamiTension: (tension - 194) / 3.62 + 30, origamiFriction: (friction - 25) / 3 + 8 };
}

/**
 * Bounciness and speed: Origami's Pop Animation and Facebook POP. Converted as Rebound's
 * BouncyConversion does, with a fitted friction curve, then through the Origami scale. There is
 * no closed-form inverse.
 */
export function fromBounciness({ bounciness = 5, speed = 10 } = {}) {
  if (!Number.isFinite(bounciness) || !Number.isFinite(speed)) {
    throw new RangeError(`invalid bounciness/speed: ${bounciness}, ${speed}`);
  }
  const b = 0.8 * (bounciness / 1.7 / 20);
  const t = 0.5 + (speed / 1.7 / 20) * 199.5;
  let noBounce;
  if (t <= 18) noBounce = 0.0007 * t ** 3 - 0.031 * t ** 2 + 0.64 * t + 1.28;
  else if (t <= 44) noBounce = 0.000044 * t ** 3 - 0.006 * t ** 2 + 0.36 * t + 2;
  else noBounce = 0.00000045 * t ** 3 - 0.000332 * t ** 2 + 0.1078 * t + 5.84;
  const q = 2 * b - b * b;
  return fromOrigami({ origamiTension: t, origamiFriction: q * 0.01 + (1 - q) * noBounce });
}

/**
 * Motion's `visualDuration` and `bounce`: the time the move takes to look finished, with any
 * bounce after it. Per Motion's source, stiffness is (2π / 1.2v)² and the damping ratio is
 * 1 − bounce clamped to [0.05, 1].
 */
export function fromVisualDuration({ visualDuration = 0.3, bounce = 0 } = {}) {
  if (!(visualDuration > 0) || !Number.isFinite(bounce)) {
    throw new RangeError(`invalid visualDuration/bounce: ${visualDuration}, ${bounce}`);
  }
  const root = TAU / (visualDuration * 1.2);
  const zeta = Math.min(1, Math.max(0.05, 1 - bounce));
  return spring({ mass: 1, stiffness: root * root, damping: 2 * zeta * root });
}

/** The Motion form, or null for springs Motion's clamp can't express (overdamped, or ζ < 0.05). */
export function toVisualDuration(s) {
  const zeta = dampingRatio(s);
  if (zeta < 0.05 - 1e-9 || zeta > 1 + 1e-9) return null;
  return { visualDuration: period(s) / 1.2, bounce: 1 - Math.min(zeta, 1) };
}

/**
 * The vocabularies, described for tools that build input forms. `fields` are listed in the order
 * the platform's API takes them.
 */
export const parameterizations = Object.freeze({
  durationBounce: {
    label: 'Duration · bounce',
    apis: ['SwiftUI Spring(duration:bounce:)', 'UIKit animate(springDuration:bounce:)', 'CASpringAnimation(perceptualDuration:bounce:)'],
    fields: [
      { key: 'duration', label: 'Duration', unit: 's', min: 0.05, max: 3, step: 0.01, default: 0.5 },
      { key: 'bounce', label: 'Bounce', unit: '', min: -0.9, max: 0.95, step: 0.01, default: 0 },
    ],
    from: fromDurationBounce,
    to: toDurationBounce,
  },
  response: {
    label: 'Response · damping fraction',
    apis: ['SwiftUI .spring(response:dampingFraction:)'],
    fields: [
      { key: 'response', label: 'Response', unit: 's', min: 0.05, max: 3, step: 0.01, default: 0.5 },
      { key: 'dampingFraction', label: 'Damping fraction', unit: '', min: 0.05, max: 3, step: 0.01, default: 0.825 },
    ],
    from: fromResponse,
    to: toResponse,
  },
  stiffnessRatio: {
    label: 'Stiffness · damping ratio',
    apis: ['Android SpringForce', 'Compose spring()'],
    fields: [
      { key: 'stiffness', label: 'Stiffness', unit: '', min: 1, max: 10000, step: 1, default: 1500 },
      { key: 'dampingRatio', label: 'Damping ratio', unit: '', min: 0.05, max: 3, step: 0.01, default: 1 },
    ],
    from: fromStiffnessRatio,
    to: toStiffnessRatio,
  },
  tensionFriction: {
    label: 'Tension · friction',
    apis: ['react-spring', 'Rebound'],
    fields: [
      { key: 'tension', label: 'Tension', unit: '', min: 1, max: 1000, step: 1, default: 170 },
      { key: 'friction', label: 'Friction', unit: '', min: 0, max: 200, step: 0.5, default: 26 },
      { key: 'mass', label: 'Mass', unit: '', min: 0.1, max: 10, step: 0.1, default: 1 },
    ],
    from: fromTensionFriction,
    to: toTensionFriction,
  },
  physical: {
    label: 'Mass · stiffness · damping',
    apis: ['CASpringAnimation', 'SwiftUI interpolatingSpring', 'Flutter SpringDescription', 'Motion'],
    fields: [
      { key: 'mass', label: 'Mass', unit: '', min: 0.1, max: 10, step: 0.1, default: 1 },
      { key: 'stiffness', label: 'Stiffness', unit: '', min: 1, max: 10000, step: 1, default: 100 },
      { key: 'damping', label: 'Damping', unit: '', min: 0, max: 200, step: 0.5, default: 10 },
    ],
    from: fromPhysical,
    to: toPhysical,
  },
  visualDuration: {
    label: 'Visual duration · bounce',
    apis: ['Motion { visualDuration, bounce }'],
    fields: [
      { key: 'visualDuration', label: 'Visual duration', unit: 's', min: 0.05, max: 3, step: 0.01, default: 0.3 },
      { key: 'bounce', label: 'Bounce', unit: '', min: 0, max: 0.95, step: 0.01, default: 0 },
    ],
    from: fromVisualDuration,
    to: toVisualDuration,
  },
  origami: {
    label: 'Origami tension · friction',
    apis: ['Origami Studio Spring Animation', 'Rebound fromOrigamiTensionAndFriction'],
    fields: [
      { key: 'origamiTension', label: 'Tension', unit: '', min: 1, max: 200, step: 0.5, default: 40 },
      { key: 'origamiFriction', label: 'Friction', unit: '', min: 0, max: 60, step: 0.5, default: 7 },
    ],
    from: fromOrigami,
    to: toOrigami,
  },
  bounciness: {
    label: 'Bounciness · speed',
    apis: ['Origami Pop Animation', 'Facebook POP', 'Rebound fromBouncinessAndSpeed'],
    fields: [
      { key: 'bounciness', label: 'Bounciness', unit: '', min: 0, max: 20, step: 0.5, default: 5 },
      { key: 'speed', label: 'Speed', unit: '', min: 0.5, max: 20, step: 0.5, default: 10 },
    ],
    from: fromBounciness,
    to: () => null,
  },
});

// ---------------------------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------------------------

/**
 * Solves the spring for a start value, target and initial velocity. Returns functions of time
 * that are exact at any t, so an animation can sample whatever frame it lands on.
 */
export function solve(s, { from = 0, to = 1, velocity = 0 } = {}) {
  const w0 = naturalFrequency(s);
  const zeta = dampingRatio(s);
  const x0 = from - to;
  const v0 = velocity;
  let displacement;
  let speed;

  if (Math.abs(zeta - 1) < CRITICAL_TOLERANCE) {
    const b = v0 + w0 * x0;
    displacement = (t) => Math.exp(-w0 * t) * (x0 + b * t);
    speed = (t) => Math.exp(-w0 * t) * (v0 - w0 * b * t);
  } else if (zeta < 1) {
    const wd = w0 * Math.sqrt(1 - zeta * zeta);
    const b = (v0 + zeta * w0 * x0) / wd;
    const decay = zeta * w0;
    displacement = (t) => Math.exp(-decay * t) * (x0 * Math.cos(wd * t) + b * Math.sin(wd * t));
    speed = (t) => Math.exp(-decay * t)
      * ((b * wd - decay * x0) * Math.cos(wd * t) - (x0 * wd + decay * b) * Math.sin(wd * t));
  } else {
    const root = w0 * Math.sqrt(zeta * zeta - 1);
    const r1 = -zeta * w0 + root;
    const r2 = -zeta * w0 - root;
    const c2 = (v0 - r1 * x0) / (r2 - r1);
    const c1 = x0 - c2;
    displacement = (t) => c1 * Math.exp(r1 * t) + c2 * Math.exp(r2 * t);
    speed = (t) => c1 * r1 * Math.exp(r1 * t) + c2 * r2 * Math.exp(r2 * t);
  }

  return {
    value: (t) => to + displacement(t),
    velocity: speed,
    state: (t) => ({ value: to + displacement(t), velocity: speed(t) }),
  };
}

/**
 * Time until the motion stays within `epsilon` of the travel distance.
 *
 * Uses the spring's energy, √(x² + (v/ω₀)²), which never increases, so the first time it drops
 * below the limit is also the last. A position-only test can report a spring as settled while it
 * is still passing through the target at speed. With no travel, a velocity kick is measured
 * against the amplitude it produces, |v|/ω₀.
 */
export function settlingTime(s, { from = 0, to = 1, velocity = 0, epsilon = 0.001 } = {}) {
  const w0 = naturalFrequency(s);
  const scale = Math.abs(to - from) || Math.abs(velocity) / w0;
  if (scale === 0) return 0;
  if (s.damping === 0) return Infinity;

  const motion = solve(s, { from, to, velocity });
  const limit = epsilon * scale;
  const amplitude = (t) => Math.hypot(motion.value(t) - to, motion.velocity(t) / w0);
  if (amplitude(0) <= limit) return 0;

  let lo = 0;
  let hi = 1 / w0;
  while (amplitude(hi) > limit) {
    lo = hi;
    hi *= 2;
    if (hi > 3600) return Infinity;
  }
  for (let i = 0; i < 60 && hi - lo > 1e-7; i++) {
    const mid = (lo + hi) / 2;
    if (amplitude(mid) > limit) lo = mid;
    else hi = mid;
  }
  return hi;
}

/**
 * Everything a designer reads off a spring: the numbers each platform uses, plus how the motion
 * looks. Overshoot and peak time describe a move from rest; `bounces` counts extrema that exceed
 * `visible` of the travel.
 */
export function describe(s, { epsilon = 0.001, visible = 0.01 } = {}) {
  const zeta = dampingRatio(s);
  const w0 = naturalFrequency(s);
  const under = zeta < 1;
  const overshoot = under ? Math.exp((-zeta * Math.PI) / Math.sqrt(1 - zeta * zeta)) : 0;
  let bounces = 0;
  if (under) {
    if (zeta === 0) bounces = Infinity;
    else if (overshoot > visible) bounces = Math.ceil(Math.log(visible) / Math.log(overshoot)) - 1;
  }
  const { duration, bounce } = toDurationBounce(s);
  return {
    mass: s.mass,
    stiffness: s.stiffness,
    damping: s.damping,
    dampingRatio: zeta,
    naturalFrequency: w0,
    duration,
    bounce,
    response: duration,
    overshoot,
    peakTime: under ? Math.PI / (w0 * Math.sqrt(1 - zeta * zeta)) : null,
    bounces,
    settlingTime: settlingTime(s, { epsilon }),
  };
}

/**
 * An animation that can change target mid-flight. Retargeting restarts the solution from the
 * current position and velocity, which is what makes interrupted springs look continuous.
 */
export function animator(s, { value = 0, velocity = 0 } = {}) {
  let start = 0;
  let target = value;
  let motion = solve(s, { from: value, to: value, velocity });
  let current = s;

  const stateAt = (t) => motion.state(Math.max(0, t - start));

  return {
    get target() { return target; },
    get spring() { return current; },
    state: stateAt,
    /** Points the spring at a new target at time `t`, keeping position and velocity. */
    retarget(t, to, { spring: next = current, velocity: override } = {}) {
      const now = stateAt(t);
      current = next;
      target = to;
      start = t;
      motion = solve(current, { from: now.value, to, velocity: override ?? now.velocity });
    },
    /** Jumps to a value with a given velocity, e.g. while a finger drags it. */
    set(t, v, velocityNow = 0) {
      target = v;
      start = t;
      motion = solve(current, { from: v, to: v, velocity: velocityNow });
    },
    settled(t, epsilon = 0.001) {
      const { value: x, velocity: v } = stateAt(t);
      const w0 = naturalFrequency(current);
      return Math.hypot(x - target, v / w0) <= epsilon;
    },
  };
}
