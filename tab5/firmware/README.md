# Prebuilt firmware

Ready-to-flash images for the **M5Stack Tab5**, built with ESP-IDF 5.5.1 from the commit that last
changed this folder (`idf.py build`, then `esptool.py merge_bin`). If the source has moved on since, rebuild; see
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
9aa4ec05a15f8e8804931072159cecec46491058b3a799f33c74e45e66708624  catalyst-tab-merged.bin
2965e24341f7475c8c440e74447607f2d92e47e167bb157c86f95d8ff22aeed5  bootloader.bin
a9dd45b38158fe74aca40ab373f015fcc5a0bebe3aabb0f29cad0a2cc8f596eb  partition-table.bin
e2218abdafbd6ccacf083c09907a8c81aca55f31e12826695714b70b71c9f38c  catalyst_tab.bin
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

**Flash the merged image this time**, not just the app: the partition table changed (it now holds a
crash-dump area at `0xA10000`).

## If it doesn't start

The tablet keeps a record of how far each start got. After a start that crashes, hits a watchdog,
browns out or gets restarted, the next start does two things:
- It shows the record in amber under the Catalyst card, e.g. `LAST START: CRASH AT WI-FI · SAFE MODE`,
  with the crashing task and address underneath. The same line is appended to
  `catalyst-boot.txt` on the microSD card.
- It comes up in **safe mode**: solid glass, no frost.

After three failed starts in a row it also skips Wi-Fi and the USB tether, until one start runs for
20 seconds.

A photo of that amber text is enough for a fix. The serial log says the same and more: plug in the
USB-C port and open a monitor at 115200 baud. `idf.py -p <port> monitor` works, or the
Console tab at <https://espressif.github.io/esptool-js/>.

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
