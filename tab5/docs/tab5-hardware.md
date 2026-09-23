# M5Stack Tab5 — what's in it, and what Catalyst Tab uses it for

Research notes for the firmware in this folder, gathered in September 2026. Sources are marked:
**[M]** M5Tab5-UserDemo at commit b4e356bc (its code is the ground truth when sources disagree),
**[U]** Espressif's upstream BSP `espressif/m5stack_tab5` 1.3.1, **[D]** docs.m5stack.com/en/core/Tab5
read as raw HTML, **[S]** the Tab5 schematic PDF. Anything not confirmed from one of them says
**unverified**.

## The chips, and the job each one gets

| Part | What it is | Catalyst Tab uses it for |
|---|---|---|
| **ESP32-P4NRW32** | Dual-core RISC-V HP at **360 MHz** (silicon rev v1.x; 400 MHz is v3.x only), 40 MHz LP core, FPU + PIE SIMD "AI extensions", 768 KB SRAM, **32 MB PSRAM** (hex mode, 200 MHz) in package, 16 MB flash [D][M] | Core 1: LVGL + the glass compositor. Core 0: NetworkTables, CAN tap, audio DSP, sensors. LVGL's software renderer runs two draw units, one per core. |
| **PPA** (pixel processing accelerator) | Scale-rotate-mirror (0/90/180/270°, 1/16-step scale), alpha blend, fill, in ARGB8888 / RGB565 / YUV [IDF docs] | Rotating each composed landscape frame into the portrait panel's scan-out buffer; blending the glass-ink layer over content; copying unchanged regions. |
| **2D-DMA** | Memory-to-memory DMA used by the DSI driver | Scan-out (`use_dma2d`). |
| **JPEG codec** | Baseline encode or decode, 720p at ~88 fps encode [IDF docs] | Lens snapshots to microSD. |
| **H.264 encoder** | Hardware, up to 1080p30 | Not used yet (reserved for recording a Lens clip). |
| **ISP + MIPI-CSI** | Via `esp_video` / `esp_ipa` | The Lens tool's camera pipeline. |
| **ESP32-C6-MINI-1U** co-processor | Wi-Fi 6 (2.4 GHz), BLE 5, 802.15.4; 4-bit SDIO to the P4 (CLK 12, CMD 13, D0–D3 11/10/9/8, RESET 15), stock slave firmware esp-hosted 1.4.1 [D][M] | The robot link over Wi-Fi, through `esp_wifi_remote` + `esp_hosted` — the normal `esp_wifi_*` API is forwarded over SDIO. 802.15.4 isn't exposed by esp-hosted. |
| **Display** | 5″ IPS, **720 × 1280 portrait**, 2-lane MIPI-DSI, RGB565, backlight PWM on GPIO22 | Rendered in **landscape 1280 × 720**: exactly Bezel's 720-high design, so Bezel's panel unit is 1. |
| **Touch** | GT911 @0x14 (ILI9881C units) **or** ST7123 @0x55 (from 2025-10) **or** ST7121 @0x55 (from 2026-04); 5 points; INT on GPIO23 [D][M] | Detected at boot the way both BSPs do it: probe 0x55 and read register 0 (1 = ST7121, 3 = ST7123), else 0x14 = GT911. |
| **BMI270** IMU @0x68 | 6-axis; no magnetometer; its interrupt goes to the power MCU, not the P4 [D][S] | The **Level** tool (inclinometer to check an arm's reported angle against gravity), Bezel's glass light leaning with tilt, and pick-up-to-wake. |
| **ES7210** 4-ch ADC @0x40 + dual mics | I2S DIN GPIO28, TDM 4 slots, 48 kHz [M] | The **Listen** tool: an FFT of a mechanism's sound — gear mesh, bearing whine, a slipping belt — with the dominant frequency turned into RPM. |
| **ES8388** codec @0x10 + NS4150B 1 W amp | I2S DOUT GPIO26, MCLK 30, BCLK 27, LRCK 29; amp enable on expander E1.P1 [M] | Detent ticks for dial detents and alert chimes (brownout, e-stop, a motor over temperature). |
| **SC2356** 2 MP camera | Driven as SC202CS @0x36 over 1-lane MIPI, 1280×720 RAW8 at 30 fps; CAM_RST E1.P6; 24 MHz XCLK on GPIO36 [M][U] | The **Lens** tool: look into a gearbox or behind a bellypan, freeze, snapshot to SD. |
| **RX8130CE** RTC @0x32 | Supercap-backed | Timestamps on logs and snapshots when there's no network time; set from the robot's NT server clock when connected. |
| **INA226** @0x41 | Tablet battery voltage/current, 5 mΩ shunt [M] | The tablet's own battery in the top island (percentage from voltage: there is no fuel gauge). |
| **Battery** | NP-F550, 2S 7.4 V, 2000 mAh; charges only when firmware sets CHG_EN (E2.P7) [D][M] | Charging is enabled at boot. |
| **2× PI4IOE5V6408** @0x43 (E1) / 0x44 (E2) | Expanders: antenna select, speaker enable, 5 V to Grove/M5-Bus, LCD/touch/camera resets, C6 power, USB-A VBUS, charge control, power-off pulse [M][D] | All of the above. **LCD_RST must be released as an input with pull-up, never driven high** [D]. |
| **microSD** | SDMMC 4-bit: CLK 43, CMD 44, D0–D3 39–42 [M] | Lens snapshots, Listen captures, and reading Driver Station logs (`.wpilog`, `.dslog`, `.dsevents`). |
| **USB-A** | The P4's USB 2.0 **high-speed** OTG PHY, VBUS on E2.P3 [S] | Experimental: a wired robot link through a USB-Ethernet adapter that speaks CDC-ECM (esp-iot-solution's `iot_eth` ECM/RNDIS host). Needed at events, where Wi-Fi to the robot isn't allowed. Unverified per adapter. |
| **USB-C** | Full-speed PHY0 on GPIO24/25 = USB-Serial/JTAG | Flashing and the console. |
| **Grove Port A** | I2C1: SDA 53, SCL 54, switched 5 V | The **CAN tap**: with a Grove CAN transceiver unit on these two pins, the P4's TWAI controller listens (listen-only, never transmits) to a classic 1 Mbit/s robot CAN bus. |
| **RS-485** | SIT3088 on UART1 (TX 20, RX 21, DE 34); its 1.25 mm 6-pin connector also carries **SYS_VIN 6–24 V** [S] | Mainly a **power input**: the tablet can run off a robot's 12 V rail. The serial side isn't used — nothing on an FRC robot speaks RS-485. |
| **Buttons** | Power (press on, double press off), Reset/Boot | — |
| Not present | Ethernet, IR, magnetometer | — |

## Gotchas this firmware handles

1. **Silicon revision.** Tab5 units are v1.x. ESP-IDF ≥ 5.5.3 targets v3 by default; this project sets
   `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` where the IDF has that option, and keeps 360 MHz.
2. **Three panels.** ILI9881C (≈48 Hz), ST7123 (≈58 Hz), ST7121 (≈57 Hz), told apart by the touch
   controller at boot.
3. **Landscape without tearing.** LVGL's port can't combine software rotation with direct mode or
   tear avoidance. Catalyst Tab doesn't use the port: it composes a landscape frame itself and has the
   PPA rotate only the changed rectangles into the back of two DPI frame buffers, swapped on vsync.
4. **PSRAM bandwidth** is the budget: scan-out alone reads ~107 MB/s. Nothing is recomposed that
   didn't change; glass blur is rebuilt only when the content under it changes.
5. **Charging** needs CHG_EN set by firmware, and the upstream BSP's Wi-Fi feature call is reported to
   reinitialise expander 0x44 and clear it — this firmware drives the expanders itself.
6. **The LP core** can't see the IMU or RTC interrupts (they go to a PMS150G power MCU), so "sleep" on
   a Tab5 is a timed or IMU-triggered power-on. The LP core isn't used.
7. **After a hard power loss** wait 5 s before powering on, or the IMU may not initialise [D].

## Pin map

| Function | GPIO |
|---|---|
| System I2C SDA / SCL (0x10 0x14 0x32 0x36 0x40 0x41 0x43 0x44 0x55 0x68) | 31 / 32 |
| Grove Port A SDA / SCL | 53 / 54 |
| LCD backlight | 22 |
| Touch INT | 23 |
| I2S MCLK / BCLK / LRCK / DOUT / DIN | 30 / 27 / 29 / 26 / 28 |
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
