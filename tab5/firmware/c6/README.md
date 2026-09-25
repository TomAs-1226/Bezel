# The Wi-Fi co-processor's firmware (ESP32-C6)

The Tab5's Wi-Fi is an ESP32-C6 on an SDIO link, running Espressif's **esp-hosted** slave. The tablet's
own firmware (the host side) is esp-hosted 1.4.7 as a local component, `components/espressif__esp_hosted`;
the C6 must run the matching slave, built with the settings here.

| File | What |
|---|---|
| `network_adapter.bin` | The slave, esp-hosted 1.4.7, built with ESP-IDF 5.5.1 for `esp32c6` |
| `sdkconfig.defaults.esp32c6` | Its settings: the stock file plus the Catalyst Tab block at the end |

SHA-256:

```
9e80199b9a00f38206c329c746e228070a4245db988ff498687519f4d1f2ac87  network_adapter.bin
```

## What differs from stock, and why

- **Packet mode, not stream mode** (`CONFIG_ESP_SDIO_STREAMING_MODE=n`). The host reads one fixed 1.5 KB
  transfer per packet (`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_MAX_SIZE`, chosen because stream mode's
  growing receive buffer ran internal RAM out during a scan). A slave in stream mode lets one read carry
  several packets, or part of one; the host kept only the first packet of each read, the two sides' byte
  counts drifted apart, and within minutes of steady traffic the C6 stopped sending anything, RPC replies
  included, while "connected". With both sides on one packet per read, a 30-round stress (Blue Alliance plus a
  companion question each round) that hung the C6 five times before ran with no hang at all. How it was found:
  the dev console's `wdhold 1` keeps a hang for looking at, `c6dbg` reads the slave's SDIO registers (its
  packet-length register frozen, 20 send buffers free), and `c6kick N` forces reads (which freed it).
- **No Bluetooth** (`CONFIG_BT_ENABLED=n`): unused.
- **SDIO default speed** (`CONFIG_ESP_SDIO_DEFAULT_SPEED=y`) and the host at 20 MHz: 40 MHz failed sooner.
- **Task watchdog panics** (`CONFIG_ESP_TASK_WDT_PANIC=y`): a stuck C6 task resets it.
- Two source patches for ESP-IDF 5.5, in the slave's `main/`: `CMakeLists.txt` defines
  `H_WIFI_VHT_FIELDS_AVAILABLE=1 H_WIFI_NEW_RESERVED_FIELD_NAMES=1` (5.5's `wifi_sta_config_t`), and
  `slave_control.c` uses `reserved1` where 5.5 renamed `reserved`.

## Building it

The slave's source is the `slave/` folder of esp-hosted-mcu **1.4.7** (the release the host component is).

```sh
cd slave
cp <this folder>/sdkconfig.defaults.esp32c6 .
# apply the two patches above
idf.py set-target esp32c6
idf.py build          # build/network_adapter.bin
```

## Installing it (over the air, from the tablet)

The C6 has no USB of its own: the tablet updates it over the SDIO link.

```sh
python tools/tab5_dev.py COM9 "put firmware/c6/network_adapter.bin /sdcard/CATOS/DATA/C6.BIN"
python tools/tab5_dev.py COM9 "c6ota /sdcard/CATOS/DATA/C6.BIN"
```

`put` checks the copy by CRC-32; `c6ota` writes the C6's other OTA slot, switches to it once the whole image
checks out, and restarts the tablet (a planned restart: it comes back where it was). The boot log then says
`wi-fi: the C6 runs esp-hosted 1.4.7`. If `c6ota` stalls after "starting", reset the tablet and run it
again: the C6 must be answering to take an update.
