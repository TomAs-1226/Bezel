// Bezel's motion, built on Detent (./detent is a copy of Detent's src). Springs are Detent's sourced presets
// by id, and gestures follow its patterns: press feedback, paging, momentum scrolling and snap to target.

import { animator, fromDurationBounce, fromResponse, fromStiffnessRatio, spring } from './detent/spring.js';
import { decay, nearest, project, rubberBand, rubberBandClamp } from './detent/gesture.js';
import { velocityTracker } from './detent/velocity.js';
import { springToCss } from './detent/easing.js';

export { nearest, project, rubberBand, rubberBandClamp, velocityTracker };

// Values from Detent's knowledge/presets.json.
export const PRESETS = {
  'apple-swiftui-interactive-spring': fromResponse({ response: 0.15, dampingFraction: 0.86 }),
  'apple-swiftui-snappy': fromDurationBounce({ duration: 0.5, bounce: 0.15 }),
  'apple-swiftui-smooth': fromDurationBounce({ duration: 0.5, bounce: 0 }),
  'apple-swiftui-bouncy': fromDurationBounce({ duration: 0.5, bounce: 0.3 }),
  'apple-wwdc18-pip': fromResponse({ response: 0.4, dampingFraction: 1 }),
  'apple-wwdc18-rotation': fromResponse({ response: 0.4, dampingFraction: 0.8 }),
  'm3-default-effects': fromStiffnessRatio({ stiffness: 1600, dampingRatio: 1 }),
  'm3-fast-effects': fromStiffnessRatio({ stiffness: 3800, dampingRatio: 1 }),
  'm3-expressive-fast-spatial': fromStiffnessRatio({ stiffness: 800, dampingRatio: 0.6 }),
};

// Roles. Each names the preset or pattern it comes from.
export const SPRINGS = {
  hold: PRESETS['apple-swiftui-interactive-spring'], // press held, value following a finger (press feedback pattern)
  release: PRESETS['apple-swiftui-snappy'], //        press release, lens travel, toast rise
  smooth: PRESETS['apple-swiftui-smooth'], //         things leaving: toast sinking back
  settle: PRESETS['apple-wwdc18-pip'], //             a thrown value settling on its detent (snap to target pattern)
  detent: PRESETS['apple-wwdc18-rotation'], //        a dial clicking between steps and settling after a spin
  wobble: PRESETS['apple-swiftui-bouncy'], //         the dock's droplet melting back into the bar
  effect: PRESETS['m3-default-effects'], //           colour, opacity, glass strength
  layer: PRESETS['m3-fast-effects'], //               state layers
  tick: PRESETS['m3-expressive-fast-spatial'], //     the dial's second hand stepping into place
  page: fromDurationBounce({ duration: 0.45, bounce: 0.12 }), // paging pattern: near .snappy, bounce up to 0.15
  edge: spring({ mass: 0.5, stiffness: 100, damping: 2 * 1.1 * Math.sqrt(50) }), // momentum pattern: Flutter's iOS edge spring
  light: fromResponse({ response: 0.4, dampingFraction: 1 }), // our tuning
};

let reduced = false;
export const setReducedMotion = (on) => { reduced = on; };
const calmed = (s) => {
  const critical = 2 * Math.sqrt(s.stiffness * s.mass);
  return reduced && s.damping < critical ? spring({ mass: s.mass, stiffness: s.stiffness, damping: critical }) : s;
};

const running = new Set();
const seconds = () => performance.now() / 1000;

/** Steps every running motion. The render loop calls it once per frame, before drawing. */
export function tickMotions(t) {
  for (const m of [...running]) m.tick(t);
}

/** A value driven by Detent's animator: retargetable at any moment without losing its velocity. */
export class Motion {
  constructor(value, { spring: s = SPRINGS.release, epsilon = 0.001, onChange } = {}) {
    Object.assign(this, { spring: s, epsilon, onChange, value, velocity: 0, target: value });
    this.anim = animator(s, { value });
  }

  to(target, { spring: s = this.spring, velocity } = {}) {
    const t = seconds();
    const held = !running.has(this);
    if (held) this.anim.set(t, this.value, 0);
    this.anim.retarget(t, target, { spring: calmed(s), velocity: held ? velocity ?? this.velocity : velocity });
    this.target = target;
    running.add(this);
    return this;
  }

  /** Holds the value, e.g. under a finger, remembering its velocity for the hand-off. */
  set(value, velocity = 0) {
    this.anim.set(seconds(), value, 0);
    Object.assign(this, { value, velocity, target: value });
    running.delete(this);
    this.onChange?.(value, velocity);
  }

  tick(t) {
    if (this.anim.settled(t, this.epsilon)) {
      this.set(this.target, 0);
      return;
    }
    const { value, velocity } = this.anim.state(t);
    this.value = value;
    this.velocity = velocity;
    this.onChange?.(value, velocity);
  }

  get moving() { return running.has(this); }
}

const capped = (v) => Math.max(-5000, Math.min(5000, v));

/** Momentum scrolling pattern: coast on Detent's decay at UIScrollView's rate, spring back at the edges. */
export class Coast {
  constructor(motion, bounds) {
    this.motion = motion;
    this.bounds = bounds;
    this.fling = null;
  }

  release(velocity, rate = 0.998) {
    const [min, max] = this.bounds();
    const x = this.motion.value;
    if (x < min || x > max) {
      this.motion.to(x < min ? min : max, { spring: SPRINGS.edge, velocity: capped(velocity) });
      return;
    }
    const d = decay({ from: x, velocity, rate });
    if (Math.abs(d.end - x) < 0.5) return;
    this.fling = { d, t0: seconds(), stop: d.durationUntilDistance(0.5) };
    running.add(this);
  }

  cancel() {
    this.fling = null;
    running.delete(this);
  }

  tick(t) {
    const { d, t0, stop } = this.fling;
    const [min, max] = this.bounds();
    const x = d.value(t - t0);
    const v = d.velocity(t - t0);
    if (x < min || x > max) {
      this.cancel();
      this.motion.set(x, v);
      this.motion.to(x < min ? min : max, { spring: SPRINGS.edge, velocity: capped(v) });
    } else if (t - t0 >= stop) {
      this.cancel();
      this.motion.set(d.end, 0);
    } else {
      this.motion.set(x, v);
    }
  }
}

/** Paging pattern: project the flick, then stay within one page of where the drag began. */
export function pageFor(offset, velocity, size, from, count) {
  const aim = Math.round(-(offset + project(velocity)) / size);
  return Math.max(0, Math.min(count - 1, Math.min(from + 1, Math.max(from - 1, aim))));
}

/** Snap to target pattern for a value: project with the fast rate and take the nearest detent. */
export const snapTarget = (value, velocity, detents, rate = 0.99) => nearest(value + project(velocity, rate), detents);

/** A spring as a CSS transition, through Detent's springToCss. */
export function cssSpring(s) {
  const { duration, easing } = springToCss(s, { from: 0, to: 1 });
  return { duration: `${Math.round(duration * 1000)}ms`, easing };
}
