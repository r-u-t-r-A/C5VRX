/* Firmware identity and a minimal USB status line.
 *
 * firmware_id()/firmware_print() are carried over unchanged from C5VRX-5
 * main/console.c by Twotoz and the C5VRX contributors
 * (https://github.com/Twotoz/C5VRX). c5div-rx modification: the interactive
 * console (menu keys, bench labs, calibration dumps, flight-log commands) is
 * removed; only the download key and a periodic status line remain.
 * Polled, never paces the realtime path. */
#include "receiver.h"
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "hal/usb_serial_jtag_ll.h"
#include "rf.h"

#ifndef C5VRX5_COMMIT
#define C5VRX5_COMMIT "unknown"
#endif

/* "<version>+<commit 12>" */
const char *firmware_id(void)
{
    static char id[64];
    if (!id[0]) snprintf(id, sizeof(id), "%s+%.12s", esp_app_get_description()->version, C5VRX5_COMMIT);
    return id;
}

void firmware_print(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char elf[65] = "";
    (void)esp_app_get_elf_sha256(elf, sizeof(elf));
    printf("FIRMWARE c5div-rx id=%s version=%s commit=%s elf_sha256=%s built=\"%s %s\" idf=%s\n", firmware_id(),
           d->version, C5VRX5_COMMIT, elf[0] ? elf : "unknown", d->date, d->time, d->idf_ver);
}

/* Runs above the receiver tasks so the download key works however busy the
 * CPU is. '`' reboots into USB download mode: esptool's own DTR/RTS reset
 * does not reach the ROM loader on a running image (the modem domain keeps
 * running), so tools/flash.py and tools/enter_download.py send this key
 * first, exactly as with C5VRX-5. Every 2 s one STATUS line. */
static void usb_task(void *arg)
{
    (void)arg;
    int64_t beat = 0;
    for (;;) {
        for (unsigned k = 0; k < 64; ++k) {
            uint8_t c;
            if (usb_serial_jtag_ll_read_rxfifo(&c, 1) == 0) break;
            if (c == '`') rf_reboot_to_download();   /* never returns */
        }
        if (esp_timer_get_time() - beat >= 2000000) {
            beat = esp_timer_get_time();
            control_print_beat();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void console_start(void)
{
    ESP_ERROR_CHECK(xTaskCreate(usb_task, "usb_status", 3072, NULL, 10, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
