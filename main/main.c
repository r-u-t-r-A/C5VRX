/* C5VRX-5 by Twotoz and the C5VRX contributors: ESP32-C5 analog 5.8 GHz FPV
 * receiver. https://github.com/Twotoz/C5VRX - https://twotoz.github.io/C5VRX/
 *
 * c5div-rx modifications: no boot logo/menu (a TX-held boot window with the
 * splash's timing instead, txhold.c), no flight log, no restore of menu
 * preferences or a saved decoder choice: every boot uses the C5VRX-5
 * defaults (D32-R2, LEVEL gain, digital filter mode 12, FILTER AUTO). */
#include "receiver.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "rf.h"
#include "phy.h"

volatile const char *s_boot_stage = "start";
static int64_t s_boot_radio_ms = -1, s_boot_ready_ms = -1;
void firmware_boot_timing(int64_t *radio_ms, int64_t *ready_ms)
{
    *radio_ms = s_boot_radio_ms; *ready_ms = s_boot_ready_ms;
}

/* The boot window (was the boot logo) while the receiver tasks start and
 * Direct Gain makes its first decision (a hold, also without a carrier):
 * at least BOOT_MIN_MS, up to BOOT_MAX_MS only while the gain has not held
 * yet, so live video starts on a found gain. TX is held meanwhile and the
 * boot sampling-phase probe may run (calibration.c). */
#define BOOT_MIN_MS 3000
#define BOOT_MAX_MS 4000

static void boot_wait_gain(int64_t t0, uint32_t holds, int64_t deadline)
{
    int settled_ms = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        const int ms = (int)((esp_timer_get_time() - t0) / 1000);
        if (settled_ms < 0 && s_gain.state == DG_HOLD && s_gain.holds != holds) settled_ms = ms;
        if ((settled_ms >= 0 && ms >= BOOT_MIN_MS) || ms >= BOOT_MAX_MS || esp_timer_get_time() >= deadline) break;
    }
    if (settled_ms >= 0) printf("BOOT_WINDOW gain G%u held after %d ms\n", s_current_gain, settled_ms);
    else printf("BOOT_WINDOW ended at %d ms before a gain hold\n", BOOT_MAX_MS);
}

void app_main(void)
{
    const int64_t app_t0 = esp_timer_get_time();
    /* USB output never blocks a task when no host is attached. */
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    fcntl(fileno(stdout), F_SETFL, fcntl(fileno(stdout), F_GETFL, 0) | O_NONBLOCK);
    usb_serial_jtag_vfs_use_nonblocking();
    /* The console first: flashing (download key) and diagnostics must stay
     * reachable whatever a later boot step does. */
    console_start();
    firmware_print();   /* first log line names the exact build */
    if(!control_prepare()) {
        s_boot_stage="control_memory";
        printf("CONTROL stack reservation failed; USB status/download remain available\n");
        return;
    }
    if (!receiver_prepare()) {
        s_boot_stage = "receiver_memory";
        printf("RECEIVER memory reservation failed; USB status/download remain available\n");
        return;
    }
    s_boot_stage = "rf";
    ESP_ERROR_CHECK(rf_start());
    s_boot_radio_ms = (esp_timer_get_time() - app_t0) / 1000;
    printf("BOOT_TIMING radio_ms=%lld\n", s_boot_radio_ms);
    s_boot_stage = "settings";
    settings_load();
    /* Start at the first index of the highest RF stage; Direct Gain takes
     * over at once (table maximum without a carrier). */
    receiver_set_gain(arc_gain_highest_rf_stage_start(rf_gain_table()));
    s_boot_stage = "calibration";
    calibration_start();
    /* The receive profile (filter class + tracker) of this channel is
     * chosen before live TX starts; it never changes under live video. */
    profile_load();
    profile_boot();
    s_boot_stage = "transport";
    if (transport_start() != ESP_OK) {
        s_boot_stage = "transport_fault";
        printf("TRANSPORT boot failed: live OFF; USB status/download remain available\n");
        return;
    }
    s_boot_stage = "boot_window";
    const int64_t window_t0 = esp_timer_get_time();
    const uint32_t window_holds = s_gain.holds;
    /* Leave one second of a ten-second target for final TX handover. */
    const int64_t ready_deadline = app_t0 + 9000000;
    const bool window = esp_timer_get_time() < ready_deadline;
    if (window) boot_window_begin();
    s_boot_stage = "tasks";
    if (!receiver_start()) {
        boot_window_end();
        transport_stop_tx();
        s_boot_stage = "receiver_fault";
        printf("RECEIVER start failed: live OFF; USB status/download remain available\n");
        return;
    }
    control_start();
    if (window) boot_wait_gain(window_t0, window_holds, ready_deadline);
    boot_window_end();
    s_boot_stage = "ready";
    s_boot_ready_ms = (esp_timer_get_time() - app_t0) / 1000;
    printf("BOOT_TIMING ready_ms=%lld target_ms=10000\n", s_boot_ready_ms);
    char gain_name[16];
    printf("c5div-rx %s ready: %s %u MHz, %s, ultrafine IQ, gain %s fw=%s\n",
           esp_app_get_description()->version, rf_channel()->name, rf_frequency_mhz(),
           transport_program_name(), receiver_gain_mode_name(receiver_gain_mode(), gain_name, sizeof(gain_name)),
           firmware_id());
}
