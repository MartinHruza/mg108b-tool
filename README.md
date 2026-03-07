# MonsGeek MG108B Linux Tool

Reverse-engineered Linux CLI tool for configuring and flashing the MonsGeek MG108B keyboard, replacing the Windows-only official driver.

## What's here

- `flash_mg108b.c` — Standalone C tool using libusb for keyboard configuration and firmware flashing
- `download_firmware.sh` — Script to download the latest firmware from MonsGeek's API
- `driver.md` — Reverse-engineered USB HID protocol documentation
- `firmware.md` — Firmware binary analysis (YiChip YC31xx, ARM Cortex-M0)

## Building

```bash
sudo apt install libusb-1.0-0-dev
make
```

## Usage

Requires root for USB access.

```
sudo ./flash_mg108b get-version
./flash_mg108b get-battery
./flash_mg108b get-rate
./flash_mg108b set-rate <125|250|500|1000>
./flash_mg108b get-debounce
./flash_mg108b set-debounce <1-10>
./flash_mg108b get-info
./flash_mg108b factory-reset
./flash_mg108b flash <firmware.raw>
```

Download and flash firmware (**untested — use at your own risk**):

```bash
./download_firmware.sh
./flash_mg108b flash mg108b_firmware_v108.raw
```
