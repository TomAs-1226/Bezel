// The tree's scene pass: a flat-shaded low-poly mesh lit by the panel's one light. Every piece of the tree
// carries the growth step it appears at and the point it grows from, so growing is a uniform on the GPU:
// pieces past the current step collapse into their pivot and swell out as the step reaches them. Fruit and
// blossoms also carry a kind and an index, so each waits for its own count. Wind is a sway in the vertex
// shader that bends the tree further the higher it goes.

/** Floats per vertex: position 3, normal 3, colour 3, pivot 3, growth step 1, kind and index 2. */
export const TREE_FLOATS = 15;
export const KIND = { wood: 0, leaf: 1, fruit: 2, blossom: 3 };

const VS = `#version 300 es
in vec3 aPos; in vec3 aNormal; in vec3 aColor; in vec3 aPivot; in float aOrder; in vec2 aKind;
uniform mat4 uMvp; uniform mat3 uTurn; uniform float uGrowth; uniform vec3 uLight; uniform vec2 uNew;
uniform vec2 uWind; uniform vec2 uCount; uniform vec4 uGlow;
out vec3 vColor;
void main() {
  // a piece exists once growth reaches its step; fruit (kind 2) and blossoms (kind 3) also wait for their count
  float s = smoothstep(aOrder, aOrder + 1.4, uGrowth);
  if (aKind.x > 1.5) s *= smoothstep(aKind.y, aKind.y + 1.0, aKind.x > 2.5 ? uCount.y : uCount.x);
  vec3 p = aPivot + (aPos - aPivot) * s;
  // wind bends the tree in proportion to height. The field depends only on where a vertex is, so joints never
  // open; it is pushed in view space, so it blows the same way however the tree is turned. Leaves, fruit and
  // blossoms flutter a little on top of the sway, each cluster as one piece.
  float h = max(p.y, 0.0);
  float sway = sin(uWind.x * 0.9 + p.x * 0.35) * 0.7 + sin(uWind.x * 2.3 + dot(p, vec3(0.8, 0.5, 0.6))) * 0.3;
  float flutter = step(0.5, aKind.x) * sin(uWind.x * 4.6 + dot(aPivot, vec3(5.7, 2.1, 3.9))) * 0.006;
  vec3 push = vec3(sway * 0.016 * h + flutter, flutter * 0.6, sin(uWind.x * 0.6 + p.x * 0.3) * 0.005 * h);
  p += transpose(uTurn) * (push * uWind.y);
  gl_Position = uMvp * vec4(p, 1.0);
  float lit = 0.46 + 0.54 * max(dot(normalize(uTurn * aNormal), uLight), 0.0);
  // fruit and blossoms keep some light even in the crown's shade, so they stand out from the leaves
  lit = max(lit, step(1.5, aKind.x) * (aKind.x > 2.5 ? 0.78 : 0.84));
  // the pieces grown on the day in view glow, so you can find them; fruit and blossoms never do
  float fresh = step(uNew.x, aOrder) * step(aOrder, uNew.y) * step(aKind.x, 1.5);
  vColor = aColor * lit + fresh * uGlow.rgb * uGlow.a;
}`;
const FS = `#version 300 es
precision highp float;
in vec3 vColor; out vec4 o;
void main() { o = vec4(vColor, 1.0); }`;

const NONE = [1e5, 1e5];
const STILL = [0, 0];
const GLOW = [0.1, 0.13, 0.05, 1];

const multiply = (a, b) => {
  const o = new Float32Array(16);
  for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
    let sum = 0;
    for (let k = 0; k < 4; k++) sum += a[k * 4 + r] * b[c * 4 + k];
    o[c * 4 + r] = sum;
  }
  return o;
};
const perspective = (fov, aspect, near, far) => {
  const f = 1 / Math.tan(fov / 2);
  return new Float32Array([f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (far + near) / (near - far), -1, 0, 0, (2 * far * near) / (near - far), 0]);
};
const turn = (yaw, pitch) => {
  const cy = Math.cos(yaw), sy = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch);
  // rotate about y by yaw, then about x by pitch; column-major
  return new Float32Array([cy, sy * sp, -sy * cp, 0, 0, cp, sp, 0, sy, -cy * sp, cy * cp, 0, 0, 0, 0, 1]);
};

export function createTreePass(renderer) {
  const { gl } = renderer;
  const prog = renderer.program(VS, FS, ['aPos', 'aNormal', 'aColor', 'aPivot', 'aOrder', 'aKind'],
    ['uMvp', 'uTurn', 'uGrowth', 'uLight', 'uNew', 'uWind', 'uCount', 'uGlow']);
  const vao = gl.createVertexArray();
  const buffer = gl.createBuffer();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
  [3, 3, 3, 3, 1, 2].reduce((offset, size, k) => {
    gl.enableVertexAttribArray(k);
    gl.vertexAttribPointer(k, size, gl.FLOAT, false, TREE_FLOATS * 4, offset);
    return offset + size * 4;
  }, 0);
  let count = 0;

  return {
    setMesh(data) {
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
      count = Math.floor(data.length / TREE_FLOATS);
    },
    /** Views are { rect, clip?, yaw, pitch, growth, light, fresh?, glow?, wind?: [time, strength], counts?: [fruit, blossoms] }. */
    draw({ s, dpr, H }) {
      const face = s.treeFace && s.treeFaceParams ? { ...s.treeFaceParams, rect: s.treeFace } : null;
      const views = [s.tree, face].filter(Boolean);
      if (!count || !views.length) return;
      gl.useProgram(prog.p);
      gl.bindVertexArray(vao);
      gl.enable(gl.DEPTH_TEST);
      gl.enable(gl.SCISSOR_TEST);
      for (const view of views) {
        const { rect, clip = rect } = view;
        const x = Math.floor(rect.x * dpr), y = Math.floor(H - (rect.y + rect.h) * dpr);
        const w = Math.max(1, Math.ceil(rect.w * dpr)), h = Math.max(1, Math.ceil(rect.h * dpr));
        gl.viewport(x, y, w, h);
        gl.scissor(Math.floor(clip.x * dpr), Math.floor(H - (clip.y + clip.h) * dpr), Math.max(1, Math.ceil(clip.w * dpr)), Math.max(1, Math.ceil(clip.h * dpr)));
        gl.clear(gl.DEPTH_BUFFER_BIT);
        const rotation = turn(view.yaw, view.pitch);
        const camera = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1.18, -5.7, 1]);
        const mvp = multiply(perspective(0.62, rect.w / rect.h, 0.5, 20), multiply(camera, rotation));
        gl.uniformMatrix4fv(prog.u.uMvp, false, mvp);
        gl.uniformMatrix3fv(prog.u.uTurn, false, [rotation[0], rotation[1], rotation[2], rotation[4], rotation[5], rotation[6], rotation[8], rotation[9], rotation[10]]);
        gl.uniform1f(prog.u.uGrowth, view.growth);
        gl.uniform3fv(prog.u.uLight, view.light);
        gl.uniform2fv(prog.u.uNew, view.fresh || NONE);
        gl.uniform2fv(prog.u.uWind, view.wind || STILL);
        gl.uniform2fv(prog.u.uCount, view.counts || STILL);
        gl.uniform4fv(prog.u.uGlow, view.glow || GLOW);
        gl.drawArrays(gl.TRIANGLES, 0, count);
      }
    },
  };
}
