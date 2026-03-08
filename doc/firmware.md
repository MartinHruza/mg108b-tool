# MonsGeek MG108B — Firmware Analysis

Analysis of `mg108b_firmware_v108.raw` (282,912 bytes / 0x45120 bytes).

## File Structure

```
Offset      Size      Content
─────────────────────────────────────────────
0x00000     64 KB     Bootloader
0x10000     217 KB    Application firmware
─────────────────────────────────────────────
Total       282,912 bytes
```

The firmware file is a raw flash image. The first 64 KB is the bootloader (not reflashed during normal updates). Only the application portion (offset 0x10000 onward, 217,376 bytes) is sent to the device during a firmware upgrade.

## MCU Identification

**Chip**: YiChip YC31xx series (likely YC3121 or YC3122)
**Architecture**: ARM Cortex-M0 (Thumb-2 instruction set, little-endian)
**Flash base address**: `0x01000000`

Evidence:
- SDK function names: `yc_gpio`, `yc_uart`, `yc_timer`, `yc_systick`, `yc_sysctrl`, `yc_adc`, `yc_wdt`, `yc_ssc`
- String: `"YiChip"` found in firmware
- Vector table entries point to `0x010xxxxx` range
- Flash layout consistent with YiChip documentation

## Memory Map

```
Address         Description
──────────────────────────────────────
0x01000000      Flash base (bootloader)
0x01010000      Application start
0x00020000      RAM base
0x00020004      Reboot flag (bootloader checks this)
```

The bootloader occupies flash from `0x01000000` to `0x0100FFFF`. The application starts at `0x01010000`.

## Bootloader Analysis

### Entry Point

The bootloader has its own vector table at the start of flash. It implements:

- USB HID device enumeration (single interface, vendor-specific)
- Firmware receive and flash write protocol
- Verification with checksum
- Jump to application firmware

### Flash Protocol Handler

The bootloader recognizes these command prefixes:

| Prefix     | Command  | Description                    |
|------------|----------|--------------------------------|
| `0xBA 0xC0` | Header  | Start transfer, receive chunk count and payload size |
| `0xBA 0xC2` | Verify  | End transfer, verify checksum  |

Response prefix is `0xAB`.

### Transfer Flow (Bootloader Perspective)

1. **Receive header** (`0xBA 0xC0`): Extract chunk count (UInt16LE) and data size (3 bytes of UInt32LE). Prepare to receive firmware data.

2. **Receive data chunks**: Each 64-byte Set Feature Report is a raw firmware chunk. The bootloader writes chunks to flash sequentially starting at the application base address (`0x01010000`).

3. **Read-back verification**: The bootloader reads back each written chunk from flash and compares it against the received data to detect write failures.

4. **Receive verify** (`0xBA 0xC2`): Extract the expected checksum (UInt32LE). Compute a running sum over all received payload bytes. Compare:
   - `0x55` = PASS (checksums match)
   - `0xAA` = FAIL (mismatch)

### Reboot Mechanism

The bootloader checks a flag at RAM address `0x00020004`:
- If the flag indicates "boot application", it jumps to the application vector table at `0x01010000`
- After successful flash verification, the bootloader sets this flag and triggers a system reset
- On cold boot, if no valid application is detected, the bootloader stays in DFU mode

### USB Descriptors (Bootloader)

The bootloader implements minimal USB handling:
- `GET_DESCRIPTOR` (device, configuration, string)
- `SET_INTERFACE`
- `SET_IDLE`
- Single HID interface for firmware transfer

### Interrupt Dispatch

The bootloader uses a dual interrupt dispatch mechanism:
- Bootloader has its own interrupt vector table at `0x01000000`
- Application has its vector table at `0x01010000`
- The bootloader's default handler forwards interrupts to the application's vector table when running in application mode

## Application Firmware

### USB Descriptors

Found in the application firmware:

- **VID**: 0x3151
- **PID**: 0x4015 (wireless dongle), 0x4011 (wired USB)
- **Manufacturer**: "SHENZHEN RONGYUAN ELECTRONIC TECHNOLOGY CO."
- **Product**: "MonsGeek MG108B Keyboard"
- **3 HID interfaces**: Keyboard (iface 0), Consumer Control (iface 1), Vendor Config (iface 2)

### Bluetooth

The firmware includes a Broadcom Bluetooth stack:

- **Protocol**: BT 3.0 Classic
- **Device name**: `"MonsGeek MG108B BT3.0"`
- **Profiles**: HID over Bluetooth (keyboard profile)

Bluetooth-related strings found:
- `"MonsGeek MG108B BT3.0"`
- Various Broadcom stack internal strings

### SDK Functions

Identified YiChip SDK functions used by the application:

| Module       | Functions                                    |
|-------------|----------------------------------------------|
| GPIO        | `yc_gpio_init`, `yc_gpio_set`, `yc_gpio_get` |
| UART        | `yc_uart_init`, `yc_uart_send`, `yc_uart_recv` |
| Timer       | `yc_timer_init`, `yc_timer_start`, `yc_timer_stop` |
| SysTick     | `yc_systick_init`, `yc_systick_get`          |
| SysCtrl     | `yc_sysctrl_init`, `yc_sysctrl_set_clk`      |
| ADC         | `yc_adc_init`, `yc_adc_read`                 |
| Watchdog    | `yc_wdt_init`, `yc_wdt_feed`                 |
| SSC (SPI)   | `yc_ssc_init`, `yc_ssc_send`                 |

### Keyboard Features (from firmware strings)

- 108 key layout (full-size with numpad)
- RGB LED control (per-key or zone-based)
- Multiple profiles
- Macro support
- Key remapping
- N-key rollover
- Game mode (Win key disable)
- Debounce configuration (1-10)
- Report rate selection (125/250/500/1000 Hz)
- Sleep/deep sleep timers
- 2.4 GHz wireless (via dongle)
- Bluetooth 3.0
- Wired USB

## Firmware Package (.bin)

The file `mg108b_firmware_v108.bin` (85,947 bytes) is an **encrypted** firmware package distributed by MonsGeek:

- **Shannon entropy**: 7.99 bits/byte (near-perfect randomness, indicating encryption)
- **Format**: Not the raw flash image — this is the encrypted container that the MonsGeek Driver app decrypts before flashing
- **Decryption**: Handled by the Rust backend (`iot_driver.exe`) before the raw firmware is sent to the keyboard

The `.raw` file is the already-decrypted version ready for direct flashing.

## Flash Image Layout (Detailed)

```
Offset    Content
────────────────────────────────────────────────
0x00000   Bootloader vector table (Cortex-M0)
          - 0x00: Initial SP
          - 0x04: Reset handler
          - 0x08-0x3C: Exception handlers
0x00040   Bootloader code
          - USB stack (device enumeration)
          - HID Feature Report handler
          - Flash write routines
          - Checksum verification
          - Application jump logic
0x0FFFF   End of bootloader region
────────────────────────────────────────────────
0x10000   Application vector table
          - 0x00: Initial SP
          - 0x04: Reset handler (app entry point)
          - 0x08-0x3C: Exception handlers
0x10040   Application code
          - USB HID stack (3 interfaces)
          - Bluetooth stack (Broadcom)
          - Keyboard matrix scanning
          - RGB LED controller
          - Config command handler
          - Profile management
          - Macro engine
          - Power management
0x45120   End of firmware image
────────────────────────────────────────────────
```
