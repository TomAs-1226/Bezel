# Prebuilt firmware

Ready-to-flash images for the **M5Stack Tab5**, built with ESP-IDF 5.5.1 from commit `277448c`
(`idf.py build`, then `esptool.py merge_bin`). If the source has moved on since, rebuild; see
[../README.md](../README.md#building-the-firmware).

| File | Flash at | What |
|---|---|---|
| `catalyst-tab-merged.bin` | `0x0` | Everything in one file: bootloader, partition table, app |
| `bootloader.bin` | `0x2000` | Bootloader |
| `partition-table.bin` | `0x8000` | Partition table |
| `catalyst_tab.bin` | `0x10000` | The app alone |

Flash settings: DIO, 80 MHz, 16 MB.

SHA-256:

```
61b3460e4ad03f60afd74ea5aa370e0268792c3fa04bb537a0982433f221e9c3  catalyst-tab-merged.bin
6955e009009e869fd0a3288263cc3a7dd855a1a8aaf4ec6ef1bf5a89f8c2d52b  bootloader.bin
ef38c1c8bdb24a2006840e0b411327c92ac382d3138835ada6b6a8448d0db3a7  partition-table.bin
9077d533968a2802735224c44d0cfc761ad239812d6e45e587dc69ff926231ba  catalyst_tab.bin
```

## Flash and go

Plug the Tab5's **USB-C** port into your computer. That port is the P4's USB-Serial/JTAG; the USB-A
port is for the robot tether and can't flash.

**From a terminal** (`pip install esptool`):

```sh
esptool.py --chip esp32p4 -b 921600 write_flash 0x0 catalyst-tab-merged.bin
```

Add `-p <port>` if it doesn't find the tablet by itself: `COM5` on Windows,
`/dev/cu.usbmodem…` on macOS, `/dev/ttyACM0` on Linux.

**From a browser** (Chrome or Edge, nothing to install): open
<https://espressif.github.io/esptool-js/>. Click **Connect** and pick the Tab5, set the flash
address to `0x0`, choose `catalyst-tab-merged.bin`, then click **Program**.

If the tablet won't connect, it isn't in download mode: put it into download mode with the
reset/boot button as M5Stack's Tab5 documentation describes, then try again. After flashing, press
reset or unplug and replug to start it.

## Updating later

The merged image covers the settings area (`nvs` at `0x9000`), so flashing it **erases saved
settings**: team, robot address, Wi-Fi, the Link pairing. To update and keep them, flash the app
alone:

```sh
esptool.py --chip esp32p4 -b 921600 write_flash 0x10000 catalyst_tab.bin
```

## On first boot

1. Open **tools → settings** and set your team number.
2. Plug the robot into the USB-A port: the Systemcore's USB-C with an A-to-C cable, or a USB-Ethernet
   dongle into the radio. Or join the robot's Wi-Fi from settings.
3. Optional: pair **Catalyst Link** on the PC from **tools → link** to enable the assistant, patches and
   work orders ([../link/README.md](../link/README.md)).

This image has been built and simulated but **has never run on a Tab5**. If something's off
(orientation, touch, IMU axes, the CAN tap pins), the fixes are listed under
[First boot](../README.md#first-boot). The serial log is the quickest way to see what happened:
`idf.py -p <port> monitor`, or any serial terminal at 115200 baud.
