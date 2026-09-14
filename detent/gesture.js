/**
 * The physics between a finger and an animation: momentum, projection, rubber-banding, targets.
 */

/** UIScrollView.DecelerationRate raw values: the fraction of velocity kept each millisecond. */
export const decelerationRate = Object.freeze({ normal: 0.998, fast: 0.99 });

/**
 * How far a flick travels before it stops. This is the projection function Apple published with
 * "Designing Fluid Interfaces" (WWDC18): pick the target nearest `position + project(velocity)`.
 */
export function project(velocity, rate = decelerationRate.normal) {
  return ((velocity / 1000) * rate) / (1 - rate);
}

/**
 * Exponential deceleration, the continuous form of a per-millisecond rate: v(t) = v₀·rate^(1000t).
 * `end` is where it comes to rest; `durationUntil(speed)` is when it drops below that speed.
 */
export function decay({ from = 0, velocity = 0, rate = decelerationRate.normal } = {}) {
  if (!(rate > 0 && rate < 1)) throw new RangeError(`deceleration rate must be in (0, 1): ${rate}`);
  const k = 1000 * Math.log(rate);
  return {
    value: (t) => from + (velocity * (Math.exp(k * t) - 1)) / k,
    velocity: (t) => velocity * Math.exp(k * t),
    end: from - velocity / k,
    /** When the speed drops below `speed` (units per second). */
    durationUntil: (speed = 1) => (Math.abs(velocity) <= speed ? 0 : Math.log(speed / Math.abs(velocity)) / k),
    /** When the motion is within `distance` of where it will rest. */
    durationUntilDistance: (distance = 0.5) => {
      const remaining = Math.abs(velocity / k);
      return remaining <= distance ? 0 : Math.log(distance / remaining) / k;
    },
  };
}

/**
 * Rubber-banding: how far the content moves when dragged `offset` past an edge. Resistance grows
 * with distance and never lets the content travel further than `dimension`. The constant 0.55 is
 * the one widely measured on iOS.
 */
export function rubberBand(offset, dimension, constant = 0.55) {
  if (!(dimension > 0)) return 0;
  const x = Math.abs(offset);
  return (Math.sign(offset) * (constant * x * dimension)) / (dimension + constant * x);
}

/** The drag offset that produces a given rubber-banded offset, for resuming a drag from it. */
export function rubberBandInverse(shown, dimension, constant = 0.55) {
  if (!(dimension > 0)) return 0;
  const y = Math.min(Math.abs(shown), dimension * (1 - 1e-9));
  return (Math.sign(shown) * (y * dimension)) / (constant * (dimension - y));
}

/** Clamps to [min, max] with rubber-banding beyond either end. */
export function rubberBandClamp(value, min, max, dimension, constant = 0.55) {
  if (value < min) return min - rubberBand(min - value, dimension, constant);
  if (value > max) return max + rubberBand(value - max, dimension, constant);
  return value;
}

/** The candidate closest to a value. Works on numbers or on points `{ x, y }`. */
export function nearest(value, candidates) {
  let best = candidates[0];
  let bestDistance = Infinity;
  for (const c of candidates) {
    const d = typeof c === 'number' ? Math.abs(c - value) : Math.hypot(c.x - value.x, c.y - value.y);
    if (d < bestDistance) {
      bestDistance = d;
      best = c;
    }
  }
  return best;
}

/**
 * UIKit's `initialSpringVelocity` is measured in "distances per second": the gesture's speed
 * divided by the distance left to travel.
 */
export function relativeVelocity(velocity, from, to) {
  const distance = to - from;
  return distance === 0 ? 0 : velocity / distance;
}
