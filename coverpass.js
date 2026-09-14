// Cover Flow's scene pass: album covers as real perspective quads with mirrored reflections, drawn into the
// scene target after the page so the glass above can still bend them. Vertices arrive projected: clip-space
// position scaled by w, so the GPU's perspective divide keeps each cover's texture straight. A cover turned past
// 90° shows its back: a tonal card that takes a little of its own cover's colour.

/** Floats per vertex: clip position 4, then atlas u and v, how much of the cover shows, and how much is its back. */
export const COVER_FLOATS = 8;

const VS = `#version 300 es
in vec4 aPos; in vec4 aTex;
out vec4 vTex;
void main() { vTex = aTex; gl_Position = aPos; }`;

// vTex.z is how much of the cover shows: 1 in front, less for covers turned away, fading for reflections.
// vTex.w is how much of it is the back: 0 or 1 when it turns, anything between for the calm version's cross-fade.
const FS = `#version 300 es
precision highp float;
in vec4 vTex; out vec4 o;
uniform sampler2D uAtlas; uniform vec3 uGround; uniform vec3 uCard; uniform vec3 uCells;
void main() {
  vec3 front = texture(uAtlas, vTex.xy).rgb;
  // the cover's average colour is one texel of the mip level where each cell is a single texel
  vec2 cell = (floor(clamp(vTex.xy, 0.0, 0.9999) * uCells.xy) + 0.5) / uCells.xy;
  vec3 back = mix(uCard, textureLod(uAtlas, cell, uCells.z).rgb, 0.16);
  o = vec4(mix(uGround, mix(front, back, vTex.w), vTex.z), 1.0);
}`;

export function createCoverPass(renderer) {
  const { gl } = renderer;
  const prog = renderer.program(VS, FS, ['aPos', 'aTex'], ['uAtlas', 'uGround', 'uCard', 'uCells']);
  const vao = gl.createVertexArray();
  const buffer = gl.createBuffer();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 4, gl.FLOAT, false, COVER_FLOATS * 4, 0);
  gl.enableVertexAttribArray(1);
  gl.vertexAttribPointer(1, 4, gl.FLOAT, false, COVER_FLOATS * 4, 16);
  const atlas = renderer.texture(true);
  let ready = false;
  let cells = [4, 2, 9];

  return {
    /** The atlas canvas, `across` × `down` covers. */
    setAtlas(canvas, across = 4, down = 2) {
      gl.bindTexture(gl.TEXTURE_2D, atlas);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
      gl.generateMipmap(gl.TEXTURE_2D);
      cells = [across, down, Math.max(0, Math.log2(canvas.width / across))];
      ready = true;
    },
    draw({ s, dpr, H }) {
      const c = s.covers;
      if (!ready || !c?.vertices?.length) return;
      gl.useProgram(prog.p);
      gl.bindVertexArray(vao);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.bufferData(gl.ARRAY_BUFFER, c.vertices, gl.DYNAMIC_DRAW);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, atlas);
      gl.uniform1i(prog.u.uAtlas, 0);
      gl.uniform3fv(prog.u.uGround, c.ground || s.tokens.ground);
      gl.uniform3fv(prog.u.uCard, c.card || s.tokens.solid || s.tokens.ground);
      gl.uniform3f(prog.u.uCells, cells[0], cells[1], cells[2]);
      if (c.clip) {
        gl.enable(gl.SCISSOR_TEST);
        gl.scissor(Math.floor(c.clip.x * dpr), Math.floor(H - (c.clip.y + c.clip.h) * dpr), Math.ceil(c.clip.w * dpr), Math.ceil(c.clip.h * dpr));
      }
      gl.drawArrays(gl.TRIANGLES, 0, Math.floor(c.vertices.length / COVER_FLOATS));
    },
  };
}
