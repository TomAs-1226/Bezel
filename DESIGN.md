# Bezel — design rationale

Bezel is a UI house style built from three things that don't usually appear together: embedded
wall-panel Material structure, Apple's Liquid Glass, and Detent's fluid, interruptible motion. This
document is the "why" — what each influence contributes, how they're kept from fighting each other,
and the reasoning behind the specimen's larger pieces. For exact numbers — spring constants, blur
radii, colour formulas, component sizes — see the Detent repository's `knowledge/bezel.md`,
`knowledge/bezel-glass.md` and `knowledge/bezel-components.md`
([github.com/TomAs-1226/Detent](https://github.com/TomAs-1226/Detent)); this document doesn't duplicate their tables.

## Why these three, together

**Embedded wall panels** (dotstartech's CM4 panel, brushknight's ESP32 panels) are the reference,
not web "glassmorphism." Their vocabulary — dark tonal tiles, pill sliders, lowercase mono labels,
a tick ring around album art, one warm signal colour, a particle-sphere standby screen — reads as a
piece of hardware, not a webpage. That's the register Bezel wants: something that could be screwed
to a wall.

**Liquid Glass** gives that hardware register a floating control layer. Apple's own scoping rule —
glass for controls and navigation, never for the content underneath — is what keeps the two
languages from blurring into each other. Tiles, sliders and text stay solid and legible; only the
dock, the pager, the top island, now playing's transport, the standby actions, the climate dial's
knob, the control center's modules and the live-activities stack float as glass above them. If
everything were glass, nothing would read as glass.

**Detent's motion** is what makes the glass and the tiles feel like one physical object instead of
two design systems glued together. Every spring is a sourced preset played by role — a press
answers on pointer-down, a drag tracks 1:1, a release hands off its velocity, a fling coasts and
resists at its edges, a dial clicks into a detent. Nothing here is a CSS `ease-out` chosen by eye;
that consistency is what reads as "fluid" rather than "animated."

The restraint rule ties them together: nothing gets added that doesn't serve one of the three. A
feature that doesn't obviously belong to wall-panel structure, glass behaviour, or Detent's gesture
patterns doesn't belong on the panel.

## The rendering architecture, and why it's shaped this way

Glass drawn with a CSS blur *over* DOM text looks fake — the words sit on top of a lens that should
be bending them. So the specimen draws its own renderer in WebGL2, in two passes:

- **Pass A** paints everything under the glass — backgrounds, Material surfaces, the standby
  particle sphere, and text/icons/shapes rasterized from the DOM — into an offscreen texture.
- **A blur pyramid** (six half-size levels, low-passed on the way down, read back through a bicubic
  sample) gives any radius of frost without shimmering as content moves under it.
- **Pass B** composites a union of glass shapes over that texture: refraction at the bezel's
  profile, dispersion, luminosity evening, saturation, tint, one directional edge light, an adaptive
  shadow, and the press glow.

The DOM never disappears — it stays in the layout for hit testing, focus and screen readers, with
its own ink made transparent wherever the renderer is drawing it instead. `[data-glass]` and
`[data-live]` elements are the exception: they're skipped by the rasterizer and stay visible DOM
drawn over the canvas, because a control's own label and a cascading launcher icon need to move
independently of anything the renderer batches. Everything else gets rasterized so the glass can
bend it — that's the whole reason the renderer exists rather than a simpler backdrop-filter.

That two-pass, DOM-plus-canvas shape is also why glass groups exist: Apple's glass can't sample
other glass consistently, so every glass shape in the specimen draws in one union, split into
small families (the dock, the transport, the standby actions, the island, the dial's knob, the
pager, each control-center module, the live-activities stack) that merge only within themselves.
Without that grouping, an early version let a confirmation rising from the dock visually fuse with
the standby actions and bleed their tint into itself — which is also why confirmations moved to
their own top island instead of rising from wherever a change happened.

## Interruptible app windows and the launcher

Apps don't navigate to a new screen; they grow out of the icon you touched, on the same spring role
used for every other release, and can be interrupted at any point in that growth. Dragged down, an
open app becomes a card under your finger and either flies home or springs back open depending on
where its released velocity projects it; catching a *closing* window mid-flight stops it exactly
where you touched it, ready to reopen from there. This follows directly from Detent's principle
that nothing in a fluid interface should have a state a gesture can't interrupt — a window that
only animates open or only animates closed, with no path between, isn't fluid, it's two animations
stitched together.

The launcher's icons cascade into place on arrival rather than appearing at once, delayed by their
distance from wherever the eye entered from (the dock's apps button, or the edge swiped in from).
That's a small thing, but it's the same idea as the interruptible window: motion should read as
coming *from* somewhere, not simply turning on.

## Cover Flow, the energy tree, and other bespoke scenes

Not every screen is built from Material tiles and glass controls. The music app's Cover Flow view
and the energy app's tree are their own WebGL scene passes, added to the same Pass A the renderer
already runs, so glass elsewhere on screen still refracts them correctly. They exist because some
content — a shelf of album covers you flick through in 3D, a growing tree that visualizes energy
saved over time — doesn't reduce to a rounded rectangle with a label, and forcing it into the tile
vocabulary would lose what makes it worth having. The rule is still restraint: a bespoke scene earns
its place only when the tile-and-glass vocabulary genuinely can't express what it's showing.

## Live activities and the orb

Whatever is quietly running — a timer, a washing machine, unheard music, an unlocked door — floats
as a small stack of glass cards you can throw to a corner, or past an edge, where it stashes into a
single orb. The orb pulses and rings for what needs you: a warm glow as a countdown nears zero, a
brief travelling ring of light when something finishes unseen. This is the panel's answer to a
problem ambient wall displays have that phones don't: there's no notification center to check
later, and no badge count sitting in a pocket. Something running out of sight still has to be able
to say so, without ever taking over the screen the way a modal would. The design leans on the same
attention vocabulary Apple's Dynamic Island research warns against misusing — no slow looping
pulses that train the eye to ignore them, and reduced motion drops the travelling ring entirely,
leaving only a glow that fades in place.

## Standby's twelve faces

Standby is deliberately calm — designed scenes paged vertically, slow drift, nothing blinking —
because a wall panel spends most of its life in this state, and a wall panel that restlessly
animates while idle stops being furniture. Twelve faces (orbit, dots, dial, horizon, words, world,
moon, tree, flip clock, album, orrery, next-up) exist because a single ambient screen gets stale
fast on something meant to sit in a room for years; paging between calm variations, each true to the
same restraint rule, keeps the idle state interesting without making it busy. The standby actions
(quick whole-home scenes like "we're back" and "we're out") ride along the same paging spring so
they always land wherever the current face has left room for them, rather than sitting in a fixed
slot that some faces would have to design around.

## Colour: Bezel's tokens, or Monet from what's on screen

Bezel's own dark and light token sets are the default and the fallback. Dynamic colour — extracting
a seed from the playing album's cover art, or picking a fixed seed swatch, and building tonal
palettes from it the way Android's Monet does — is an optional layer on top, loaded from a CDN
module that may not be available. The reasoning for building dynamic colour on HCT tonal palettes
rather than simple hue rotation, and for keeping the signal colour vivid and amber semantically
fixed regardless of seed, lives in Detent's `bezel-colour.md`; the point worth stating here is that
losing the network never loses Bezel's own identity — the panel is designed to look like itself
with nothing but its own CSS and fonts loaded.

## The calm version

Reduced transparency and reduced motion aren't an afterthought bolted onto a finished design; every
spring, drift amount and stretch factor in the specimen is defined so that a single flag (mirroring
`prefers-reduced-motion` and `prefers-reduced-transparency`) can turn refraction into a solid rim
and critically damp every spring at its own stiffness, rather than swapping in a separate simplified
theme. That's what lets "calm" mean the same panel, quieted, instead of a different, lesser panel.
