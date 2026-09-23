# Bezel

Bezel is Thomas's UI house style: embedded wall-panel Material structure (tonal surfaces, a shape
scale, state layers), Apple's Liquid Glass (a real refracting, frosted, lit glass renderer, not a
blur filter pretending to be one) and Detent's motion (interruptible springs and gesture patterns).
The name has two meanings: an instrument's bezel, and the glass rim where light bends.

This repository holds the **specimen**: a 720 × 720 smart-home wall panel built as a static web
page (WebGL2 + DOM). The design tokens, the philosophy and review-checklist docs, the Claude skill
and the MCP tools that read them all live in the [Detent](https://github.com/TomAs-1226/Detent)
repository: see `knowledge/bezel.md`, `knowledge/bezel-glass.md`, `knowledge/bezel-components.md`
and friends there. This README and `DESIGN.md` link to that material rather than repeating it.

## Running it

Bezel is a static page with no build step. Serve the repository root with any static file server
and open it:

```
python -m http.server 8000
```

then visit `http://localhost:8000/`.

The page needs **WebGL2** to draw the glass; it loads two type faces from **Google Fonts** and
Material color tokens (the dynamic-colour library) from **jsDelivr**. All three are optional:
without WebGL2 the glass falls back to a CSS `backdrop-filter` blur with no refraction, without the
Google Fonts request the CSS fallback stacks (`Roboto Flex`, `Roboto Mono`, then system fonts) take
over, and without the jsDelivr module dynamic colour just stays off — Bezel's own dark and light
tokens are always in place regardless of network access.

## Publishing it as a Claude artifact

An artifact publish wraps the page in its own document, so it takes the page without one:

```
python tools/artifact.py
```

writes `dist/artifact.html` (ignored by git). Publish that file as the page, with every module,
`detent/` and both stylesheets mapped at the artifact's root.

## What's on the panel

- **Four home pages** — overview, rooms, climate, today — plus an apps page, now playing and
  standby, six slots side by side that a swipe or the dock pages between.
- **An apps launcher** with fourteen apps: rooms, climate, today, weather, calendar, music, energy,
  timers, security, shopping, appliances, plants, standby and settings. Icons cascade into place
  when the launcher opens.
- **Now playing**, with a spinning disc, a tick ring and transport controls.
- **Standby**, twelve faces paged vertically: orbit, dots, dial, horizon, words, world, moon, tree,
  flip clock, album, orrery and next-up.
- **The control center**, a sheet of glass modules (scenes, media, three sliders, four toggles)
  that pulls down from the top edge.
- **Live activities and the orb** — a floating stack of glass cards for whatever is running
  (timers, appliances, music, an unlocked door) that stashes into a single glass orb at the nearest
  edge when thrown to a side, and pulses or rings for what needs attention.

## Header controls

The controls above the live panel change how it's drawn:

- **panel** — Dark or Light tone. Light mode is its own considered design, not a mechanical
  inversion of dark mode.
- **glass** — Regular or Clear glass variant.
- **light** — Pointer or Tilt: what leans the glass's one light direction (device tilt falls back to
  the pointer if no sensor answers).
- **colour** — Bezel's own tokens, Cover (dynamic colour extracted from the currently playing
  album's cover art, Monet-style), or a fixed seed swatch (Sage, Iris, Rose). Disabled if the
  colour-science module fails to load.
- **Calm version** — a checkbox for reduced motion plus solid, non-refracting glass.

## File map

| Path | Purpose |
| --- | --- |
| `index.html` | The page: markup, header controls, the spec sheet below the panel |
| `bezel.css` | Page-level styles and the spec sheet |
| `device.css` | The panel's own UI styles |
| `panel.js` | Core panel state: slots, paging, chrome, the `ctx` API other modules use |
| `glass.js` | The WebGL2 renderer: the scene pass, blur pyramid and glass pass |
| `shaders.js` | Shared GLSL source (vertex shader, downsample, glass fragment shader, points) |
| `scene.glsl.js` | The scene pass's fragment shader |
| `scapes.glsl.js` | Standby background shaders for faces 0–3 |
| `faces.glsl.js` | Standby background shaders (continued) |
| `faces2.glsl.js` | Standby background shaders for faces 8–11 |
| `faces2.js` | Live text and helpers for standby faces 8–11 (flip clock, album, orrery, next up) |
| `raster.js` | Paints a DOM subtree's text, icons and marked shapes into a canvas for the renderer |
| `motion.js` | Detent springs, roles (`SPRINGS`), gesture helpers and the `Motion` class |
| `apps.js` | The interruptible app-window contract and its open/close/drag behaviour |
| `launcher.js` | The app grid's cascading icons |
| `cc.js` | The control center sheet |
| `activities.js` | Live activities cards and the stash-to-orb behaviour |
| `controls.js` | Press feedback, the light slider, scenes, stepper, media, standby actions, the island |
| `pages.js` | Rooms, climate and today: dimmer tiles, the climate dial, the reminder |
| `standby.js` | Standby paging, the clock and the twelve faces' shared plumbing |
| `monet.js` | Dynamic colour: seed extraction and HCT tonal palettes (Android Monet-style) |
| `app.js` | Boot, settings, colour application, the one light and the frame loop |
| `coverflow.js` / `coverpass.js` | The music app's Cover Flow view and its WebGL scene pass |
| `household.js` | Timers, security, notes (shopping) and settings apps |
| `lifestyle.js` | Weather, calendar, appliances and plants apps |
| `tree.js` / `treepass.js` | The energy app's tree visualization and its WebGL scene pass |
| `scenes.js` | Album art canvas painting and atlas building |
| `detent/` | A copy of Detent's spring solver, gesture math and easing (`spring.js`, `gesture.js`, `velocity.js`, `easing.js`) |
| `tools/artifact.py` | Turns `index.html` into the page an artifact publish takes |
| `tab5/` | **Catalyst Tab**: Bezel ported to the M5Stack Tab5 (ESP32-P4) as a diagnostics handheld for FrcCatalyst robots — its own README |
