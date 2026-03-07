# MonsGeek MG108B Linux Tool

Reverse-engineered Linux CLI tool for configuring and flashing the MonsGeek MG108B keyboard, replacing the Windows-only official driver.

## What's here

- `mgctl.c` — Standalone C tool using libusb for keyboard configuration and firmware flashing (builds to `mgctl`)
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
sudo ./mgctl get-version
./mgctl get-battery
./mgctl get-rate
./mgctl set-rate <125|250|500|1000>
./mgctl get-debounce
./mgctl set-debounce <1-10>
./mgctl get-info
./mgctl factory-reset
./mgctl flash <firmware.raw>
```

Download and flash firmware (**untested — use at your own risk**):

```bash
./download_firmware.sh
./mgctl flash mg108b_firmware_v108.raw
```
