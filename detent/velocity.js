/**
 * Release velocity from a stream of pointer samples.
 *
 * A least-squares polynomial fit over a short window, the approach Android's VelocityTracker
 * takes. Differencing the last two samples instead amplifies jitter and reports whatever the final
 * event happened to do. Times are in seconds.
 */

export function velocityTracker({ horizon = 0.1, assumeStopped = 0.04, historySize = 20, degree = 2 } = {}) {
  const samples = [];

  return {
    add(time, value) {
      samples.push([time, value]);
      if (samples.length > historySize) samples.shift();
    },
    reset() {
      samples.length = 0;
    },
    /** Velocity in units per second. Pass `now` to report 0 once the pointer has rested. */
    velocity(now) {
      if (samples.length < 2) return 0;
      const [newest] = samples[samples.length - 1];
      if (now !== undefined && now - newest > assumeStopped) return 0;

      const window = [samples[samples.length - 1]];
      for (let i = samples.length - 2; i >= 0; i--) {
        const [t] = samples[i];
        if (newest - t > horizon || window[window.length - 1][0] - t > assumeStopped) break;
        window.push(samples[i]);
      }
      if (window.length < 2) return 0;
      return fitSlope(window, newest, Math.min(degree, window.length - 1));
    },
  };
}

/** Slope at `origin` of a least-squares polynomial of the given degree (1 or 2). */
function fitSlope(points, origin, degree) {
  const n = degree + 1;
  const ata = Array.from({ length: n }, () => new Array(n).fill(0));
  const atb = new Array(n).fill(0);
  for (const [t, y] of points) {
    const tau = t - origin;
    const powers = [1, tau, tau * tau].slice(0, n);
    for (let r = 0; r < n; r++) {
      atb[r] += powers[r] * y;
      for (let c = 0; c < n; c++) ata[r][c] += powers[r] * powers[c];
    }
  }
  const coefficients = solveLinear(ata, atb);
  if (!coefficients) {
    const [t0, y0] = points[points.length - 1];
    const [t1, y1] = points[0];
    return t1 === t0 ? 0 : (y1 - y0) / (t1 - t0);
  }
  return coefficients[1];
}

/** Gaussian elimination with partial pivoting; null when the system is singular. */
function solveLinear(a, b) {
  const n = b.length;
  const m = a.map((row, i) => [...row, b[i]]);
  for (let col = 0; col < n; col++) {
    let pivot = col;
    for (let r = col + 1; r < n; r++) if (Math.abs(m[r][col]) > Math.abs(m[pivot][col])) pivot = r;
    if (Math.abs(m[pivot][col]) < 1e-12) return null;
    [m[col], m[pivot]] = [m[pivot], m[col]];
    for (let r = col + 1; r < n; r++) {
      const f = m[r][col] / m[col][col];
      for (let c = col; c <= n; c++) m[r][c] -= f * m[col][c];
    }
  }
  const x = new Array(n).fill(0);
  for (let r = n - 1; r >= 0; r--) {
    let sum = m[r][n];
    for (let c = r + 1; c < n; c++) sum -= m[r][c] * x[c];
    x[r] = sum / m[r][r];
  }
  return x;
}
