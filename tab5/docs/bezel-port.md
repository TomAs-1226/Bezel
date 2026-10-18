# Bezel on an ESP32-P4

How the house style — wall-panel Material structure, Liquid Glass, Detent motion — is kept on a
360 MHz microcontroller with no GPU. The numbers come from this repository's `device.css`,
`motion.js`, `shaders.js` and `panel.js` and from Detent's `knowledge/bezel*.md`; where those
disagree, the code won (as `AGENTS.md` says).

## The frame

Bezel's reference panel is 720 × 720 and one panel px is `W / 720`. The Tab5 renders **landscape,
1280 × 720**, so the height is Bezel's and the unit is exactly 1: every Bezel number is a Tab5 pixel.
The extra 560 px of width holds a second column. Bottom-anchored chrome keeps its bottom offsets
(dock 30, page inset 168); top-anchored chrome its top offsets (island 18, page padding 30).

Pixel density is 294 ppi against the reference's 255, so a Bezel control is 13 % physically smaller.
The 48 dp touch floor becomes 88 px: visuals stay 1:1, hit areas are enlarged
(`lv_obj_set_ext_click_area`).

## The renderer (`components/bezel/src/bz_comp.c`)

The specimen draws everything under the glass into a texture (pass A), blurs it through a six-level
pyramid, and composites refracting, lit glass shapes over it (pass B), with the glass's labels as DOM
on top. Catalyst Tab keeps that order with two LVGL displays and a compositor:

| Web specimen | Catalyst Tab |
|---|---|
| Pass A scene texture (tiles, text, icons) | LVGL display #1, RGB565, direct mode, full-frame buffer in PSRAM |
| DOM over glass (`[data-glass]` labels) | LVGL display #2, ARGB8888, transparent screen — only what sits *on* glass |
| Blur pyramid, level ≈ σ 11.3 px at panel scale | Per glass group: the content under it at ¼ scale, two box passes of radius 3 (σ ≈ 2.83 × 4 = 11.3 px), rebuilt only when that content changes |
| Pass B per pixel: SDF, refraction, frost, evening, saturation, tint, rim, shadow, press glow | Same order; geometry (coverage, refraction offset, rim light, shadow) cached per group while the shape is still, so a still frame costs one bilinear lookup and a few integer ops per glass pixel |
| Glass groups merging with smooth-min k = 16 CSS px | Same groups (dock 0, island 3, pager 5, control center 6, activities 7 — here the assistant's orb), k = 22.6 panel px; a shape marked *solo* never melts (the control center's modules, which pass near each other while they cascade) |
| WebGL canvas to screen | PPA rotates only the damaged rectangles into the back of two DPI frame buffers, swapped on vsync, asynchronously: the cores draw frame N+1 while the PPA turns frame N |

**Glass parameters** (shader CSS px × 1.41 for a 720 px panel): frost σ 11.3, slab thickness 37,
bezel width clamp(0.9 r, 11, 40) with profile `(1 − (1 − x)^4)^¼`, Snell n = 1.5, evening toward grey
0.17 by 22 % (dark) or 0.93 by 38 % (light), saturation 1.12, tint `mix(g, tint·(0.82 + 0.35·luma) +
0.04, 0.82·amt)`, rim ≈ 1 px lit by `0.16 + 0.62·max(f,0)^1.5 + 0.26·max(−f,0)^2` with the light at
(−0.42, −0.91), shadow offset 7·big and falloff to 34·big with `big = clamp(min(w,h)/127, 0.5, 1.4)`,
press glow `0.22·exp(−d/65)`, press scale 1.06. The dock presses with the glow alone (scale 1.0): a
scaled shape is a new shape, and the dock's geometry table would be rebuilt every frame of every tap.
The IMU's lean of the light is applied when compositing, from the table's stored normals, so tilting
the tablet never rebuilds a table either.

**Materialize by strength, not opacity**: strength 0→1 scales frost, evening, tint, rim, shadow and
thickness together.

**Calm** = reduced motion + solid glass: surface2 fill, rim and shadow, no frost or refraction — the
design's own cheap path.

## Keeping 60 Hz

The panel is driven at 60.5 Hz (DPI timings chosen for it, `hal_tab5.c`), and a frame has 16.5 ms.
Redrawing the page with LVGL's software renderer costs ~26 ns a pixel across both cores — a whole
1280 × 720 page is ~24 ms — so the rule is that nothing big is ever drawn twice while it moves:

- **Presenting.** Each damaged area is split into cells. A cell with no glass, ink, backdrop or
  antialiased layer corner in it goes to the panel straight from wherever its pixels already are (the
  content buffer or a cached picture); only the rest is composited. Work inside a frame is split
  across both cores by rows.
- **Pages.** While the pager moves, the five pages are one cached picture (a strip, half a page of
  ground either side) that the compositor slides; LVGL is frozen. The page on screen is copied, not
  drawn, when a swipe starts; the others are refreshed in the background while nothing moves, a band
  of 90 rows per frame, never a whole page at once.
- **App windows.** Opening draws the window once, as it will look full size, before it moves; it then
  grows out of its icon as a picture over a picture of the page. Closing takes the window as it is on
  screen.
- **Landing.** When a gesture ends, LVGL has to catch up with everything it didn't draw while frozen.
  That happens a band per frame over eight frames while the pictures stay up, and a finger on the page
  finishes it at once so a press always shows.
- **Lists.** A list scrolls by moving the rows already drawn and having LVGL draw only the strip that
  came into view (`bz_ui_scroll`), and a list's column is fixed and very tall so a row that grows or
  arrives redraws only itself. The assistant's transcript streams without redrawing what's above.
- **Tables.** Refraction geometry per glass group, rebuilt only when a shape's size or radius changes;
  a shape that only moves shifts its table; strength, press, tint and light are applied per pixel.
- **The control center's blind.** The web specimen frosts the whole page by the pull's amount; a
  per-pixel cross-fade of the whole screen every frame is more memory traffic than a frame has on the
  P4. Here a frosted, dimmed picture of the page comes down behind the modules like a blind, and only
  the rows its soft edge crosses are mixed. The page underneath is frozen while it is down.

**What a frame costs** comes from the simulator: `trace NAME` … `trace end` in a script prints each
frame's work (pixels LVGL drew, composited, presented directly, glass by path, tables rebuilt, blur
sources, rows shifted) costed for the P4 — the larger of the CPU's time and the PPA's, per-kernel
cycle estimates, PSRAM/PPA at 400 MB/s. It is a model, not a measurement; on the tablet the
settings screen's `fps` chip shows measured frame times over every screen. Modelled today:

| Interaction | avg ms | p95 ms | frames over 16.7 ms |
|---|---|---|---|
| swipe between pages | 11.5 | 12.8 | 0 of 65 |
| jump pages from the dock | 11.1 | 13.7 | 0 of 65 |
| open an app | 9.6 | 16.2 | 2 of 45 — the window is drawn once before it moves (one frame of start-up) |
| scroll a list | 12.8 | 14.4 | 0 of 53 |
| close an app | 7.2 | 14.3 | 0 of 65 |
| assistant streaming a reply | 8.5 | 13.4 | 1 of 67 — the first row of a new conversation |
| close the assistant | 5.1 | 17.0 | 9 of 124 — the orb's glass re-frosting as the window shrinks, at 17–18.5 ms |
| **pull the control center** | 15.7 | 47.6 | **28 of 87** |
| **close the control center** | 13.5 | 69.5 | **13 of 61** |
| control center open, idle | 2.6 | 3.4 | 0 of 60 |

The control center is the one place that doesn't hold 60 Hz in the model. Seven solo glass modules
cascade in at once, so every frame pays for their edges and shadows (~450k glass pixels), LVGL
redrawing their labels as they move and fade (~270k), and composing ~600k pixels over the blind. Making
it fit means caching each module's ink while it slides and a cheaper edge while strength is below 1.

## Motion (`components/bezel/src/bz_motion.c`)

Closed-form springs sampled at the frame time, retargeted from the current value and velocity.

| Role | Source | k | c | ζ |
|---|---|---|---|---|
| hold | response 0.15, damping 0.86 | 1754.6 | 72.05 | 0.86 |
| release | duration 0.5, bounce 0.15 | 157.9 | 21.36 | 0.85 |
| smooth | duration 0.5, bounce 0 | 157.9 | 25.13 | 1.0 |
| settle | response 0.4, damping 1 | 246.7 | 31.42 | 1.0 |
| detent | response 0.4, damping 0.8 | 246.7 | 25.13 | 0.8 |
| wobble | duration 0.5, bounce 0.3 | 157.9 | 17.59 | 0.7 |
| effect | stiffness 1600, ratio 1 | 1600 | 80.0 | 1.0 |
| layer | stiffness 3800, ratio 1 | 3800 | 123.3 | 1.0 |
| tick | stiffness 800, ratio 0.6 | 800 | 33.94 | 0.6 |
| page | duration 0.45, bounce 0.12 | 195.0 | 24.57 | 0.88 |
| edge | m 0.5, k 100, ratio 1.1 | 100 | 15.56 | 1.1 |
| light | response 0.4, damping 1 | 246.7 | 31.42 | 1.0 |

Gesture math: `project(v, rate) = v/1000 · rate/(1 − rate)` (0.998 normal, 0.99 fast);
`rubberBand(o, dim, 0.55) = c·|o|·dim/(dim + c·|o|)`; `snapTarget = nearest(x + project(v, 0.99))`;
velocity from a least-squares quadratic over the last 100 ms (a 40 ms gap breaks the window). Pans
lock an axis after 10 px, sliders start after 3 px, taps are ignored for 400 ms after a drag. Calm
critically damps every underdamped spring at its own stiffness.

## Colour

Bezel's own tables (`device.css:4-15`), with Catalyst Console's status colours added as a separate job:
status colours mean status and nothing else and always carry a shape (● ok, ◆ check, ■ fault,
○ stale). Signal stays Bezel's single warm accent: the primary action, progress, focus, the current
selection. Ice means "on"; amber means a light is on and isn't used here.

| Token | Dark | Light |
|---|---|---|
| ground | #0B0C0E | #E8EAED |
| surface1 / 2 / 3 | #16181B / #1F2226 / #2A2E33 | #FBFBFC / #EEF0F3 / #E1E4E8 |
| track / meter | #1F2226 / #24272C | #FBFBFC / #E4E7EB |
| ink / dim / faint | #EEF0F2 / #9AA1A8 / #5D646B | #15181B / #5B636C / #A3AAB2 |
| signal / onSignal | #FF5B1F / #1A0A02 | #EA5A1A / #FFFFFF |
| ice / onIce | #CFE0EE / #0E2233 | #CDE1F3 / #0C2436 |
| ok / warn / fault | #30D158 / #FFB340 / #FF453A | #1F9D45 / #B86E00 / #D42A20 |

## Components on this panel

- **Dock** (glass group 0): items 104 × 76, radius 38, gap 4, padding 8, 30 from the bottom, icon 32,
  mono label 13; the current item is ink with a filled icon, the rest dim. Tucks away after 4 s idle.
- **Top island** (group 3): 60 tall capsule 18 from the top; carries robot, mode and battery, and
  morphs to carry a message (`h = 60·(0.58 + 0.42k)`, text fades in above k = 0.55).
- **Tiles**: surface1, radius 32, padding 24, 14 apart. Values in the display face with tabular
  figures; labels in the lowercase mono face.
- **Level** (Bezel's slider): track 128 tall radius 32 in the specimen; the tunables use a 96 tall
  track here, handle 6 × 88 → 4 × 104 while held, rubber-band 12 % past the ends.
- **Meter**: 34 tall capsule, fill inner radius 6.
- **Control center** (group 6, every module solo, 26 px apart): pulled down from the top edge;
  progress = dy / 380, opens if `p + project(v, 0.99) > 0.5`; the modules cascade in by delay, each
  materializing by strength; behind them the frosted, dimmed page comes down as a blind (above).
- **The orb** (group 7, solo): the assistant, bottom right on every page; the confirmation card it
  raises is group 5, solo.
- **Apps** grow out of the icon that opened them on the release spring and can be caught mid-flight.
