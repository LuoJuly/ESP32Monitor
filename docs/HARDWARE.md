# Hardware

This firmware targets the Waveshare **ESP32-S3-Touch-AMOLED-1.75C**.

Product page: https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75C/

## What is on the board

| Part | Detail |
|------|--------|
| MCU | ESP32-S3R8, 240 MHz, 8 MB octal PSRAM, 32 MB flash |
| Panel | 1.75 inch 466×466 AMOLED, driver CO5300, QSPI |
| Touch | CST9217 on I2C |
| Power | AXP2101, USB-C, optional 3.7 V battery |
| Link to the PC | Native USB Serial/JTAG. One cable is enough to flash and to receive stats |

There is no separate display to wire. The panel is already connected:

| Signal | GPIO |
|--------|------|
| QSPI D0–D3 | 4, 5, 6, 7 |
| QSPI clock | 38 |
| LCD CS | 12 |
| LCD reset | 1 |
| Touch reset / interrupt | 2 / 11 |
| I2C SDA / SCL | 15 / 14 |

Those pins are claimed by the official board package. Leave them alone.

## What the screen shows

The original project used four 240×240 round LCDs. This board has one square panel, so the same four views sit in quadrants:

```
┌──────────┬──────────┐
│   CPU    │   GPU    │
├──────────┼──────────┤
│   RAM    │   NET    │
└──────────┴──────────┘
```

Corners of the glass are rounded. Gauges are centered in each quadrant so the clipped corner does not cover the numbers.

## Flash

ESP-IDF 5.5 or newer is required. From this directory, with the IDF environment loaded:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

`idf.py set-target` is only needed the first time. The partition table is sized for 32 MB flash (two 6 MB app slots plus LittleFS). A board that was previously flashed with the 16 MB table needs one full flash; after that, the PC app can update firmware over USB.

## Power

USB-C from the PC powers the board and carries the serial protocol at 115200 8N1. The Windows client scans COM ports and sends `WHO_ARE_YOU?`. No port has to be configured by hand.
