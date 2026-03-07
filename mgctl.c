#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

#define VID_NORMAL      0x3151
#define PID_NORMAL_WIRE 0x4015
#define PID_NORMAL_WIFI 0x4011
#define VID_BOOT_ALT    0x0461
#define PID_BOOT        0x4001

#define FW_SKIP         65536
#define CHUNK_SIZE      64
#define REPORT_BUF_SIZE 65  /* 1 byte report ID + 64 bytes data */

#define BOOT_POLL_MS    500
#define BOOT_TIMEOUT_MS 50000
#define HEADER_RETRIES  10

/* Config command IDs (GET = SET | 0x80) */
#define CMD_SET_REV       0x00
#define CMD_GET_REV       0x80
#define CMD_SET_RESERT    0x02
#define CMD_GET_BATTERY   0x83
#define CMD_SET_REPORT    0x04
#define CMD_GET_REPORT    0x84
#define CMD_SET_DEBOUNCE  0x11
#define CMD_GET_DEBOUNCE  0x91
#define CMD_GET_INFOR     0x8F

#define DEBOUNCE_MIN     1
#define DEBOUNCE_MAX     10

/* Set/Get Feature Report matching hidapi behavior:
   buf[0] is the report ID (0x00 for this device). When report ID is 0,
   hidapi strips it before the control transfer — the wire data starts
   at buf[1]. We replicate that here. */

static int hid_set_feature(libusb_device_handle *dev, int iface,
                           const uint8_t *data, int len)
{
    uint8_t report_id = data[0];
    const uint8_t *payload = data + 1;
    int payload_len = len - 1;

    if (report_id == 0x00) {
        /* No report ID on wire — skip the leading 0x00 byte */
    } else {
        /* Device uses report IDs — include it in the data */
        payload = data;
        payload_len = len;
    }

    int rc = libusb_control_transfer(dev, 0x21, 0x09,
                                     0x0300 | report_id, iface,
                                     (unsigned char *)payload,
                                     payload_len, 5000);
    if (rc < 0) {
        fprintf(stderr, "Set Feature Report failed: %s\n",
                libusb_strerror(rc));
    }
    return rc;
}

static int hid_get_feature(libusb_device_handle *dev, int iface,
                           uint8_t *buf, int len)
{
    uint8_t report_id = buf[0];
    uint8_t *payload = buf + 1;
    int payload_len = len - 1;

    if (report_id == 0x00) {
        /* No report ID on wire — read into buf+1, keep buf[0] as 0x00 */
    } else {
        payload = buf;
        payload_len = len;
    }

    int rc = libusb_control_transfer(dev, 0xA1, 0x01,
                                     0x0300 | report_id, iface,
                                     payload, payload_len, 5000);
    if (rc < 0) {
        fprintf(stderr, "Get Feature Report failed: %s\n",
                libusb_strerror(rc));
    }
    return rc;
}

/* Find and open a USB device by VID/PID. Detach kernel driver and claim
   an HID interface. If target_iface >= 0, use that specific interface number;
   otherwise use the first HID interface found. Returns handle and sets *out_iface. */
static libusb_device_handle *find_device(libusb_context *ctx,
                                         uint16_t vid, uint16_t pid,
                                         int target_iface, int *out_iface)
{
    libusb_device_handle *handle = NULL;
    libusb_device **devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) return NULL;

    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) < 0)
            continue;
        if (desc.idVendor != vid || desc.idProduct != pid)
            continue;

        int err = libusb_open(devs[i], &handle);
        if (err < 0) {
            fprintf(stderr, "Found %04x:%04x but libusb_open failed: %s\n",
                    vid, pid, libusb_strerror(err));
            handle = NULL;
            continue;
        }

        /* Find HID interface */
        struct libusb_config_descriptor *cfg;
        if (libusb_get_active_config_descriptor(devs[i], &cfg) < 0) {
            libusb_close(handle);
            handle = NULL;
            continue;
        }

        int found_iface = -1;
        for (int j = 0; j < cfg->bNumInterfaces; j++) {
            const struct libusb_interface *intf = &cfg->interface[j];
            for (int k = 0; k < intf->num_altsetting; k++) {
                const struct libusb_interface_descriptor *alt = &intf->altsetting[k];
                if (alt->bInterfaceClass != 3) /* HID */
                    continue;
                if (target_iface >= 0 && alt->bInterfaceNumber != target_iface)
                    continue;
                found_iface = alt->bInterfaceNumber;
                break;
            }
            if (found_iface >= 0) break;
        }
        libusb_free_config_descriptor(cfg);

        if (found_iface < 0) {
            fprintf(stderr, "Found %04x:%04x but no matching HID interface"
                    " (target=%d)\n", vid, pid, target_iface);
            libusb_close(handle);
            handle = NULL;
            continue;
        }

        /* Detach kernel driver if attached */
        int kd = libusb_kernel_driver_active(handle, found_iface);
        if (kd == 1) {
            int dr = libusb_detach_kernel_driver(handle, found_iface);
            if (dr < 0) {
                fprintf(stderr, "Could not detach kernel driver from "
                        "interface %d: %s\n", found_iface,
                        libusb_strerror(dr));
            }
        }

        int cl = libusb_claim_interface(handle, found_iface);
        if (cl < 0) {
            fprintf(stderr, "Could not claim interface %d: %s\n",
                    found_iface, libusb_strerror(cl));
            libusb_close(handle);
            handle = NULL;
            continue;
        }

        *out_iface = found_iface;
        libusb_free_device_list(devs, 1);
        return handle;
    }

    libusb_free_device_list(devs, 1);
    return NULL;
}

#define IFACE_CONFIG 2  /* vendor-specific HID interface for config commands */
#define IFACE_ANY   -1  /* first HID interface (for bootloader) */

/* BIT7 checksum: buf index 8 = 255 - (sum of buf[1..7] & 0xFF)
   (indices relative to the 65-byte report buffer where buf[0] is report ID) */
static void set_bit7_checksum(uint8_t *buf)
{
    uint8_t sum = 0;
    for (int i = 1; i <= 7; i++)
        sum += buf[i];
    buf[8] = 255 - sum;
}

/* Open the keyboard in normal mode (wired or wireless).
   Caller must release interface and close handle when done. */
static libusb_device_handle *open_normal_kbd(libusb_context *ctx, int *out_iface)
{
    libusb_device_handle *dev = find_device(ctx, VID_NORMAL, PID_NORMAL_WIRE,
                                            IFACE_CONFIG, out_iface);
    if (!dev)
        dev = find_device(ctx, VID_NORMAL, PID_NORMAL_WIFI,
                          IFACE_CONFIG, out_iface);
    if (!dev)
        fprintf(stderr, "Normal-mode keyboard not found\n");
    return dev;
}

/* Send a config command and optionally read a response.
   cmd is the 65-byte buffer (buf[0]=0x00 report ID, buf[1]=command, ...).
   BIT7 checksum is computed automatically.
   If resp is non-NULL, a Get Feature Report is read into it after sending.
   Returns 0 on success, -1 on failure. */
static int kbd_cmd(libusb_context *ctx, uint8_t *cmd, uint8_t *resp)
{
    int iface = -1;
    libusb_device_handle *dev = open_normal_kbd(ctx, &iface);
    if (!dev) return -1;

    set_bit7_checksum(cmd);
    int rc = hid_set_feature(dev, iface, cmd, REPORT_BUF_SIZE);
    if (rc < 0) goto fail;

    if (resp) {
        memset(resp, 0, REPORT_BUF_SIZE);
        resp[0] = 0x00;
        rc = hid_get_feature(dev, iface, resp, REPORT_BUF_SIZE);
        if (rc < 0) goto fail;
    }

    libusb_release_interface(dev, iface);
    libusb_close(dev);
    return 0;

fail:
    libusb_release_interface(dev, iface);
    libusb_close(dev);
    return -1;
}

/* Helper: prepare a zeroed 65-byte command buffer with report ID and command byte */
static void cmd_init(uint8_t *buf, uint8_t cmd)
{
    memset(buf, 0, REPORT_BUF_SIZE);
    buf[0] = 0x00; /* report ID */
    buf[1] = cmd;
}

/* --- Core commands --- */

static int cmd_get_version(libusb_context *ctx)
{
    uint8_t cmd[REPORT_BUF_SIZE], resp[REPORT_BUF_SIZE];
    cmd_init(cmd, CMD_GET_REV);
    if (kbd_cmd(ctx, cmd, resp) < 0) return -1;

    /* UInt16LE at data byte[1..2] → buf[2..3] */
    int ver = resp[2] | (resp[3] << 8);
    printf("Firmware version: %d.%02d\n", ver >> 8, ver & 0xFF);
    return 0;
}

static int cmd_get_battery(libusb_context *ctx)
{
    uint8_t cmd[REPORT_BUF_SIZE], resp[REPORT_BUF_SIZE];
    cmd_init(cmd, CMD_GET_BATTERY);
    if (kbd_cmd(ctx, cmd, resp) < 0) return -1;

    printf("Battery level: %d%%\n", resp[2]);
    return 0;
}

static int cmd_factory_reset(libusb_context *ctx)
{
    uint8_t cmd[REPORT_BUF_SIZE];
    cmd_init(cmd, CMD_SET_RESERT);
    printf("Performing factory reset...\n");
    if (kbd_cmd(ctx, cmd, NULL) < 0) return -1;
    usleep(500 * 1000);
    printf("Factory reset sent.\n");
    return 0;
}

static int cmd_get_info(libusb_context *ctx)
{
    uint8_t cmd[REPORT_BUF_SIZE], resp[REPORT_BUF_SIZE];
    cmd_init(cmd, CMD_GET_INFOR);
    if (kbd_cmd(ctx, cmd, resp) < 0) return -1;

    /* resp[1..2] = product ID (high, low), resp[3..4] = reserved,
       resp[5] = connection mode (0=wired, 1=wireless), resp[6] = reserved */
    int product_id = (resp[2] << 8) | resp[3];
    int conn_mode = resp[6];

    printf("Product ID:  0x%04X (%d)\n", product_id, product_id);
    printf("Connection:  %s\n", conn_mode == 0 ? "USB wired" : "wireless");
    printf("Raw:");
    for (int i = 1; i <= 8; i++)
        printf(" %02x", resp[i]);
    printf("\n");
    return 0;
}

/* --- Report rate commands --- */

static const struct { int hz; uint8_t code; } rate_table[] = {
    { 1000, 1 }, { 500, 2 }, { 250, 4 }, { 125, 8 }
};
#define RATE_TABLE_SIZE (int)(sizeof(rate_table) / sizeof(rate_table[0]))

static int rate_code_to_hz(uint8_t code)
{
    for (int i = 0; i < RATE_TABLE_SIZE; i++)
        if (rate_table[i].code == code) return rate_table[i].hz;
    return -1;
}

static int rate_hz_to_code(int hz)
{
    for (int i = 0; i < RATE_TABLE_SIZE; i++)
        if (rate_table[i].hz == hz) return rate_table[i].code;
    return -1;
}

static int cmd_get_rate(libusb_context *ctx)
{
    uint8_t cmd[REPORT_BUF_SIZE], resp[REPORT_BUF_SIZE];
    cmd_init(cmd, CMD_GET_REPORT);
    /* byte[1] = profile; 0 = current */
    if (kbd_cmd(ctx, cmd, resp) < 0) return -1;

    int hz = rate_code_to_hz(resp[3]); /* data byte[2] → buf[3] */
    if (hz > 0)
        printf("Report rate: %d Hz\n", hz);
    else
        printf("Report rate: unknown (code=%d)\n", resp[3]);
    return 0;
}

static int cmd_set_rate(libusb_context *ctx, int hz)
{
    int code = rate_hz_to_code(hz);
    if (code < 0) {
        fprintf(stderr, "Invalid rate. Valid values: 125, 250, 500, 1000\n");
        return -1;
    }

    uint8_t cmd[REPORT_BUF_SIZE];
    cmd_init(cmd, CMD_SET_REPORT);
    /* byte[1] = profile (0 = current), byte[2] = rate code */
    cmd[3] = code; /* data byte[2] → buf[3] */

    printf("Setting report rate to %d Hz...\n", hz);
    if (kbd_cmd(ctx, cmd, NULL) < 0) return -1;
    usleep(500 * 1000);
    printf("Report rate set to %d Hz\n", hz);
    return 0;
}

/* --- Debounce commands --- */

static int cmd_get_debounce(libusb_context *ctx)
{
    uint8_t cmd[REPORT_BUF_SIZE], resp[REPORT_BUF_SIZE];
    cmd_init(cmd, CMD_GET_DEBOUNCE);
    if (kbd_cmd(ctx, cmd, resp) < 0) return -1;

    printf("Current debounce: %d\n", resp[3]); /* data byte[2] → buf[3] */
    return 0;
}

static int cmd_set_debounce(libusb_context *ctx, int value)
{
    if (value < DEBOUNCE_MIN || value > DEBOUNCE_MAX) {
        fprintf(stderr, "Debounce value must be %d-%d\n",
                DEBOUNCE_MIN, DEBOUNCE_MAX);
        return -1;
    }

    uint8_t cmd[REPORT_BUF_SIZE];
    cmd_init(cmd, CMD_SET_DEBOUNCE);
    cmd[3] = value; /* data byte[2] → buf[3] */

    printf("Setting debounce to %d...\n", value);
    if (kbd_cmd(ctx, cmd, NULL) < 0) return -1;
    usleep(500 * 1000);
    printf("Debounce set to %d\n", value);
    return 0;
}

static int enter_bootloader(libusb_context *ctx)
{
    int iface = -1;
    libusb_device_handle *dev = open_normal_kbd(ctx, &iface);
    if (!dev) return -1;

    printf("Found keyboard in normal mode, sending boot command...\n");

    uint8_t buf[REPORT_BUF_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x00; /* report ID */
    buf[1] = 0x7F;
    buf[2] = 0x55;
    buf[3] = 0xAA;
    buf[4] = 0x55;
    buf[5] = 0xAA;
    buf[6] = 0x00;
    buf[7] = 0x00;
    buf[8] = 0x82; /* BIT7 checksum: 255 - ((0x7F+0x55+0xAA+0x55+0xAA) & 0xFF) */

    int rc = hid_set_feature(dev, iface, buf, REPORT_BUF_SIZE);

    libusb_release_interface(dev, iface);
    libusb_close(dev);

    if (rc < 0) {
        fprintf(stderr, "Failed to send boot command\n");
        return -1;
    }

    printf("Boot command sent. Waiting for device to re-enumerate...\n");
    usleep(1000 * 1000); /* 1000ms */
    return 0;
}

static libusb_device_handle *wait_for_bootloader(libusb_context *ctx,
                                                  int *out_iface)
{
    int elapsed = 0;
    libusb_device_handle *dev = NULL;

    printf("Polling for bootloader device...\n");
    while (elapsed < BOOT_TIMEOUT_MS) {
        dev = find_device(ctx, VID_NORMAL, PID_BOOT, IFACE_ANY, out_iface);
        if (dev) break;
        dev = find_device(ctx, VID_BOOT_ALT, PID_BOOT, IFACE_ANY, out_iface);
        if (dev) break;

        usleep(BOOT_POLL_MS * 1000);
        elapsed += BOOT_POLL_MS;
        printf("  ... waiting (%d ms)\n", elapsed);
    }

    if (!dev) {
        fprintf(stderr, "Bootloader device not found after %d ms\n",
                BOOT_TIMEOUT_MS);
        return NULL;
    }

    printf("Bootloader device found! Waiting 1s to stabilize...\n");
    usleep(1000 * 1000);
    return dev;
}

static int flash_firmware(libusb_device_handle *dev, int iface,
                          const uint8_t *payload, size_t payload_len)
{
    int chunk_count = (payload_len + CHUNK_SIZE - 1) / CHUNK_SIZE;
    uint8_t buf[REPORT_BUF_SIZE];
    int rc;

    /* --- Step 3: Transfer header --- */
    printf("Sending transfer header (chunks=%d, size=%zu)...\n",
           chunk_count, payload_len);

    int header_ok = 0;
    for (int attempt = 0; attempt < HEADER_RETRIES; attempt++) {
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x00; /* report ID */
        buf[1] = 0xBA;
        buf[2] = 0xC0;
        buf[3] = chunk_count & 0xFF;         /* chunkCount low byte */
        buf[4] = (chunk_count >> 8) & 0xFF;  /* chunkCount high byte */
        buf[5] = payload_len & 0xFF;         /* dataSize byte 0 */
        buf[6] = (payload_len >> 8) & 0xFF;  /* dataSize byte 1 */
        buf[7] = (payload_len >> 16) & 0xFF; /* dataSize byte 2 */

        rc = hid_set_feature(dev, iface, buf, REPORT_BUF_SIZE);
        if (rc < 0) {
            fprintf(stderr, "Header Set failed (attempt %d/%d)\n",
                    attempt + 1, HEADER_RETRIES);
            usleep(500 * 1000);
            continue;
        }

        memset(buf, 0, sizeof(buf));
        buf[0] = 0x00; /* report ID */
        rc = hid_get_feature(dev, iface, buf, REPORT_BUF_SIZE);
        if (rc >= 0) {
            header_ok = 1;
            printf("Header acknowledged.\n");
            break;
        }
        fprintf(stderr, "Header Get failed (attempt %d/%d)\n",
                attempt + 1, HEADER_RETRIES);
        usleep(500 * 1000);
    }

    if (!header_ok) {
        fprintf(stderr, "Failed to get header acknowledgment after %d retries\n",
                HEADER_RETRIES);
        return -1;
    }

    /* --- Step 4: Send firmware chunks --- */
    printf("Flashing %d chunks...\n", chunk_count);
    for (int i = 0; i < chunk_count; i++) {
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x00; /* report ID */

        size_t offset = (size_t)i * CHUNK_SIZE;
        size_t remaining = payload_len - offset;
        size_t copy_len = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
        memcpy(buf + 1, payload + offset, copy_len);

        rc = hid_set_feature(dev, iface, buf, REPORT_BUF_SIZE);
        if (rc < 0) {
            fprintf(stderr, "\nChunk %d/%d failed\n", i + 1, chunk_count);
            return -1;
        }

        /* Progress every 100 chunks or on last chunk */
        if (i % 100 == 0 || i == chunk_count - 1) {
            int pct = (int)((i + 1) * 100 / chunk_count);
            printf("\r  Progress: %d/%d chunks (%d%%)", i + 1, chunk_count, pct);
            fflush(stdout);
        }
    }
    printf("\n");

    /* --- Step 5: Checksum & verify --- */
    uint32_t checksum = 0;
    for (size_t i = 0; i < payload_len; i++)
        checksum += payload[i];

    printf("Sending verify (checksum=0x%08X)...\n", checksum);

    memset(buf, 0, sizeof(buf));
    buf[0]  = 0x00; /* report ID */
    buf[1]  = 0xBA;
    buf[2]  = 0xC2;
    buf[3]  = chunk_count & 0xFF;
    buf[4]  = (chunk_count >> 8) & 0xFF;
    buf[5]  = checksum & 0xFF;
    buf[6]  = (checksum >> 8) & 0xFF;
    buf[7]  = (checksum >> 16) & 0xFF;
    buf[8]  = payload_len & 0xFF;
    buf[9]  = (payload_len >> 8) & 0xFF;
    buf[10] = (payload_len >> 16) & 0xFF;

    rc = hid_set_feature(dev, iface, buf, REPORT_BUF_SIZE);
    if (rc < 0) {
        fprintf(stderr, "Verify Set failed\n");
        return -1;
    }

    memset(buf, 0, sizeof(buf));
    buf[0] = 0x00;
    rc = hid_get_feature(dev, iface, buf, REPORT_BUF_SIZE);
    if (rc < 0) {
        fprintf(stderr, "Verify Get failed\n");
        return -1;
    }

    printf("Verify acknowledged. Flash complete!\n");
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s get-version                Get firmware version\n"
        "  %s get-battery                Get battery level\n"
        "  %s get-info                   Get device info (raw)\n"
        "  %s factory-reset              Factory reset keyboard\n"
        "  %s get-rate                   Get USB report rate\n"
        "  %s set-rate <125|250|500|1000> Set USB report rate (Hz)\n"
        "  %s get-debounce               Get debounce value\n"
        "  %s set-debounce <1-10>        Set debounce value\n"
        "  %s flash <firmware.raw>       Flash firmware\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* Init libusb, run a command callback, cleanup. */
static int run_cmd(int (*fn)(libusb_context *), int argc, int need, const char *msg)
{
    if (argc < need) {
        fprintf(stderr, "%s\n", msg);
        return 1;
    }
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) {
        fprintf(stderr, "libusb_init failed\n");
        return 1;
    }
    int rc = fn(ctx);
    libusb_exit(ctx);
    return rc < 0 ? 1 : 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *subcmd = argv[1];

    /* Simple commands (no extra args) */
    if (strcmp(subcmd, "get-version") == 0)
        return run_cmd(cmd_get_version, argc, 2, "");
    if (strcmp(subcmd, "get-battery") == 0)
        return run_cmd(cmd_get_battery, argc, 2, "");
    if (strcmp(subcmd, "get-info") == 0)
        return run_cmd(cmd_get_info, argc, 2, "");
    if (strcmp(subcmd, "factory-reset") == 0)
        return run_cmd(cmd_factory_reset, argc, 2, "");
    if (strcmp(subcmd, "get-rate") == 0)
        return run_cmd(cmd_get_rate, argc, 2, "");
    if (strcmp(subcmd, "get-debounce") == 0)
        return run_cmd(cmd_get_debounce, argc, 2, "");

    /* Commands with arguments */
    if (strcmp(subcmd, "set-rate") == 0) {
        if (argc < 3) {
            fprintf(stderr, "set-rate requires a value (125, 250, 500, 1000)\n");
            return 1;
        }
        int hz = atoi(argv[2]);
        libusb_context *ctx = NULL;
        if (libusb_init(&ctx) < 0) {
            fprintf(stderr, "libusb_init failed\n");
            return 1;
        }
        int rc = cmd_set_rate(ctx, hz);
        libusb_exit(ctx);
        return rc < 0 ? 1 : 0;
    }

    if (strcmp(subcmd, "set-debounce") == 0) {
        if (argc < 3) {
            fprintf(stderr, "set-debounce requires a value (%d-%d)\n",
                    DEBOUNCE_MIN, DEBOUNCE_MAX);
            return 1;
        }
        int val = atoi(argv[2]);
        libusb_context *ctx = NULL;
        if (libusb_init(&ctx) < 0) {
            fprintf(stderr, "libusb_init failed\n");
            return 1;
        }
        int rc = cmd_set_debounce(ctx, val);
        libusb_exit(ctx);
        return rc < 0 ? 1 : 0;
    }

    /* Flash command */
    if (strcmp(subcmd, "flash") != 0) {
        usage(argv[0]);
        return 1;
    }
    if (argc < 3) {
        fprintf(stderr, "flash requires a firmware file\n");
        return 1;
    }

    /* Read firmware file */
    FILE *fp = fopen(argv[2], "rb");
    if (!fp) {
        perror("Cannot open firmware file");
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= FW_SKIP) {
        fprintf(stderr, "Firmware file too small (%ld bytes, need >%d)\n",
                file_size, FW_SKIP);
        fclose(fp);
        return 1;
    }

    uint8_t *file_buf = malloc(file_size);
    if (!file_buf) {
        perror("malloc");
        fclose(fp);
        return 1;
    }
    if (fread(file_buf, 1, file_size, fp) != (size_t)file_size) {
        perror("fread");
        free(file_buf);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    const uint8_t *payload = file_buf + FW_SKIP;
    size_t payload_len = file_size - FW_SKIP;

    printf("Firmware: %s (%ld bytes)\n", argv[2], file_size);
    printf("Payload:  %zu bytes (skipping first %d)\n", payload_len, FW_SKIP);

    /* Init libusb */
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) {
        fprintf(stderr, "libusb_init failed\n");
        free(file_buf);
        return 1;
    }

    /* Step 1: Enter bootloader */
    if (enter_bootloader(ctx) < 0) {
        libusb_exit(ctx);
        free(file_buf);
        return 1;
    }

    /* Step 2: Wait for bootloader device */
    int boot_iface = -1;
    libusb_device_handle *boot_dev = wait_for_bootloader(ctx, &boot_iface);
    if (!boot_dev) {
        libusb_exit(ctx);
        free(file_buf);
        return 1;
    }

    /* Steps 3-5: Flash */
    int result = flash_firmware(boot_dev, boot_iface, payload, payload_len);

    libusb_release_interface(boot_dev, boot_iface);
    libusb_close(boot_dev);
    libusb_exit(ctx);
    free(file_buf);

    if (result == 0)
        printf("Keyboard will reboot into normal mode.\n");
    else
        fprintf(stderr, "Flashing failed!\n");

    return result != 0;
}
