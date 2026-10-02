# Where the 32 MB goes

The tablet has 32 MB of PSRAM and the UI has about six and a half of them. Everything in this file was
measured on the device with the dev console's `psram`, which exists because the free figure on its own
never told anybody anything.

```
tab5_dev.py COM9 psram
```

## The arithmetic

| | |
|---|---|
| PSRAM on the package | 32 MB |
| the program itself, run from PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`) | ~8.5 MB |
| **the PSRAM heap, therefore** | **~23.5 MB** |
| the HAL's fixed blocks | 11.3 MB |
| the renderer's fixed blocks | 5.3 MB |
| **left for the UI** | **~6.5 MB** |

The program's share is the surprise. With XIP from PSRAM the `.text` and `.rodata` segments are loaded
into PSRAM at boot and executed from there, outside the heap entirely — which is why PSRAM's total is
never the 32 MB on the package and why `heap_caps_get_total_size` is not the number to reason from.

The fixed blocks are nine full-screen pictures of the same screen:

| | |
|---|---|
| the panel's three DPI frame buffers | 5.4 MB |
| the page slide's snapshot, neighbour and chrome | 5.4 MB |
| the renderer's LVGL content buffer and its second one | 3.6 MB |
| the neighbour's landscape band target | 1.8 MB |
| the rotation stage | 0.6 MB |
| the ground strip, the slide's chrome mask | 0.2 MB |

## What grows with use, and what holds it

| | measured |
|---|---|
| app windows, all 39 built | 788 KB, of which **settings is 342 KB** |
| app icons | 557 KB |
| baked glass (the frost cache) | 200–830 KB, and it moves |
| one assistant answer carrying a chart and a drawing | **~360 KB** |

The last one is the one that emptied the heap. The transcript keeps 60 rows, which bounds nothing: eight
such answers took LVGL from 1.1 MB to 4.0 MB and left the tablet with 1.1 MB free. Rows were the wrong
unit, so `ui_app_assist.c` now measures each row and caps the transcript at 1 MB of them. After that, 4,
8 and 16 turns all hold at 1.8 MB of LVGL and 3.86 MB free — flat.

Below 1.5 MB free the shell hands memory back (`mem_pressure`): unreferenced baked glass first, then the
windows of apps that set `shed`. Only `settings` sets it so far. **Checking an app means** reading it for
widget pointers reached from anywhere other than its `build`, `open`, `close`, `refresh` and `frame`
hooks — a background ticker registered with `ui_on_refresh`, or another screen poking its widgets. An app
that has one must not be shed, because its window is about to stop existing.

## The cyan flash

Two different faults look the same on the glass, and they want opposite fixes.

**A scan-out underrun.** The panel reads the frame buffer out of PSRAM on a hard deadline — 110 MB/s at
60 Hz with no slack — and when it misses, the glass goes cyan for a frame. It is a *bandwidth* fault:
more free bytes do not help it, less PSRAM traffic does. `hal_underruns()` counts them, and the dev
console prints the count in `psram`.

Counting them is harder than it looks, and two earlier attempts counted nothing at all while the glass
was plainly flashing. The DSI driver reports an underrun with `ESP_DRAM_LOGE` from its ISR, which is
`esp_rom_printf`: it goes round `esp_log`, so a `vprintf` hook never sees it, and it writes to the ROM
console rather than the USB-Serial/JTAG console the dev tool reads, so a capture never sees it either.
The count sits on `esp_rom` output channel 2, where the characters actually pass.

**Memory nobody wrote.** The driver's three frame buffers came up uncleared, and PSRAM powers on holding
noise, which reads as vertical bars of saturated blue, green and cyan. Any region handed to the panel
before anything was written into it shows exactly that — a panel shot caught one: the left of the frame
was the page, the right was bars. They are cleared at boot now, which does not stop a region going
unwritten but makes the worst case black.

## XIP from PSRAM: measured, and kept

Turning `CONFIG_SPIRAM_XIP_FROM_PSRAM` off is tempting from both directions at once: it gives back 5.7 MB
of PSRAM (4.7 MB free → 10.4 MB at boot) and it takes every instruction fetch off the bus the panel is
trying to meet a deadline on. It was tried, on the device, and put back:

| | XIP from PSRAM | code from flash |
|---|---|---|
| PSRAM free at boot | 4.7 MB | **10.4 MB** |
| page swipe, frames at 60 | 78 % | 76 % |
| app open, frames at 60 | **82 %** | 78 % |
| UI loop work, mean | **6.5 ms** | 14.7 ms |
| UI loop work, max | **95 ms** | 101 ms |

The frame pacing is a wash; the CPU time more than doubles, and the UI's complaint has been that app
screens are slow. Flash at 120 MHz would have bought some of that back and **the tablet did not boot at
all** with it — the flash on this unit will not run that fast.

So the memory has to come from what the UI holds, not from where the code lives. That is what the
transcript cap, the canvas cap and shedding are for.
