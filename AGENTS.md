# AGENTS.md — working in Bezel

Bezel is the specimen for Thomas's UI house style: embedded wall-panel Material structure, Apple
Liquid Glass and Detent motion. This file is the practical map for changing the code. For the
design rationale and the "why", read `DESIGN.md` here and the Detent repository's
`knowledge/bezel.md`, `knowledge/bezel-glass.md` and `knowledge/bezel-components.md`
([github.com/TomAs-1226/Detent](https://github.com/TomAs-1226/Detent)) — those are the source of truth for tokens, exact numbers and the
review checklist. Don't duplicate their numeric tables here; link to them.

There is no build step. The page is `index.html` plus ES modules loaded directly by the browser —
edit a file, reload, look at it.

## Rendering pipeline

Every frame runs in `glass.js` (`createRenderer`), fed by `panel.js`'s `frame()`:

1. **Pass A — the scene.** Draws everything *under* the glass into an offscreen framebuffer:
   per-slot backgrounds, Material surfaces (SDF rounded rects), the standby particle sphere, and
   any extra scene passes registered with `renderer.addPass` (Cover Flow's `coverpass.js`, the
   energy tree's `treepass.js`).
2. **The blur pyramid.** Six half-size levels, each low-passed on the way down (`down.p` /
   `DOWN_FS`), read back through a bicubic sample in the glass shader. This is what gives glass
   frost at any radius and drives the control center's page-wide blur.
3. **Pass B — the glass.** Composites a union of glass shapes over the blurred scene: refraction,
   dispersion, luminosity, saturation, tint, edge light, shadow, and the press glow.

**Budgets, as declared in `glass.js`** (verify here before assuming a number from memory or from
`bezel-glass.md`, which covers Apple/system-material platforms too):
- `MAX_SURF = 40` — Material surfaces per frame.
- `MAX_GLASS = 12` — glass shapes per frame.
- `MAX_DRAWS = 5` — rasterized text/DOM layers bound as textures per frame (two page layers, up to
  two standby-scape layers, one app-window layer — see `panel.js`'s `frame()`, the `draws` array).
- `MAX_OVER = 6` — overlay fills drawn inside the control center's own glass (`uOver`/`uOverC`).
- `LEVELS = 6` — blur pyramid levels.
- `SPHERE_POINTS = 1100` — standby orbit particles.

These are hard caps in typed `Float32Array`/`Int32Array` uniforms; going over one silently drops
the extra items rather than erroring; if you add a slot that could realistically be exceeded, widen
the array in `glass.js` and the corresponding `MAX_*`.

## The markup contract

`panel.js` and `raster.js` read these `data-*` attributes off the DOM:

- **`data-surface`** — a Material surface. `panel.js` tracks its rect, corner radius, fill colour
  (from the computed `--fill` custom property) and state-layer position, and feeds it to the
  renderer as an SDF rounded rect.
- **`data-clip`** — on a surface, the id of another surface whose rect and radius clip this one's
  drawn corners (used so a slider fill's outer corners always match its track, see `controls.js`'s
  light slider).
- **`data-ink`** — the surface's fill carries readable text; text drawn over it is recoloured to
  the fill's own on-colour, pixel by pixel, in the scene shader.
- **`data-glass`** — a floating control: dock, transport, standby actions, the top island, the
  climate dial's knob, the pager, control-center modules, activity cards and the orb. `panel.js`
  tracks these separately from surfaces and feeds them to the glass pass as `uGlass`/`uGlassR`/etc.
- **`data-live`** — the element stays real DOM, drawn by the browser on top of the canvas, never
  rasterized (launcher icons mid-cascade — a rasterized layer can't move icons independently).
- **`data-raster`** — a shape (not text) that `raster.js` paints into the 2D canvas: fill from
  `--raster-fill` or the computed background colour, clipped to its border radius; `data-raster="image"`
  draws an image instead of a fill.

**The key rule:** `[data-glass]` and `[data-live]` subtrees are *skipped* by the rasterizer
(`raster.js`'s `SKIP` selector) and stay visible DOM on top of the canvas. Everything else —
surfaces, their text, icons, marked shapes — is rasterized into a canvas, uploaded as a WebGL
texture, and drawn in Pass A so the glass in Pass B can refract it. Glass draws its own labels in
CSS; it never sits over rasterized text, because rasterized text is drawn *under* the glass, not
over it.

## The `ctx` API

`panel.js`'s `createPanel()` returns `{ ctx, measure, frame, drainUploads, retone }`. Other modules
(`controls.js`, `apps.js`, `cc.js`, `activities.js`, `standby.js`, `launcher.js`, `pages.js`) take
`ctx` and wire themselves through it. As declared in `panel.js`, the surface is (verify against the
file rather than trusting this list — it changes):

- **Geometry:** `ctx.W`, `ctx.H`, `ctx.unit` (panel px → CSS px, `W / 720`), `ctx.index`,
  `ctx.page`, `ctx.section`, `ctx.appOpen`.
- **Lookups:** `ctx.surface(el)`, `ctx.glass(el)`, `ctx.local(e)` (pointer position relative to the
  screen).
- **Repaint:** `ctx.dirty(el)` (repaint one element's rasterized region), `ctx.upload(name, source,
  region)`, `ctx.recolor(el)`, `ctx.reshape(el)`.
- **Navigation:** `ctx.go(index)`, `ctx.wake()` (bring the chrome back), `ctx.setCover(image)`.
- **Hooks:** `ctx.onMeasure({ before, after })`, `ctx.onFrame(fn)`, `ctx.onRetone(fn)`,
  `ctx.onIndex(fn)`.
- **Gesture claims:** `ctx.setVerticalPan(slot, handler)`, `ctx.addDragTarget(el, handler, axis)`
  — handlers share the shape `{ begin(target, t), move(d, t), end(t), quiet? }`.
- **Windows and sheets:** `ctx.setAppWindow(fn)`, `ctx.setControlCenter(fn)`, `ctx.setAppOpen(open)`.
- **Other:** `ctx.setExtraDirty(fn)`, `ctx.toast(text, icon)`, `ctx.measure()`,
  `ctx.measureWithin(root)`, `ctx.suppressClick()`, `ctx.setIdle(ms)`.

Several modules also attach their own properties to `ctx` at boot (`ctx.home` in `controls.js`,
`ctx.applyScene`, `ctx.togglePlay`/`ctx.nowPlaying`/`ctx.skipTrack`/`ctx.playTrack`, `ctx.activities`
and `ctx.activitiesRect` in `activities.js`, `ctx.appProgress`/`ctx.appTile` in `apps.js`,
`ctx.ccProgress` in `cc.js`). Grep `panel.js` and the module you're touching before assuming a name.

## The app module contract

`apps.js`'s `createApps(ctx)` returns `{ open, close, current, register }`. Register a module with:

```js
apps.register('name', { beforeOpen, open, close, frame(t, dt, { k, full, win }) });
```

All four are optional. `beforeOpen` runs once before the window starts growing; `open`/`close` run
each time the app opens or closes (including turning around mid-flight); `frame` runs every render
frame while the app has a current entry and returns extra render state (e.g. `{ appDraw }` merged
into the frame). The markup pattern is:

```html
<section class="app" data-app="name" hidden aria-label="…">…</section>
```

with a launcher icon wired as `<button class="app-icon" data-live data-app="name">…</button>` (see
`index.html`'s `#apps-view`) — `apps.js` finds the section by `data-app`, and `launcher.js` /
`app.js`'s `openApp` helper open it by the same name, passing the icon's `.app-tile` element as the
rect the window grows from. A page reached with `data-go="N"` (rooms/climate/today/standby) is a
slot index, not an app — it calls `ctx.go(N)` instead of `apps.open`.

## Glass groups

Glass shapes only melt together (smooth-min their SDFs) within the same numeric group; separate
groups never touch or share a tint (`glass.js`'s `GLASS_FS`, driven by `uGlassG`). `panel.js`
assigns the group per element id in its `GROUPS` map — read that map rather than assuming a group
from `bezel-glass.md`'s component-level description, since new glass (e.g. `activities.js`'s cards
and orb) is grouped in code, not in that doc. Anything not in `GROUPS` and not inside `#cc` falls
back to group 4; anything inside `#cc` is group 6, and the live-activity cards, which melt into the
orb, are group 7. The glass shader keeps eight groups, 0–7; a higher number shares group 7. Shapes in
one group only melt when they come closer than the merge distance (16 px), which is why the activity
cards sit 26 px apart and the control center's modules stay separate.

## Motion

Always use a `SPRINGS` role from `motion.js`, never a raw CSS `transition`/`duration`/`animation`
for anything a finger can touch or interrupt. The roles, as declared in `motion.js` (cross-check
before citing a number — this list is copied from Detent's `presets.json` and can drift):

`hold`, `release`, `smooth`, `settle`, `detent`, `wobble`, `effect`, `layer`, `tick`, `page`,
`edge`, `light`. Each is commented in `motion.js` with what it's for and which Detent preset or
pattern it comes from.

- **Interruptibility is required.** A `Motion` (`motion.js`) always starts a retarget from its
  *current* value and hands off its current velocity — never snaps to a new start. Use `.set()` to
  hold a value under a finger (remembers velocity for later hand-off) and `.to()` to retarget.
- **Projection plus snap.** A release projects the gesture's velocity forward
  (`project(velocity, rate)`) and lands on the nearest valid target (`nearest()` /
  `snapTarget()`) — see the dock droplet, the climate dial, the control center's pull, and the
  activities stack's throw-to-corner, each in its own file.
- **Rubber banding.** Past a scroll or drag's edge, `rubberBand()` compresses the excess before
  springing back with no velocity on release. Every drag surface (`level` slider, dock, dial,
  activities, control-center pull, Cover Flow, the overview scroll) uses it — don't let content
  travel past its bound uncompressed.
- **Reduced motion.** `setReducedMotion(true)` (from `app.js`, driven by `settings.calm`) critically
  damps every spring below critical damping at the same stiffness (`motion.js`'s `calmed()`). Don't
  add a second, separate "calm" code path for a spring's damping — that's already handled centrally;
  what individual modules gate on `settings.calm` is everything a spring *can't* express: stretch
  amounts, parallax, drift, and whether something bounces at all.

## Standby faces

Standby has twelve faces (`standby.js`'s comment enumerates them: orbit, dots, dial, horizon,
words, world, moon, tree, flip, album, orrery, next up), each a `.scape` div in `index.html`'s
`#scapes`, in the same order as `standby.js`'s `ACTIONS` array (indices 0–7) concatenated with
`faces2.js`'s `ACTIONS2` (indices 8–11) and `#scape-index`'s dots. To add a face:

1. Add a `<div class="scape" data-scape="name">…</div>` to `#scapes` in `index.html`, in position,
   and an `<i>` to `#scape-index`.
2. Add its background-drawing function to the `FACES`/`FACES2`/`SCAPES` GLSL source
   (`faces.glsl.js`, `faces2.glsl.js`, `scapes.glsl.js`) and dispatch it by index in
   `scene.glsl.js`'s standby branch (`k < N.5` chain, driven by `uScapeF`).
3. Add an entry to `standby.js`'s `ACTIONS` array (or `faces2.js`'s `ACTIONS2`) saying where the
   standby actions leave room for this face — `{ row }` for a centred row, `{ split }` for the two
   corners.
4. If the face carries live text, wire it in `standby.js`'s `updateClock`/`onFrame` or add it to
   `faces2.js`'s `updateFaces2`, and make sure whatever changes it calls `stale()` (or the specific
   face's raster gets marked stale) so `ensure()` repaints it.

`faces.glsl.js`, `faces2.glsl.js`, `tree.js`, `treepass.js`, `coverflow.js` and `coverpass.js` are
under active development elsewhere as this file is written — check their current state before
relying on their internals; this file only describes what they're for.

## Gotchas

- **`hidden` measures as zero.** An element with the `hidden` attribute has no layout box, so
  anything that needs to measure a hidden layer's glass or surfaces must un-hide it inside
  `ctx.onMeasure`'s `before()` hook and restore it in `after()` — see `cc.js`'s
  `hiddenBefore`/`layer.hidden` dance and `apps.js`'s `paint()` (which un-transforms the app layer
  before measuring, not un-hiding, since apps are hidden by a different mechanism — check which
  pattern applies before copying one).
- **Calm mode = reduced motion + solid glass**, not just slower animation. `settings.calm` also
  turns off refraction (`uSolidOnly` in `glass.js`), drift and parallax — see `app.js`'s `loop()`.
- **Light mode is its own design**, not a mechanical inversion of dark mode. Tokens, the standby
  scapes and the climate dial's lit-arc colour each have their own light-mode values; don't invert
  a dark-mode value to get a light-mode one.
- **`window.__bezel` exists on localhost** (`app.js`, gated on `location.hostname === 'localhost'`)
  for inspecting `{ panel, ctx, apps }` from the console while developing. It is not present on a
  deployed build.
- **Material Symbols ship as a curated subset.** `index.html`'s Google Fonts `<link>` for Material
  Symbols Rounded has an explicit `icon_names=` list. A new icon name must be added to that list or
  the glyph won't render — and getting an HTTP 200 back from the font request does **not** by
  itself prove the glyph you asked for is in the subset; check the rendered icon, not the response
  code.

## Rules

- Files are **UTF-8 without a BOM**. Never write them with Windows PowerShell 5.1
  (`Get-Content`/`Set-Content` add a BOM or mangle `—`, `·`, `←`); use editor tools instead.
- Don't add features that don't serve the design philosophy — wall-panel Material structure +
  Liquid Glass + Detent motion. If something doesn't fit one of those three, it doesn't belong here;
  raise it instead of adding it.
- **Verify visually** in a browser at the panel's actual size, in both dark and light tone, before
  calling anything done. A change that only looks right in the DOM inspector or in isolation is not
  verified.
- Commit messages end with the `Co-Authored-By` trailer the session gives you.
- Push only when the user explicitly asks, and push to **both** remotes — `origin` carries a GitHub
  and a Forgejo push URL.

## Catalyst Tab (`tab5/`)

`tab5/` is Bezel on a microcontroller: an ESP-IDF firmware for the M5Stack Tab5 and a Linux simulator
built from the same C. Its README says how to build both; its `docs/` hold the hardware research, the
Catalyst topic contract and the Bezel port's numbers. Things that bite:

- Everything above `components/tab_hal/` is portable C and must stay that way — the simulator is how
  changes are verified. Run `sim/tour.txt` and look at the shots, and `catalyst_tab_tests`.
- No project component may be named `hal` (it replaces ESP-IDF's own); the HAL is `tab_hal`.
- Glass objects live on the glass layer (`bz_ui_glass()`), everything they frost on the content layer
  (`bz_ui_content()`), mirroring `[data-glass]` here.
- Motion uses `bz_motion.c`'s roles (the same `SPRINGS` as `motion.js`), never LVGL animations.
- The tablet never commands a robot: it writes only declared tunables, the auto choice and a
  Limelight's LED, as Catalyst Console does.

## Publishing the specimen as an artifact

`python tools/artifact.py` writes `dist/artifact.html`: the page without its document wrapper, which
is the form an artifact publish takes. Publish it with every module, `detent/` and both stylesheets
mapped at the artifact's root, and republish to the same URL to update it.
