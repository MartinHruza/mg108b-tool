# Linux CLI for the MonsGeek MG108B

> Reverse-engineered, open-source replacement for the Windows-only MonsGeek Driver.
> Configure, query, and flash your MG108B keyboard entirely from the command line.

## Features

| Command | Description |
|---|---|
| `get-version` | Firmware version |
| `get-battery` | Battery level & charging status |
| `get-info` | Device info (product ID, connection mode) |
| `get-rate` / `set-rate` | Polling rate (125 / 250 / 500 / 1000 Hz) |
| `get-debounce` / `set-debounce` | Key debounce (1–10) |
| `factory-reset` | Restore factory defaults |
| `flash` | Flash a decrypted firmware image |

## Building

```bash
sudo apt install libusb-1.0-0-dev   # Debian/Ubuntu
make
```

## Usage

Requires root for USB access (or set up udev rules).

```bash
sudo ./mgctl get-version
sudo ./mgctl set-rate 1000
sudo ./mgctl set-debounce 5
sudo ./mgctl flash mg108b_firmware_v108.raw
```

### Firmware download

```bash
./download_firmware.sh              # fetches latest from MonsGeek API
sudo ./mgctl flash mg108b_firmware_v108.raw
```

> **Warning:** Flashing is untested on hardware other than my own. Use at your own risk.

## Documentation

See [`doc/`](doc/) for reverse-engineering notes:

- [USB HID Protocol](doc/driver.md) — commands, payloads, checksum format
- [Firmware Analysis](doc/firmware.md) — YiChip YC31xx, ARM Cortex-M0, memory layout

