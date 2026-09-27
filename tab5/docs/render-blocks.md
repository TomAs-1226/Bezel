# What the tablet renders

Everything a model writes into Catalyst Tab — the assistant's answers, the companion's replies, and the
documents it writes to the card — goes through one renderer, `components/ui/src/ui_md.c`. This page is
the contract: what markdown it honours, and the two fenced languages that are not text.

It is also what the assistant's system prompt teaches, so a change here is a change there.

## Markdown

| Written | Drawn |
|---|---|
| `# Heading` | the title face |
| `## Heading` | the name face |
| `### Heading` | the bold body face |
| `**bold**` | Sans Flex 600, the bold body face (`BZ_F_BODY_B`) |
| `` `code` `` | the mono face in the accent colour |
| ```` ```lang ```` … ```` ``` ```` | a code card in the mono face |
| `- item`, `* item`, `+ item` | an em dash and a hanging indent |
| `1. item` | the number and a hanging indent |
| `> quoted` | an accent rule down the left, dimmed text |
| `---` | a hairline |
| a blank line | a new paragraph |

Deliberately **not** rendered, because a model told to answer briefly does not need them and each one
costs objects on a tablet that draws with a CPU: tables, images, links as links, nested lists past one
level, inline HTML, strikethrough, footnotes. They come out as the text they are written in.

A paragraph with no `**` and no backtick is a plain label. One with either is an LVGL span group, which
costs more — hence the split.

## ```chart

A chart in a few dozen bytes. It is line-oriented on purpose: a language model emits `y 12.4 12.3 12.1`
reliably and JSON with nested arrays much less so, and there is nothing here to parse but numbers.

    ```chart
    line battery through the match
    x 0 15 30 45 60 75 90 105 120
    y 12.6 12.4 12.1 11.7 12.0 11.9 11.6 11.4 11.8
    unit V
    ```

**The first line** is the kind and, after a space, the title. The kind is `line`, `bar` or `scatter`.

**Then one directive a line**, the key, a space, and its values. Values may be separated by spaces or
commas.

| Directive | Means |
|---|---|
| `x …` | the x values. On a `scatter` they are the real x of each point; on a `line` or `bar` they only caption the axis, and may be left out |
| `y …` | one series. Repeat the line for more; at most four |
| `name …` | names the series — before its `y` line, or straight after it |
| `unit …` | the unit, written beside the axis values (`V`, `A`, `°C`, `ms`) |
| `labels a, b, c` | what runs along the bottom, instead of the first and last x |
| `ylim lo hi` | fixes the y range; without it the range comes from the data with a little air above and below |

Two series, named:

    ```chart
    line module speed, commanded against actual
    y 0 1.2 2.4 3.1 3.0 2.2
    name commanded
    y 0 1.1 2.2 2.6 2.4 1.9
    name actual
    unit m/s
    ```

A bar chart of four things:

    ```chart
    bar current by mechanism
    y 34 18 52 7
    labels drive, arm, shooter, intake
    unit A
    ```

**Limits.** Four series, 96 points each; a longer series is cut and the chart says so along the bottom.
The y axis is drawn as its top and bottom value in the gutter — LVGL's chart draws no tick labels, and a
grid of numbers at this size is unreadable anyway.

**Malformed input is shown, never guessed.** A chart that does not parse becomes a card saying exactly
what is wrong, with the block printed underneath, because a wrong chart of a robot's battery is worse
than no chart — someone would act on it. The messages are:

- `the first word must be line, bar or scatter, not "…"`
- `x has "…" in it, which is not a number` (and the same for `y`)
- `there is no y line, so there is nothing to draw`
- `a series needs at least two points; series N has M`
- `the series are different lengths (N and M)`
- `a scatter needs one x for every y (N x, M y)`
- `N x values against M y values`
- `"…" is not one of x, y, name, unit, labels, ylim`
- `ylim wants two numbers, a low and a high`
- `more than 4 series; this tablet draws 4`

## ```svg

A drawing, in the subset of SVG this hardware can honestly rasterise. LVGL 9.5 does ship an SVG loader,
but it only builds a node tree and hands it to LVGL's vector pipeline, which needs a vector rasteriser;
on the ESP32-P4 that would have to be ThorVG's software canvas — about 2 MB of C++ compiled out of this
build, wanting exceptions and RTTI that this firmware turns off, rasterising floats on a 360 MHz core.
So the tablet draws SVG itself, onto an `lv_canvas`, and is explicit about where that stops.

**Supported:** `<svg>` with `viewBox` (or `width`/`height`), `<g>` with `transform` of `translate`,
`scale`, `rotate` and `matrix`, and the shapes `<line>`, `<rect>` (including `rx`/`ry`), `<circle>`,
`<ellipse>`, `<polyline>`, `<polygon>`, `<path>` with `M m L l H h V v C c S s Q q T t Z z`, and `<text>`.
Attributes: `fill`, `stroke`, `stroke-width`, `opacity`, `fill-opacity`, `stroke-opacity`, `font-size`,
`text-anchor`. Colours as `#rgb`, `#rrggbb`, `none`, `currentColor` and the basic CSS names.

**Not supported:** elliptical arcs in path data (`A`/`a`), gradients, patterns, clip paths, masks,
filters, dash patterns, mitred stroke joins, `<style>` blocks and class selectors, `<use>`, `<defs>`,
`<symbol>`, embedded images, and text on a path.

Anything left out is **named on screen** under the drawing — `left out: elliptical arcs` — rather than
quietly missing, so nobody reads a partial drawing as the whole one.

`.SVG` files in `CATOS/DOCS` are drawn by the documents app through the same renderer.

## Where it is used

- `components/ui/src/ui_app_assist.c` — the assistant's answers
- `components/ui/src/ui_app_companion.c` — the companion's replies
- `components/ui/src/ui_apps_os.c` — the documents app, for `.MD` and `.SVG` on the card

The two models reach it differently, because only one of them is read out loud.

The **assistant** writes markdown, `chart` and `svg` directly into its answer; its whole reply goes through
the renderer. The **companion** may not: its words go to a text-to-speech voice, so anything it writes is
spoken exactly as typed, and a chart block would be read out as gibberish. It has a second field instead —
`show` in its reply JSON, `vo_reply_t::show` — which is drawn under the spoken prose and never voiced. It
goes through the same renderer, so `svg` would draw — but the companion is taught markdown and `chart`
only, because its caption is a narrow column beside a face and a drawing wants the room the assist app
has. A companion that reaches for markdown in `say` anyway is still
rendered rather than shown as raw asterisks, but that is a fallback, not the way in.

A document can also be written onto the card from outside the app — that is what the assistant's
document tool calls:

```c
/* components/ui/include/ui.h */
bool ui_docs_write(const char *name, const char *markdown, char *why, size_t nwhy);
```

`name` is a bare file name with no directory parts (the extension is forced to `.MD`), `markdown` is the
whole document, and `why` comes back with a plain-words reason when it returns false: a name that tries
to leave the folder, no card, a card that is full, or nothing to write. It only touches the card, so the
assistant's worker may call it; the island says what happened on the next UI tick.
