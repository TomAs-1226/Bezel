# M5Stack Tab5 — what's in it, and what Catalyst Tab uses it for

Research notes for the firmware in this folder, gathered in September 2026. Sources are marked:
**[M]** M5Tab5-UserDemo at commit b4e356bc (its code is the ground truth when sources disagree),
**[U]** Espressif's upstream BSP `espressif/m5stack_tab5` 1.3.1, **[D]** docs.m5stack.com/en/core/Tab5
read as raw HTML, **[S]** the Tab5 schematic PDF. Anything not confirmed from one of them says
**unverified**.

## The chips, and the job each one gets

| Part | What it is | Catalyst Tab uses it for |
|---|---|---|
| **ESP32-P4NRW32** | Dual-core RISC-V HP at **360 MHz** (silicon rev v1.x; 400 MHz is v3.x only), 40 MHz LP core, FPU + PIE SIMD "AI extensions", 768 KB SRAM, **32 MB PSRAM** (hex mode, 200 MHz) in package, 16 MB flash [D][M] | Core 1: LVGL + the glass compositor. Core 0: NetworkTables, the CAN tap, tones, the camera and clip encoder, the USB tether, HTTPS workers. LVGL's software renderer runs two draw units, one per core. |
| **PPA** (pixel processing accelerator) | Scale-rotate-mirror (0/90/180/270°, 1/16-step scale), alpha blend, fill, in ARGB8888 / RGB565 / YUV [IDF docs] | Rotating each composed landscape frame into the portrait panel's scan-out buffer; blending the glass-ink layer over content; copying unchanged regions. |
| **2D-DMA** | Memory-to-memory DMA used by the DSI driver | Scan-out (`use_dma2d`). |
| **JPEG codec** | Baseline encode or decode, 720p at ~88 fps encode [IDF docs] | Lens snapshots to microSD. |
| **H.264 encoder** | Hardware, up to 1080p30, input "O_UYY_E_VYY" packed YUV 4:2:0 only [esp_h264] | Lens clips: each 1280×720 camera frame is converted RGB565 → YUV420 on the PPA and encoded at 30 fps, ~4 Mbit/s, an IDR a second, into an Annex-B `.h264` on microSD (`hal_clip_start`). See [Clips](#clips-h264). |
| **ISP + MIPI-CSI** | Via `esp_video` / `esp_ipa` | The Lens tool's camera pipeline. |
| **ESP32-C6-MINI-1U** co-processor | Wi-Fi 6 (2.4 GHz), BLE 5, 802.15.4; 4-bit SDIO to the P4 (CLK 12, CMD 13, D0–D3 11/10/9/8, RESET 15), stock slave firmware esp-hosted 1.4.1 [D][M] | The robot link over Wi-Fi, through `esp_wifi_remote` + `esp_hosted` — the normal `esp_wifi_*` API is forwarded over SDIO. 802.15.4 isn't exposed by esp-hosted. |
| **Display** | 5″ IPS, **720 × 1280 portrait**, 2-lane MIPI-DSI, RGB565, backlight PWM on GPIO22 | Rendered in **landscape 1280 × 720**: exactly Bezel's 720-high design, so Bezel's panel unit is 1. |
| **Touch** | GT911 @0x14 (ILI9881C units) **or** ST7123 @0x55 (from 2025-10) **or** ST7121 @0x55 (from 2026-04); 5 points; INT on GPIO23 [D][M] | Detected at boot the way both BSPs do it: probe 0x55 and read register 0 (1 = ST7121, 3 = ST7123), else 0x14 = GT911. |
| **BMI270** IMU @0x68 | 6-axis; no magnetometer; its interrupt goes to the power MCU, not the P4 [D][S] | The **Level** tool (inclinometer to check an arm's reported angle against gravity), Bezel's glass light leaning with tilt, and pick-up-to-wake. |
| **ES7210** 4-ch ADC @0x40 + dual mics | I2S DIN GPIO28, TDM 4 slots, 48 kHz [M] | **Not used.** Catalyst Tab has no tool that listens; the ES7210 is never configured. (The BSP's shared I2S bus still enables its receive channel, which captures nothing.) |
| **ES8388** codec @0x10 + NS4150B 1 W amp | I2S DOUT GPIO26, MCLK 30, BCLK 27, LRCK 29; amp enable on expander E1.P1 [M] | Detent ticks for dial detents and alert chimes (brownout, e-stop, a motor over temperature). |
| **SC2356** 2 MP camera | Driven as SC202CS @0x36 over 1-lane MIPI, 1280×720 RAW8 at 30 fps; CAM_RST E1.P6; 24 MHz XCLK on GPIO36 [M][U] | The **Lens** tool: look into a gearbox or behind a bellypan, freeze, snapshot to SD. |
| **RX8130CE** RTC @0x32 | Supercap-backed | Timestamps on logs and snapshots when there's no network time; set from the robot's NT server clock when connected. |
| **INA226** @0x41 | Tablet battery voltage/current, 5 mΩ shunt [M] | The tablet's own battery in the top island (percentage from voltage: there is no fuel gauge). |
| **Battery** | NP-F550, 2S 7.4 V, 2000 mAh; charges only when firmware sets CHG_EN (E2.P7) [D][M] | Charging is enabled at boot. |
| **2× PI4IOE5V6408** @0x43 (E1) / 0x44 (E2) | Expanders: antenna select, speaker enable, 5 V to Grove/M5-Bus, LCD/touch/camera resets, C6 power, USB-A VBUS, charge control, power-off pulse [M][D] | All of the above. **LCD_RST must be released as an input with pull-up, never driven high** [D]. |
| **microSD** | SDMMC 4-bit: CLK 43, CMD 44, D0–D3 39–42 [M] | Lens snapshots and clips, and reading Driver Station logs (`.wpilog`, `.dslog`, `.dsevents`). |
| **USB-A** | The P4's USB 2.0 **high-speed** OTG controller and its UTMI PHY (P4 pins 49/50 → 33 Ω pair R113/R114 → common-mode filter FT2 → J10); 5 V to the port switched by E2.P3 [S] | The **USB tether**: Systemcore's USB-C gadget through an A-to-C cable, or a CDC-ECM USB-Ethernet adapter into the robot's radio or switch. See [USB tether](#usb-tether). |
| **USB-C** | Full-speed PHY0 on GPIO24/25 = USB-Serial/JTAG (R111/R112 → FT1 → J8); VBUS feeds the charger, nothing switches 5 V out [S] | Flashing, the console, and power in. **It can't host**: it takes power, never supplies it, so the tether has to use the USB-A port. |
| **Grove Port A** | I2C1: SDA 53, SCL 54, switched 5 V | The **CAN tap**: with a Grove CAN transceiver unit on these two pins, the P4's TWAI controller listens (listen-only, never transmits) to a classic 1 Mbit/s robot CAN bus. |
| **RS-485** | SIT3088 on UART1 (TX 20, RX 21, DE 34); its 1.25 mm 6-pin connector also carries **SYS_VIN 6–24 V** [S] | Mainly a **power input**: the tablet can run off a robot's 12 V rail. The serial side isn't used — nothing on an FRC robot speaks RS-485. |
| **Buttons** | Power (press on, double press off), Reset/Boot | — |
| Not present | Ethernet, IR, magnetometer | — |

## Gotchas this firmware handles

1. **Silicon revision.** Tab5 units are v1.x. ESP-IDF ≥ 5.5.3 targets v3 by default; this project sets
   `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` where the IDF has that option, and keeps 360 MHz.
2. **Three panels, all at ~60.5 Hz.** ILI9881C, ST7123, ST7121, told apart by the touch controller at
   boot. The BSP's timing gives 48.2 / 66.1 / 65.5 Hz — the DPI clock is PLL_F240M over an integer
   divider rounded down, so the BSP's "70 MHz" for the ST712x is really 80 MHz, and its 60 MHz for the
   ILI9881C is 60. Catalyst Tab runs all three from an 80 MHz pixel clock with the BSP's vertical
   timing and a wider horizontal back porch, logging the rate at boot:

   | Panel | BSP: clock, total → rate | Catalyst Tab: HBP, total → rate |
   |---|---|---|
   | ILI9881C | 60 MHz, 940 × 1324 → 48.2 Hz | 198, 998 × 1324 → 60.54 Hz |
   | ST7123 | 80 MHz, 802 × 1510 → 66.1 Hz | 113, 875 × 1510 → 60.55 Hz |
   | ST7121 | 80 MHz, 802 × 1524 → 65.5 Hz | 105, 867 × 1524 → 60.55 Hz |

   RGB565 at 80 MHz needs 1280 Mbit/s of the two lanes' 2000 (1930 on the ST7121). The BSP keeps these
   configs private, so tab_hal links with `--wrap=esp_lcd_new_panel_dpi` and adjusts the config on its
   way into esp_lcd. `CATALYST_BSP_PANEL_TIMING` restores the BSP's. UNVERIFIED on all three panels.
3. **Landscape without tearing.** LVGL's port can't combine software rotation with direct mode or
   tear avoidance. Catalyst Tab doesn't use the port: it composes a landscape frame itself and has the
   PPA rotate only the changed rectangles into the back of two DPI frame buffers, swapped on vsync.
   `hal_present()` is asynchronous: it queues the rotations as non-blocking PPA transactions (last
   frame's areas, then this frame's) and returns; a core-0 task waits for the PPA's completion
   interrupts, hands the buffer to the DPI controller and waits for the vsync. At most one frame is in
   flight, so the UI core composes frame N+1 while the PPA turns frame N.
4. **PSRAM bandwidth** is the budget: scan-out alone reads ~112 MB/s at 60.5 Hz. Nothing is recomposed
   that didn't change; glass blur is rebuilt only when the content under it changes.
5. **The PPA and the cache.** Before a transaction the PPA driver invalidates the output's cache lines
   over whole rows, which would discard nearby CPU writes (or, past a buffer's end, someone else's).
   The HAL only lets the PPA write inside the heap block that holds the destination (found once with
   `heap_caps_walk()`), writes that window back first, and finishes on the CPU whatever the window
   can't cover. Each task that uses the PPA has its own client: a client queues one blocking
   transaction at a time, and a second caller would be refused, not queued.
6. **Charging** needs CHG_EN set by firmware, and the upstream BSP's Wi-Fi feature call is reported to
   reinitialise expander 0x44 and clear it — this firmware drives the expanders itself.
7. **The LP core** can't see the IMU or RTC interrupts (they go to a PMS150G power MCU), so "sleep" on
   a Tab5 is a timed or IMU-triggered power-on. The LP core isn't used.
8. **After a hard power loss** wait 5 s before powering on, or the IMU may not initialise [D].

## Pin map

| Function | GPIO |
|---|---|
| System I2C SDA / SCL (0x10 0x14 0x32 0x36 0x40 0x41 0x43 0x44 0x55 0x68) | 31 / 32 |
| Grove Port A SDA / SCL | 53 / 54 |
| LCD backlight | 22 |
| Touch INT | 23 |
| I2S MCLK / BCLK / LRCK / DOUT / DIN (DIN: the unused ES7210) | 30 / 27 / 29 / 26 / 28 |
| Camera XCLK | 36 |
| C6 SDIO CLK / CMD / D0 / D1 / D2 / D3, RESET | 12 / 13 / 11 / 10 / 9 / 8, 15 |
| microSD CLK / CMD / D0–D3 | 43 / 44 / 39–42 |
| RS-485 TX / RX / DE | 20 / 21 / 34 |
| USB-C (Serial/JTAG) | 24 / 25 |
| M5-Bus UART0 TX / RX, SPI MOSI / MISO / SCK | 37 / 38, 18 / 19 / 5 |

## Expander bits

| Pin | 0x43 (E1) | 0x44 (E2) |
|---|---|---|
| P0 | antenna: low internal, high MMCX | C6 power (WLAN_PWR_EN) |
| P1 | speaker amp enable | — |
| P2 | EXT 5 V (Grove, M5-Bus, header) | — |
| P3 | — | USB-A VBUS |
| P4 | LCD reset (release as input + pull-up) | power-off pulse |
| P5 | touch reset | quick-charge enable (active low) |
| P6 | camera reset | charge status (input) |
| P7 | headphone detect (input) | charge enable |

## USB tether

The USB-A port is a USB 2.0 high-speed host (the P4's HS OTG controller; the USB host library's default
controller on the P4). `hal_tab5_net.c` runs esp-iot-solution's host network drivers on it:

| Component | Version | Role |
|---|---|---|
| `espressif/iot_usbh_cdc` | ~3.1.0 | CDC host transport (bulk + notification endpoints), hotplug |
| `espressif/iot_usbh_rndis` | ~0.5.0 | RNDIS (an interface association: E0/01/03 or EF/04/01) |
| `espressif/iot_usbh_ecm` | ~0.4.0 | CDC-ECM (interface 02/06) |
| `espressif/iot_eth` | ~1.1.0 | the glue from either driver to `esp_netif` |

What each kind of device gets:

- **Systemcore's USB-C port** (A-to-C cable). Systemcore's `limelight_gadget` service configures a
  configfs composite of **ECM** (`usb0`, DHCP in 172.27.0–15.x) and **RNDIS** (`usb1`, DHCP in
  172.26.0–15.x, the side Console reaches at 172.26.0.1) — [dunkirk.sh, "Reverse engineering the FRC
  SystemCore image"](https://dunkirk.sh/blog/frc-systemcore-image/). Both functions in one device would
  otherwise come up as two interfaces, so a gate callback closes the ECM driver's match list to any
  device that also offers RNDIS: **RNDIS wins**, and the tablet lands on 172.26.x, where the addresses
  in [catalyst-contract.md](catalyst-contract.md) expect Systemcore. RNDIS is also the function a
  Windows-first gadget puts in configuration 1, the only one the host library enumerates by default.
  UNVERIFIED on a real Systemcore: whether both functions share configuration 1, and the exact subnet.
- **CDC-ECM USB-Ethernet adapters** into the robot radio or switch: ECM.
- **Realtek RTL8152/8153/8156 adapters**: vendor-specific in configuration 1, CDC-ECM in configuration 2
  on most firmwares; an enumeration filter (`CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK`) selects
  configuration 2 for Realtek's vendor id. UNVERIFIED per adapter.
- **ASIX AX88179/AX88772**: vendor protocol only, no ESP-IDF driver. `hal_tether()` reports the chip by
  name (`kind = "ax88179"`) with `up = false`, so the UI can say why nothing happens.
- **CDC-NCM**: no ESP-IDF host driver exists, and none is written here. A gadget that offers only NCM
  (some postmarketOS/Linux defaults) won't come up.

**Addressing.** DHCP first. With no lease after 6 s the fallback set by `hal_tether_fallback()` (for
example `10.TE.AM.60/24`) goes on and stays until the cable comes out; unplugging re-arms DHCP.

**Routing.** Each interface gets an `esp_netif` route priority: Wi-Fi STA 100, the tether 20. esp_netif
makes the highest-priority interface that is up lwIP's *default* netif, so the default route — the PC,
the internet, the Claude API — stays on Wi-Fi, and only the tether's own subnet goes out the cable.
lwIP's `ip4_route()` picks the *first* interface in `netif_list` whose subnet holds the destination,
before it considers the default; when both interfaces share a subnet (Wi-Fi to the robot radio and a
dongle into the robot's switch, both 10.TE.AM.0/24), the tether is moved to the head of that list every
time it (or Wi-Fi) gets an address, so the robot link wins. DNS follows the default netif
(`CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF`): a DHCP lease from Systemcore can't replace the Wi-Fi
network's DNS server. mDNS runs on the tether too, so `robot.local` answers over the cable.

**Counters.** `rx_bytes`/`tx_bytes` count Ethernet frames between the driver and lwIP. `mbps` is the USB
bus rate (480 on a high-speed link): the drivers don't expose the adapter's own Ethernet link speed.

## Networking: HTTPS and workers

- `hal_http_*` sits on `esp_http_client`: open, write the body, fetch the headers, then read. Each read
  waits (with the request's timeout) for the first byte and then takes only what has already arrived,
  so Server-Sent Events from the Claude API reach the assistant as they're sent; the client's parser
  removes chunked framing. A body with neither `Content-Length` nor chunking (close-delimited) isn't
  supported by `esp_http_client` and reads as empty.
- TLS is verified against the full ESP-IDF CA bundle (`esp_crt_bundle_attach`). Every mbedTLS
  allocation is in PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`), so a session costs internal RAM only for
  its task's stack.
- `hal_thread()` makes a FreeRTOS task on core 0 at priority 4 (under the NetworkTables client's 5) with
  its stack in **internal RAM**: a worker may write NVS, and while flash is written the cache — and PSRAM
  with it — is off. Only if internal RAM can't hold the stack does it go to PSRAM, with a warning;
  `hal_kv_set()` refuses to run on such a stack rather than fault.

## Clips (H.264)

`hal_clip_start()` needs the camera running. The camera task converts each 1280×720 RGB565 frame on the
PPA into one of two YUV420 buffers in PSRAM (limited range, BT.709) and hands it to a core-0 worker that
runs `espressif/esp_h264` 1.0.4's hardware encoder (the version esp_video 2.0.1 pins; esp_video's own
V4L2 H.264 device wraps the same encoder) and appends the NAL units to the file. 30 fps, ~4 Mbit/s, QP
20–40, an IDR with SPS/PPS every second. If the worker falls behind, frames are dropped, never queued;
the Lens preview keeps its own path throughout. About 4.2 MB of PSRAM while recording. Play with
`ffplay clip.h264`, or box it: `ffmpeg -framerate 30 -i clip.h264 -c copy clip.mp4`.

## First-boot checklist

Everything below has only been compiled; each item is marked `UNVERIFIED` in the source.

1. **USB tether, Systemcore**: plug the A-to-C cable in; the log should show `USB-A: …, config 1: rndis
   ecm`, then `tether rndis: link up` and a 172.26.x address. If Systemcore's gadget puts RNDIS and ECM
   in separate configurations, only configuration 1 enumerates.
2. **USB tether, adapters**: a CDC-ECM dongle should get a lease from the robot network; a Realtek
   dongle should enumerate configuration 2 (`config 2: ecm`). Check `hal_tether()`'s MAC and counters.
3. **Routing with both links**: with Wi-Fi on the robot radio and a dongle on the robot switch, pings to
   10.TE.AM.2 should leave by USB (tx counter rising) while HTTPS to api.anthropic.com still works.
4. **DHCP fallback**: on a network with no DHCP server the fallback address should appear after ~6 s.
5. **HTTPS streaming**: an SSE response should arrive event by event, not in 4 KB lumps.
6. **Clips**: the PPA's YUV420 output layout is assumed to be the encoder's `O_UYY_E_VYY` (wrong would
   show as scrambled colour); check the file plays in ffplay/VLC from its start.
7. **Panel timing**: the boot log prints `panel …: BSP timing … running 80 MHz, … -> 60.5x Hz`. A rolling,
   torn or blank picture means that panel refuses the wider line: build with `CATALYST_BSP_PANEL_TIMING`
   and report which panel.
8. **Asynchronous present**: no `present: … overdue` warnings in the log while animating, and no stale
   rectangles after fast scrolling (the catch-up re-rotates last frame's areas from their own sources).
9. **PPA addressing**: rotations and compositor copies now read from the first pixel of each area (any
   2-byte address, described from a 64- or 4-byte aligned base) and write only inside the destination's
   heap block. Garbled or shifted rectangles would point here; the boot log's in-place blend self-test
   exercises the same path.
