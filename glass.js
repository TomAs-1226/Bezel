// Bezel's renderer. Pass A draws everything under the glass into a texture: backgrounds, Material surfaces, up
// to five rasterized text layers, the orbit sphere and any extra scene passes (Cover Flow, the tree). A pyramid
// of six low-passed half-size levels follows, so glass can blur what is behind it by any radius and the control
// center can blur the whole page. Pass B composites the glass over it. Rects arrive in CSS pixels relative to
// the canvas.

import { SCENE_FS } from './scene.glsl.js';
import { DOWN_FS, GLASS_FS, POINTS_FS, POINTS_VS, VS } from './shaders.js';

const MAX_SURF = 40;
const MAX_GLASS = 12;
const MAX_DRAWS = 5;
const MAX_OVER = 6;
const LEVELS = 6;
const SPHERE_POINTS = 1100;
/** The control center's blur at full pull: a Gaussian of this many CSS pixels. */
const BACK_BLUR = 26;

export function createRenderer(canvas) {
  const gl = canvas.getContext('webgl2', { antialias: false, alpha: false });
  if (!gl) return null;

  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
    return shader;
  };
  /** Links a program; `attributes` are bound to locations 0, 1, 2… in order. */
  const program = (vs, fs, attributes, names) => {
    const p = gl.createProgram();
    gl.attachShader(p, compile(gl.VERTEX_SHADER, vs));
    gl.attachShader(p, compile(gl.FRAGMENT_SHADER, fs));
    [].concat(attributes).forEach((name, k) => gl.bindAttribLocation(p, k, name));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    return { p, u: Object.fromEntries(names.map((n) => [n, gl.getUniformLocation(p, n)])) };
  };

  let scene, points, down, glass;
  try {
    scene = program(VS, SCENE_FS, 'p', [
      'uRes', 'uDpr', 'uUnit', 'uTime', 'uTone', 'uGrid', 'uPage', 'uScroll', 'uParallax', 'uEdge',
      'uGround', 'uInk', 'uIce', 'uSignal', 'uGlowA', 'uGlowB', 'uArt', 'uDisc', 'uRing', 'uDial', 'uDialColor',
      'uScapeF', 'uClock', 'uDigits', 'uWorld', 'uMoon', 'uSurf', 'uSurfC', 'uSurfClip', 'uSurfM', 'uSurfL', 'uSurfN', 'uSurfSplit', 'uSurfSplit2',
      'uWin', 'uWinR', 'uWinShadow', 'uWinColor',
      ...Array.from({ length: MAX_DRAWS }, (_, i) => [`uTex${i}`, `uRect${i}`]).flat(),
    ]);
    points = program(POINTS_VS, POINTS_FS, 'a', ['uRes', 'uDpr', 'uTime', 'uSize', 'uCenter', 'uColA', 'uColB', 'uAlpha', 'uTone']);
    down = program(VS, DOWN_FS, 'p', ['uSrc', 'uTexel']);
    glass = program(VS, GLASS_FS, 'p', [
      'uScene', ...Array.from({ length: LEVELS }, (_, i) => `uB${i + 1}`),
      'uRes', 'uDpr', 'uTone', 'uGlass', 'uGlassR', 'uGlassK', 'uGlassP', 'uGlassG', 'uGlassN', 'uMerge', 'uBend',
      'uFrost', 'uDisp', 'uSat', 'uClear', 'uSolidOnly', 'uLight', 'uLightColor', 'uSolid', 'uTints', 'uLens', 'uLensR', 'uLensAmt',
      'uBackLod', 'uDim', 'uOver', 'uOverC', 'uOverR', 'uOverN', 'uPing', 'uPingR', 'uPingK',
    ]);
  } catch (error) {
    console.error('[bezel] renderer failed to start', error);
    return null;
  }

  const quad = gl.createVertexArray();
  gl.bindVertexArray(quad);
  gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

  // A Fibonacci sphere: evenly spread points without clumping at the poles.
  const sphere = new Float32Array(SPHERE_POINTS * 3);
  for (let i = 0; i < SPHERE_POINTS; i++) {
    const y = 1 - ((i + 0.5) * 2) / SPHERE_POINTS;
    const r = Math.sqrt(1 - y * y);
    const theta = i * Math.PI * (3 - Math.sqrt(5));
    sphere.set([Math.cos(theta) * r, y, Math.sin(theta) * r], i * 3);
  }
  const pointsVao = gl.createVertexArray();
  gl.bindVertexArray(pointsVao);
  gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
  gl.bufferData(gl.ARRAY_BUFFER, sphere, gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);

  const texture = (mips) => {
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, mips ? gl.LINEAR_MIPMAP_LINEAR : gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return t;
  };
  const art = texture(true);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([16, 22, 44, 255]));
  gl.generateMipmap(gl.TEXTURE_2D);
  const blank = texture(false);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));

  // One texture per rasterized layer, so paging rebinds instead of re-uploading.
  const layers = new Map();
  const layerTexture = (name) => {
    if (!layers.has(name)) {
      const tex = texture(false);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
      layers.set(name, { tex, w: 1, h: 1 });
    }
    return layers.get(name);
  };

  // The scene target has a depth buffer, for passes that draw real geometry. The pyramid under it is the blur.
  const target = texture(false);
  const depth = gl.createRenderbuffer();
  const fbo = gl.createFramebuffer();
  const pyramid = Array.from({ length: LEVELS }, () => ({ tex: texture(false), fbo: gl.createFramebuffer(), w: 1, h: 1 }));
  let targetW = 0;
  let targetH = 0;
  let dpr = 1;

  function resize() {
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    const box = canvas.getBoundingClientRect();
    const w = Math.max(1, Math.round(box.width * dpr));
    const h = Math.max(1, Math.round(box.height * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    if (w !== targetW || h !== targetH) {
      targetW = w;
      targetH = h;
      gl.bindTexture(gl.TEXTURE_2D, target);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.bindRenderbuffer(gl.RENDERBUFFER, depth);
      gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT16, w, h);
      gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, target, 0);
      gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, depth);
      let lw = w;
      let lh = h;
      for (const L of pyramid) {
        lw = Math.max(1, Math.ceil(lw / 2));
        lh = Math.max(1, Math.ceil(lh / 2));
        L.w = lw;
        L.h = lh;
        gl.bindTexture(gl.TEXTURE_2D, L.tex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, lw, lh, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.bindFramebuffer(gl.FRAMEBUFFER, L.fbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, L.tex, 0);
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }
  }

  const surf = new Float32Array(MAX_SURF * 4);
  const surfC = new Float32Array(MAX_SURF * 4);
  const clip = new Float32Array(MAX_SURF * 4);
  const surfM = new Float32Array(MAX_SURF * 4);
  const lay = new Float32Array(MAX_SURF * 4);
  const glassRect = new Float32Array(MAX_GLASS * 4);
  const glassR = new Float32Array(MAX_GLASS);
  const glassK = new Float32Array(MAX_GLASS * 4);
  const glassP = new Float32Array(MAX_GLASS * 4);
  const glassG = new Float32Array(MAX_GLASS);
  const over = new Float32Array(MAX_OVER * 4);
  const overC = new Float32Array(MAX_OVER * 4);
  const overR = new Float32Array(MAX_OVER);
  const tints = new Float32Array(12);
  const passes = [];

  function frame(s) {
    resize();
    const W = canvas.width;
    const H = canvas.height;
    const T = s.tokens;

    // Pass A.
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.viewport(0, 0, W, H);
    gl.useProgram(scene.p);
    gl.bindVertexArray(quad);
    const u = scene.u;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, art);
    gl.uniform1i(u.uArt, 0);
    for (let i = 0; i < MAX_DRAWS; i++) {
      const draw = s.draws[i];
      const L = draw ? layers.get(draw.name) : null;
      gl.activeTexture(gl.TEXTURE1 + i);
      gl.bindTexture(gl.TEXTURE_2D, L ? L.tex : blank);
      gl.uniform1i(u[`uTex${i}`], 1 + i);
      gl.uniform4f(u[`uRect${i}`], L ? draw.x * dpr : -9, L ? draw.y * dpr : -9, L ? Math.max(draw.w * dpr, 1) : 1, L ? Math.max(draw.h * dpr, 1) : 1);
    }
    gl.uniform2f(u.uRes, W, H);
    gl.uniform1f(u.uDpr, dpr);
    gl.uniform1f(u.uUnit, W / 720);
    gl.uniform1f(u.uTime, s.time);
    gl.uniform1f(u.uTone, s.tone);
    gl.uniform1f(u.uGrid, s.grid);
    gl.uniform1f(u.uPage, s.page * dpr);
    gl.uniform1f(u.uScroll, s.scroll * dpr);
    gl.uniform1f(u.uParallax, s.parallax);
    gl.uniform1f(u.uEdge, s.edge ?? 1);
    gl.uniform3fv(u.uGround, T.ground);
    gl.uniform3fv(u.uInk, T.ink);
    gl.uniform3fv(u.uIce, T.ice);
    gl.uniform3fv(u.uSignal, T.signal);
    gl.uniform3fv(u.uGlowA, T.signal);
    gl.uniform3fv(u.uGlowB, T.ice);
    gl.uniform4f(u.uDisc, s.disc[0] * dpr, s.disc[1] * dpr, Math.max(s.disc[2] * dpr, 1), s.disc[3]);
    gl.uniform4f(u.uRing, s.ring[0] * dpr, s.ring[1] * dpr, s.ring[2] * dpr, s.ring[3]);
    if (s.dial) gl.uniform4f(u.uDial, s.dial[0] * dpr, s.dial[1] * dpr, s.dial[2] * dpr, s.dial[3]);
    else gl.uniform4f(u.uDial, 0, 0, 0, 0);
    gl.uniform3fv(u.uDialColor, s.dialColor || T.signal);
    gl.uniform1f(u.uScapeF, s.scapeF);
    gl.uniform4fv(u.uClock, s.clock);
    gl.uniform4iv(u.uDigits, s.digits);
    gl.uniform4fv(u.uWorld, s.world || [0, 0, 0, 0]);
    gl.uniform4fv(u.uMoon, s.moon || [0.5, 1, 0, 0]);
    const n = Math.min(s.surfaces.length, MAX_SURF);
    for (let i = 0; i < n; i++) {
      const f = s.surfaces[i];
      surf.set([f.x * dpr, f.y * dpr, f.w * dpr, f.h * dpr], i * 4);
      surfC.set([f.color[0], f.color[1], f.color[2], f.alpha ?? 1], i * 4);
      if (f.clip) clip.set([f.clip.x * dpr, f.clip.y * dpr, f.clip.w * dpr, f.clip.h * dpr], i * 4);
      surfM.set([f.r * dpr, f.clip ? f.clip.r * dpr : -1, f.layer || 0, f.ink ? 1 : 0], i * 4);
      lay.set([(f.lx || 0) * dpr, (f.ly || 0) * dpr, 0, 0], i * 4);
    }
    gl.uniform4fv(u.uSurf, surf);
    gl.uniform4fv(u.uSurfC, surfC);
    gl.uniform4fv(u.uSurfClip, clip);
    gl.uniform4fv(u.uSurfM, surfM);
    gl.uniform4fv(u.uSurfL, lay);
    gl.uniform1i(u.uSurfN, n);
    gl.uniform1i(u.uSurfSplit, Math.min(s.surfSplit ?? n, n));
    gl.uniform1i(u.uSurfSplit2, Math.min(s.surfSplit2 ?? n, n));
    const win = s.win;
    if (win) {
      gl.uniform4f(u.uWin, win.x * dpr, win.y * dpr, Math.max(win.w * dpr, 1), Math.max(win.h * dpr, 1));
      gl.uniform1f(u.uWinR, win.r * dpr);
      gl.uniform1f(u.uWinShadow, win.shadow ?? 0);
      gl.uniform3fv(u.uWinColor, win.color || T.ground);
    } else {
      gl.uniform4f(u.uWin, 0, 0, 0, 0);
    }
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    if (s.sphere && s.sphere.alpha > 0.002) {
      const sp = s.sphere;
      const p = points.u;
      gl.enable(gl.BLEND);
      if (s.tone < 0.5) gl.blendFunc(gl.ONE, gl.ONE);
      else gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.useProgram(points.p);
      gl.bindVertexArray(pointsVao);
      gl.uniform2f(p.uRes, W, H);
      gl.uniform1f(p.uDpr, dpr);
      gl.uniform1f(p.uTime, s.time);
      gl.uniform1f(p.uSize, sp.size);
      gl.uniform3f(p.uCenter, sp.x * dpr, sp.y * dpr, sp.r * dpr);
      gl.uniform3fv(p.uColA, sp.colA);
      gl.uniform3fv(p.uColB, sp.colB);
      gl.uniform1f(p.uAlpha, sp.alpha);
      gl.uniform1f(p.uTone, s.tone);
      gl.drawArrays(gl.POINTS, 0, SPHERE_POINTS);
      gl.disable(gl.BLEND);
    }

    // Extra scene passes draw real geometry into the same target, so the glass bends them too.
    for (const pass of passes) {
      pass.draw({ gl, s, dpr, W, H });
      gl.disable(gl.BLEND);
      gl.disable(gl.DEPTH_TEST);
      gl.disable(gl.SCISSOR_TEST);
      gl.viewport(0, 0, W, H);
    }

    // The blur pyramid: each level half the size of the one before, low-passed on the way down.
    gl.useProgram(down.p);
    gl.bindVertexArray(quad);
    gl.activeTexture(gl.TEXTURE0);
    gl.uniform1i(down.u.uSrc, 0);
    let src = target;
    let sw = W;
    let sh = H;
    for (const L of pyramid) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, L.fbo);
      gl.viewport(0, 0, L.w, L.h);
      gl.bindTexture(gl.TEXTURE_2D, src);
      gl.uniform2f(down.u.uTexel, 1 / sw, 1 / sh);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
      src = L.tex;
      sw = L.w;
      sh = L.h;
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    // Pass B.
    gl.viewport(0, 0, W, H);
    gl.useProgram(glass.p);
    gl.bindVertexArray(quad);
    const g = glass.u;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, target);
    gl.uniform1i(g.uScene, 0);
    pyramid.forEach((L, i) => {
      gl.activeTexture(gl.TEXTURE1 + i);
      gl.bindTexture(gl.TEXTURE_2D, L.tex);
      gl.uniform1i(g[`uB${i + 1}`], 1 + i);
    });
    gl.uniform2f(g.uRes, W, H);
    gl.uniform1f(g.uDpr, dpr);
    gl.uniform1f(g.uTone, s.tone);
    const m = Math.min(s.glass.length, MAX_GLASS);
    for (let i = 0; i < m; i++) {
      const q = s.glass[i];
      glassRect.set([q.x * dpr, q.y * dpr, q.w * dpr, q.h * dpr], i * 4);
      glassR[i] = q.r * dpr;
      glassK.set([q.strength ?? 1, q.press ?? 0, q.tint ?? 0, 0], i * 4);
      glassP.set([(q.px ?? 0) * dpr, (q.py ?? 0) * dpr, q.hue ?? 0, 0], i * 4);
      glassG[i] = q.group ?? 0;
    }
    gl.uniform4fv(g.uGlass, glassRect);
    gl.uniform1fv(g.uGlassR, glassR);
    gl.uniform4fv(g.uGlassK, glassK);
    gl.uniform4fv(g.uGlassP, glassP);
    gl.uniform1fv(g.uGlassG, glassG);
    gl.uniform1i(g.uGlassN, m);
    gl.uniform1f(g.uMerge, Math.max(s.merge * dpr, 0.01));
    gl.uniform1f(g.uBend, s.bend);
    gl.uniform1f(g.uFrost, s.frost);
    gl.uniform1f(g.uDisp, s.disp);
    gl.uniform1f(g.uSat, s.sat);
    gl.uniform1f(g.uClear, s.clear);
    gl.uniform1f(g.uSolidOnly, s.solidOnly);
    gl.uniform3f(g.uLight, s.light.x, s.light.y, s.light.z);
    gl.uniform3fv(g.uLightColor, s.light.color);
    gl.uniform3fv(g.uSolid, T.solid);
    // tint hues, by index: signal, amber, leaf, ice
    tints.set(T.signal, 0);
    tints.set(T.amber ?? T.signal, 3);
    tints.set(T.leaf ?? T.signal, 6);
    tints.set(T.ice, 9);
    gl.uniform3fv(g.uTints, tints);
    const L = s.lens;
    gl.uniform4f(g.uLens, L.x * dpr, L.y * dpr, L.w * dpr, L.h * dpr);
    gl.uniform1f(g.uLensR, L.r * dpr);
    gl.uniform1f(g.uLensAmt, L.amt);
    // what is behind the control center blurs as it is pulled, 1:1 with the pull
    const back = Math.min(Math.max(s.back ?? 0, 0), 1);
    gl.uniform1f(g.uBackLod, back > 0 ? back * Math.min(LEVELS, Math.log2(Math.max(1, (BACK_BLUR * dpr) / 0.83))) : 0);
    gl.uniform1f(g.uDim, s.dim ?? 0);
    const overlays = s.overlays ?? [];
    const o = Math.min(overlays.length, MAX_OVER);
    for (let i = 0; i < o; i++) {
      const f = overlays[i];
      over.set([f.x * dpr, f.y * dpr, f.w * dpr, f.h * dpr], i * 4);
      overC.set([f.color[0], f.color[1], f.color[2], f.alpha ?? 1], i * 4);
      overR[i] = f.r * dpr;
    }
    gl.uniform4fv(g.uOver, over);
    gl.uniform4fv(g.uOverC, overC);
    gl.uniform1fv(g.uOverR, overR);
    gl.uniform1i(g.uOverN, o);
    const P = s.ping;
    if (P && P.alpha > 0.001) {
      gl.uniform4f(g.uPing, P.x * dpr, P.y * dpr, P.w * dpr, P.h * dpr);
      gl.uniform1f(g.uPingR, P.r * dpr);
      gl.uniform3f(g.uPingK, P.grow * dpr, P.alpha, P.hue ?? 0);
    } else {
      gl.uniform3f(g.uPingK, 0, 0, 0);
    }
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  /** Uploads a rasterized layer, whole or just the region that changed. */
  function uploadLayer(name, source, region) {
    const L = layerTexture(name);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, L.tex);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    if (region && source.width === L.w && source.height === L.h) {
      const data = source.getContext('2d', { willReadFrequently: true }).getImageData(region.x, region.y, region.w, region.h);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, region.x, region.y, gl.RGBA, gl.UNSIGNED_BYTE, data);
    } else {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
      L.w = source.width;
      L.h = source.height;
    }
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
  }

  function setArt(source) {
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, art);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
    gl.generateMipmap(gl.TEXTURE_2D);
  }

  let lost = false;
  canvas.addEventListener('webglcontextlost', (event) => { event.preventDefault(); lost = true; });

  return {
    gl, program, texture, frame, setArt, uploadLayer,
    /** Adds a scene pass: { draw({ gl, s, dpr, W, H }) }, drawn after the scene, before the blur and the glass. */
    addPass: (pass) => passes.push(pass),
    get lost() { return lost; },
  };
}
