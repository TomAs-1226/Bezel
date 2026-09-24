# CatalystOS

CatalystOS is what the Tab5 firmware is becoming: a small operating system for a pocket tablet, robotics
first but general purpose. This page is its architecture: what each layer is, why it is built that way,
the numbers it rests on, and what is done and what is next.

## What "OS" means here, and what it doesn't

The goal is the fluidity of a phone-class OS on a microcontroller (Xiaomi's Vela is the reference the
owner named). Vela is built on NuttX, an existing RTOS kernel; what makes it fluid is not the kernel
but the graphics pipeline above it. The same is true here, so:

- **The kernel stays FreeRTOS (SMP, both cores), as shipped in ESP-IDF.** Its scheduler switches tasks in
  microseconds and nothing measured on this device has ever been limited by it. Writing a new kernel
  would cost months and buy nothing a user can see.
- **Everything above the kernel is CatalystOS's own**: the task and core plan, the display compositor,
  the frame scheduler, the services (power, notifications, storage, network, time, the assistant), the
  shell (lock screen, control center, app library) and the app model.
- **Language: C, with the hot loops in the P4's own SIMD (PIE) where it pays.** C is the fastest
  language the toolchain supports on this chip; Rust support for the ESP32-P4 is not mature enough to
  bet a firmware on. The speed that matters comes from not touching pixels, not from the language.

## The hardware budget

Measured on the tablet, not taken from datasheets:

| Resource | Measured | What it means |
|---|---|---|
| Panel | 720 × 1280 portrait, MIPI-DSI, 60 Hz | the UI is landscape: every pixel that changes must be turned 90° |
| PPA rotate (scale-rotate-mirror engine) | 15–20 Mpx/s at any angle | a full frame takes ~50 ms: ~20 fps if every pixel changes. Confirmed as the engine's practical ceiling (docs/research) |
| DMA2D block copy (no rotation) | ~67 Mpx/s | a full frame in ~14 ms: moving pictures that are already turned is cheap |
| CPU copy PSRAM → PSRAM | ~40 MB/s each way | never copy frames with the CPU |
| Internal SRAM | 768 KB, ~12–25 KB free | the scarce resource: everything big lives in the 32 MB PSRAM |
| PSRAM | 32 MB at 200 MHz | frame buffers, pictures of screens, app data |

At 60 fps a frame is 16.7 ms, so a frame may **rotate about 300k pixels** (a quarter of the screen) or
**move about 1 M already-turned pixels**. The whole design follows from those two numbers.

## Layers

### 1. Kernel and task plan

| Core | Tasks |
|---|---|
| 1 | the UI task (LVGL drawing, the shell, the compositor's frame), the boot card |
| 0 | NetworkTables client, Wi-Fi co-processor link (esp-hosted), lwIP, USB tether, camera, CAN, tone, workers (`hal_thread`) |

Any unpinned task above priority 9 is forced onto core 0 at link time (`__wrap_xTaskCreatePinnedToCore`)
so network bursts never steal a frame. Start-up is recorded stage by stage (`hal_boot_*`); after failed
starts the next one comes up in safe mode.

### 2. The display pipeline: the Catalyst compositor

Two paths, chosen per motion:

- **Redraw path** (things that change): LVGL draws only what changed, in landscape; each changed area is
  turned onto the panel by the PPA, or by the CPU when it is a sliver (a rotation a few pixels thin hangs
  the PPA; see `rotate_area`). Presents are paced by vsync and double-buffered; the next frame is drawn
  while the panel scans the last one.
- **Composition path** (things that move): a surface that moves as a whole (a page, an app, the control
  center, the lock screen) is a picture already in the panel's orientation, and each frame is a few DMA2D
  rectangle copies. Nothing is drawn or rotated per frame. A landscape row is a portrait column, so any
  horizontal slide or any vertical sheet is a handful of rectangles.

  | Surface | Motion | Status |
  |---|---|---|
  | pages | horizontal slide under the fixed dock and status bar | done (~58 fps) |
  | control center | top sheet, follows the finger | done |
  | lock screen | top sheet, pushed up | done |
  | apps | bottom sheet, up to open, down to close | done |
  | banners, keyboard, app switcher | sheets / slides | next |

  The picture a moving surface reveals is drawn offscreen a band at a time, only as it comes into view,
  so starting a motion costs a few milliseconds, not a full redraw. At rest the real surface takes over
  without being sent again: the panel's second buffer is synced by DMA.

### 3. Services

| Service | Where | Notes |
|---|---|---|
| power | `ui_shell.c` (SLP), `ui_lock.c` | dim, sleep, wake (the waking tap presses nothing), lock screen |
| notifications | `ui_lock.c` | every island message kept; listed in the control center and on the lock screen |
| settings | `hal_kv_*` (NVS) | one key per setting |
| storage | `ui_storage.c` | microSD layout `/sdcard/CATOS/{DOCS,PHOTOS,AUDIO,DATA,LOGS}` |
| network | `hal_tab5_net.c` | Wi-Fi (C6 over SDIO), USB tether, mDNS, HTTPS streaming |
| time | SNTP, RTC | alarms ring with the app closed (`ui_os_boot`) |
| robot | `components/catalyst` (NT4 model) | see docs/catalyst-integration.md for the library contract |
| assistant | `components/assist`, Catalyst Link | Claude via the PC or a key; GPT with a key (branch tab5-companion) |

### 4. The shell

Home, robot, devices, power, motion and the app library as pages; the status bar and the island (status
at rest, messages as they come); the control center (a pull from the top edge anywhere); the lock screen;
the orb (the assistant, over the pages only).

### 5. Apps

Today every app is built in: a `ui_app_t` with build/open/close/refresh/frame, 33 of them in four groups
(robot, diagnose, everyday, this tablet).

Next, apps installed from the microSD card: native code loaded with Espressif's `elf_loader` component
(supports the P4 and running from PSRAM), calling a stable C API (`catos.h`: the LVGL subset, the shell's
widgets, the services above) through an exported symbol table. Native code rather than WebAssembly
because WASM interpreters and even AOT runtimes are markedly slower, and the UI is the hot path. There is
no memory protection between apps on this chip: an installed app is trusted code.

## Roadmap

| Phase | What | Status |
|---|---|---|
| 1 | Boot, recovery, lean renderer, dirty-area presents, page slides in DMA2D | done |
| 2 | Compositor sheets: control center, lock, apps; system edge gestures | done |
| 3 | Notifications, lock screen, sleep/wake, everyday apps, card layout | done |
| 4 | Pipelined present: the PPA/DMA2D present on core 0 while core 1 draws the next frame | next |
| 5 | PIE-accelerated fills, blends and copies in the redraw path (~1.4× on PSRAM copies, per Espressif's numbers) | next |
| 6 | More compositor surfaces: banners, keyboard, an app switcher of live cards | next |
| 7 | Apps from the microSD card (`elf_loader`, `catos.h`, a manifest per app) | planned |
| 8 | Assets on the card: wallpapers, extra fonts and icon sets, loaded at start | planned |
| 9 | Audio playback (WAV/MP3 from the card) through the ES8388 codec | planned |

Each phase is measured on the tablet before it counts as done: frame rate from the heartbeat, frame
cost by stage, and screenshots of the panel.
