/* c5div-rx: TX ownership without the on-screen menu.
 *
 * In C5VRX-5 (by Twotoz and the C5VRX contributors,
 * https://github.com/Twotoz/C5VRX) the menu raster took the TX path for the
 * boot splash, the menu and the calibration status screens, and several
 * receiver steps rely on that: an analog filter change (profile_set) and
 * the boot sampling-phase probe only run while live video does not own TX.
 *
 * This file keeps exactly those ownership rules without drawing anything:
 *   boot window  TX held while the gains settle and the boot phase probe runs
 *                (the splash's timing, main.c), then live video
 *   status hold  TX held while calibration/filter work runs; the status
 *                text goes to the USB log instead of the screen
 * While held, PARLIO TX is stopped (flat DAC level, no sync) and RX keeps
 * capturing IQ, as under the original menu raster. The 150 ms relock gaps
 * are kept from menu.c (a receiver drops its lock and locks fresh). */
#include "receiver.h"
#include <stdio.h>

#define RELOCK_GAP_MS 150u

enum { HOLD_NONE, HOLD_BOOT, HOLD_STATUS };
static volatile int s_hold = HOLD_NONE;

bool tx_hold_active(void) { return s_hold != HOLD_NONE; }
bool boot_window_active(void) { return s_hold == HOLD_BOOT; }
bool status_hold_active(void) { return s_hold == HOLD_STATUS; }

static void hold_begin(int reason)
{
    s_hold = reason;
    transport_stop_tx();
    vTaskDelay(pdMS_TO_TICKS(RELOCK_GAP_MS));
}

static void hold_end(void)
{
    vTaskDelay(pdMS_TO_TICKS(RELOCK_GAP_MS));
    transport_resume_live();
    s_hold = HOLD_NONE;
    ++s_control_epoch;
}

void boot_window_begin(void)
{
    if (s_hold == HOLD_NONE) hold_begin(HOLD_BOOT);
}

void boot_window_end(void)
{
    if (s_hold == HOLD_BOOT) hold_end();
}

void status_hold(const char *title, const char *line1, const char *line2, int percent)
{
    if (percent >= 0) printf("STATUS_HOLD %s | %s | %s | %d%%\n", title, line1, line2, percent);
    else printf("STATUS_HOLD %s | %s | %s\n", title, line1, line2);
    if (s_hold == HOLD_NONE) hold_begin(HOLD_STATUS);
}

void status_hold_end(void)
{
    if (s_hold == HOLD_STATUS) hold_end();
}
