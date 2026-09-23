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
| Glass groups merging with smooth-min k = 16 CSS px | Same groups (dock 0, island 3, pager 5, control center 6, activities 7), k = 22.6 panel px |
| WebGL canvas to screen | PPA rotates only the damaged rectangles into the back of two DPI frame buffers, swapped on vsync |

**Glass parameters** (shader CSS px × 1.41 for a 720 px panel): frost σ 11.3, slab thickness 37,
bezel width clamp(0.9 r, 11, 40) with profile `(1 − (1 − x)^4)^¼`, Snell n = 1.5, evening toward grey
0.17 by 22 % (dark) or 0.93 by 38 % (light), saturation 1.12, tint `mix(g, tint·(0.82 + 0.35·luma) +
0.04, 0.82·amt)`, rim ≈ 1 px lit by `0.16 + 0.62·max(f,0)^1.5 + 0.26·max(−f,0)^2` with the light at
(−0.42, −0.91), shadow offset 7·big and falloff to 34·big with `big = clamp(min(w,h)/127, 0.5, 1.4)`,
press glow `0.22·exp(−d/65)`, press scale 1.06 (dock 1.02).

**Materialize by strength, not opacity**: strength 0→1 scales frost, evening, tint, rim, shadow and
thickness together.

**Calm** = reduced motion + solid glass: surface2 fill, rim and shadow, no frost or refraction — the
design's own cheap path.

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
- **Control center** (group 6): pulled down from the top edge; progress = dy / 380, opens if
  `p + project(v, 0.99) > 0.5`, the page behind frosts and dims (σ grows with the pull, dim 0.9·p).
- **Apps** grow out of the icon that opened them on the release spring and can be caught mid-flight.
