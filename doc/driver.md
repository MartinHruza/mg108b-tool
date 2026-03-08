# MonsGeek MG108B — Driver Protocol Documentation

Reverse-engineered from the MonsGeek Driver v1.0.3.6 Electron app
(`extracted/app/resources/app/dist/static/js/main_1a88aec3.js`).

## Architecture

The official MonsGeek Driver is an Electron app with a Rust native backend:

- **Frontend**: Electron (Chromium) — UI in minified JS/CSS
- **Backend**: `iot_driver.exe` — Rust binary providing a gRPC server
- **Communication**: Frontend → gRPC → Rust backend → hidapi → USB HID

The Rust backend uses **hidapi** to communicate with the keyboard via HID Feature Reports.

### JS Class Hierarchy

The device model is implemented through a deep class hierarchy:

```
sSt → AKe → HGe → uB → MC → gC → uC → XM → OM
```

Where `XM` is the MG108B-specific class and `OM` is the base keyboard class.

## USB Device Identification

### Normal Mode

| Connection | VID    | PID    | Description           |
|------------|--------|--------|-----------------------|
| Wired/Dongle | 0x3151 | 0x4015 | Wireless dongle mode |
| Wired USB    | 0x3151 | 0x4011 | Direct USB connection |

### Bootloader Mode

| VID    | PID    | Description                 |
|--------|--------|-----------------------------|
| 0x3151 | 0x4001 | Standard bootloader         |
| 0x0461 | 0x4001 | Alternate bootloader VID    |

### Device Group

The MG108B belongs to the `rongyuan_k_rgb` device group in the driver configuration, with settings:

```js
{ deBounce: 10, sleep_24: { min: 1, max: 60, min_deep: 10, max_deep: 60 } }
```

## USB HID Interfaces

The keyboard exposes 3 HID interfaces:

| Interface | Class | Usage Page | Usage | Description            |
|-----------|-------|------------|-------|------------------------|
| 0         | HID   | 0x01       | 0x06  | Standard keyboard      |
| 1         | HID   | 0x0C       | 0x01  | Consumer control       |
| 2         | HID   | 0xFFFF     | 0x02  | Vendor-specific config |

**All configuration commands use interface 2** (vendor-specific).

The bootloader device exposes only one HID interface (use the first found).

### HID Report Descriptor (Interface 2)

The vendor HID interface has a minimal report descriptor (20 bytes):

```
06 FF FF  Usage Page (0xFFFF - Vendor Defined)
09 02     Usage (0x02)
A1 01     Collection (Application)
15 00       Logical Minimum (0)
26 FF 00    Logical Maximum (255)
75 08       Report Size (8)
96 00 01    Report Count (256)   ← actually 64 bytes used
09 01       Usage (0x01)
91 02       Output (Data, Var, Abs)
C0        End Collection
```

**Important**: No Report ID is defined. This means the wire transfer is exactly 64 bytes (not 65). The hidapi library convention of prepending report ID 0x00 in the API buffer does not translate to a 0x00 byte on the wire.

## HID Transport

All communication uses **HID Feature Reports** via USB control transfers.

### Set Feature Report

```
bmRequestType: 0x21 (Host-to-device, Class, Interface)
bRequest:      0x09 (SET_REPORT)
wValue:        0x0300 | report_id
wIndex:        interface_number
Data:          64 bytes (no report ID byte on wire when report_id=0)
```

### Get Feature Report

```
bmRequestType: 0xA1 (Device-to-host, Class, Interface)
bRequest:      0x01 (GET_REPORT)
wValue:        0x0300 | report_id
wIndex:        interface_number
Data:          64 bytes (no report ID byte on wire when report_id=0)
```

### Wire Format Note

When using libusb directly (not hidapi), you must replicate hidapi's behavior:
- API buffer: 65 bytes — `buf[0]` = report ID (0x00), `buf[1..64]` = data
- Wire transfer: 64 bytes — `buf[1..64]` only (skip `buf[0]` when report ID is 0x00)

## Checksum Algorithms

### BIT7 Checksum

Used by all configuration commands (debounce, report rate, version, etc.).

Computed over bytes 0-6 of the data portion (buf[1..7] in the 65-byte API buffer):

```
buf[8] = 255 - (sum(buf[1..7]) & 0xFF)
```

### NONE (No Checksum)

Used by firmware upgrade data transfers. Raw bytes sent as-is.

## Configuration Commands

### Command ID Convention

- SET commands: `0x00` - `0x7F`
- GET commands: `SET | 0x80` (bit 7 set)
- All use BIT7 checksum unless noted otherwise

### Command Buffer Layout

65-byte API buffer:

```
Byte 0:     Report ID (0x00)
Byte 1:     Command ID
Byte 2-7:   Command parameters (varies by command)
Byte 8:     BIT7 checksum
Byte 9-64:  Zeros (padding)
```

### Full Command Table

| SET ID | GET ID | Name           | Category    | Description                      |
|--------|--------|----------------|-------------|----------------------------------|
| 0x00   | 0x80   | SET_REV/GET_REV | Core       | Firmware version                 |
| 0x02   | —      | SET_RESERT     | Core        | Factory reset                    |
| —      | 0x83   | GET_BATTERY    | Core        | Battery level                    |
| 0x04   | 0x84   | SET_REPORT/GET_REPORT | Core | USB report rate                  |
| —      | 0x8F   | GET_INFOR      | Core        | Device information               |
| 0x05   | 0x85   | SET_PROFILE/GET_PROFILE | Profile | Active profile switch        |
| 0x06   | 0x86   | SET_MK_FN/GET_MK_FN | Keyboard | Fn key mapping                |
| 0x07   | 0x87   | SET_MKEY/GET_MKEY | Keyboard   | Key remapping                  |
| 0x08   | 0x88   | SET_LED/GET_LED | RGB        | LED mode / RGB settings          |
| 0x09   | 0x89   | SET_CUS_LED/GET_CUS_LED | RGB | Custom LED pattern            |
| 0x0A   | 0x8A   | SET_MC/GET_MC  | Macros      | Macro definitions                |
| 0x0B   | 0x8B   | SET_MC_REPORT/GET_MC_REPORT | Macros | Macro report         |
| 0x11   | 0x91   | SET_DEBOUNCE/GET_DEBOUNCE | Options | Key debounce time          |
| 0x12   | 0x92   | SET_SLEEP/GET_SLEEP | Options   | Sleep timeout settings         |
| 0x14   | 0x94   | SET_2_4_PAIR/GET_2_4_PAIR | Wireless | 2.4GHz pairing            |
| 0x15   | 0x95   | SET_GAME_MODE/GET_GAME_MODE | Options | Game mode (disable Win key)|
| 0x18   | 0x98   | SET_INDICATOR/GET_INDICATOR | Options | LED indicators config    |
| 0x19   | 0x99   | SET_PWR_LED/GET_PWR_LED | Options | Power LED settings           |

### Implemented Command Details

#### GET_REV (0x80) — Firmware Version

**Request**: `[0x80, 0, 0, 0, 0, 0, 0]` + checksum

**Response**: Version as UInt16LE at data bytes [1..2] (buf[2..3])
- High byte = major, low byte = minor
- Displayed as `major.minor` (e.g., `1.08`)

#### GET_BATTERY (0x83) — Battery Level

**Request**: `[0x83, 0, 0, 0, 0, 0, 0]` + checksum

**Response**: Battery percentage at data byte [2] (buf[3])

#### SET_RESERT (0x02) — Factory Reset

**Request**: `[0x02, 0, 0, 0, 0, 0, 0]` + checksum

No response read. 500ms delay recommended after sending.

#### GET_INFOR (0x8F) — Device Information

**Request**: `[0x8F, 0, 0, 0, 0, 0, 0]` + checksum

**Response**: 16 bytes of device info starting at data byte [1] (buf[2])

#### SET_REPORT (0x04) / GET_REPORT (0x84) — Report Rate

Rate codes:

| Rate   | Code |
|--------|------|
| 1000 Hz | 1   |
| 500 Hz  | 2   |
| 250 Hz  | 4   |
| 125 Hz  | 8   |

**GET_REPORT request**: `[0x84, profile, 0, 0, 0, 0, 0]` + checksum
- `profile=0` for current profile

**GET_REPORT response**: Rate code at data byte [2] (buf[3])

**SET_REPORT request**: `[0x04, profile, rate_code, 0, 0, 0, 0]` + checksum
- `profile=0` for current, `rate_code` from table above

#### SET_DEBOUNCE (0x11) / GET_DEBOUNCE (0x91) — Debounce

Value range: 1-10 (MG108B specific, no wire/UI offset)

**GET_DEBOUNCE request**: `[0x91, 0, 0, 0, 0, 0, 0]` + checksum

**GET_DEBOUNCE response**: Debounce value at data byte [2] (buf[3])

**SET_DEBOUNCE request**: `[0x11, 0, value, 0, 0, 0, 0]` + checksum

## Firmware Upgrade Protocol

### Overview

1. Enter bootloader mode (via normal-mode device)
2. Wait for bootloader device to enumerate
3. Send transfer header
4. Send firmware data in 64-byte chunks
5. Send checksum and verify

### Firmware File Format

The `.raw` firmware file contains:
- **Bytes 0x00000 - 0x0FFFF** (64 KB): Bootloader — **skipped** during flash
- **Bytes 0x10000 - end**: Application firmware — this is the payload

Only the application portion (bytes after offset 65536) is sent to the device.

### Step 1: Enter Bootloader

Send to the **normal-mode** keyboard (VID=0x3151, PID=0x4015 or 0x4011), interface 2:

```
API buffer (65 bytes):
[0x00,  0x7F, 0x55, 0xAA, 0x55, 0xAA, 0x00, 0x00, 0x82,  0x00 x56]
 ^       ^                                          ^
 |       |                                          BIT7 checksum
 |       Boot command: 7F 55 AA 55 AA 00 00
 Report ID
```

The BIT7 checksum: `255 - ((0x7F + 0x55 + 0xAA + 0x55 + 0xAA) & 0xFF)` = `255 - 125` = `130` = `0x82`

Close the device handle after sending. Wait 1000ms for USB re-enumeration.

### Step 2: Wait for Bootloader

Poll every 500ms (up to 50 seconds) for:
- VID=0x3151, PID=0x4001
- VID=0x0461, PID=0x4001

Use the **first HID interface** found (bootloader has only one).

Wait an additional 1000ms after detection for the device to stabilize.

### Step 3: Transfer Header

Send to the **bootloader** device:

```
[0xBA, 0xC0, chunkCount_lo, chunkCount_hi, size_0, size_1, size_2, 0x00 x57]
```

Where:
- `chunkCount` = `ceil(payloadLen / 64)` as UInt16LE
- `size` = `payloadLen` as first 3 bytes of UInt32LE

After Set Feature, issue a Get Feature (5s timeout) to get acknowledgment.

**Retry up to 10 times** if no response.

### Step 4: Send Firmware Data

For each chunk (0 to chunkCount-1):
- Extract 64 bytes from payload at offset `chunk_index * 64`
- Last chunk is zero-padded if payload doesn't align to 64 bytes
- Send as Set Feature Report (64 bytes data, no checksum)

### Step 5: Verify

Compute checksum: simple sum of ALL payload bytes (uint32).

```
[0xBA, 0xC2, chunkCount_lo, chunkCount_hi, cksum_0, cksum_1, cksum_2, size_0, size_1, size_2, 0x00 x54]
```

Send as Set Feature Report, then Get Feature Report (5s timeout).

The bootloader responds with prefix `0xAB`. Verify result: `0x55` = PASS, `0xAA` = FAIL.

After successful verification, the keyboard automatically reboots into normal mode.

## Linux Setup

### udev Rules

For non-root access, install udev rules:

```bash
sudo cp 99-monsgeek.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

File `99-monsgeek.rules`:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="3151", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="0461", ATTR{idProduct}=="4001", MODE="0666"
```

### Kernel Driver

Linux automatically binds `usbhid`/`hidraw` to HID interfaces. The tool must detach the kernel driver before claiming the interface:

```c
libusb_detach_kernel_driver(handle, interface_number);
libusb_claim_interface(handle, interface_number);
```

### Building the Tool

```bash
gcc -o flash_mg108b flash_mg108b.c $(pkg-config --cflags --libs libusb-1.0)
```

### Usage

```
./flash_mg108b get-version
./flash_mg108b get-battery
./flash_mg108b get-info
./flash_mg108b factory-reset
./flash_mg108b get-rate
./flash_mg108b set-rate <125|250|500|1000>
./flash_mg108b get-debounce
./flash_mg108b set-debounce <1-10>
./flash_mg108b flash <firmware.raw>
```
