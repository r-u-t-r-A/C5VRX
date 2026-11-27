/* c5div-rx: the BOOT button, debounced. One sample per control tick (50 ms).
 * Pure logic, host-tested (test/test_button.c).
 *
 *   debounce  the level counts only after BTN_DEBOUNCE_TICKS equal samples
 *             in a row, on press and on release: contact bounce and short
 *             glitches never start or split a press
 *   arming    a button counts only after it was seen released for
 *             BTN_ARM_TICKS (a button held at start does nothing)
 *   stuck     held for BTN_STUCK_TICKS: ignored until released and armed again
 *
 * Events: a press shorter than BTN_HOLD_R1_TICKS gives BTN_PRESS on release;
 * holding gives BTN_HOLD_R1 at 2 s and BTN_HOLD_CAL at 8 s while still held,
 * and no press on release. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BTN_DEBOUNCE_TICKS  2       /* 100 ms stable */
#define BTN_ARM_TICKS       10      /* 0.5 s released before it counts */
#define BTN_HOLD_R1_TICKS   40      /* 2 s: back to R1 */
#define BTN_HOLD_CAL_TICKS  160     /* 8 s: filter calibration */
#define BTN_STUCK_TICKS     300     /* 15 s: ignored until released */

typedef enum { BTN_NONE, BTN_PRESS, BTN_HOLD_R1, BTN_HOLD_CAL, BTN_STUCK } btn_event_t;

typedef struct {
    bool down;          /* debounced level */
    bool armed;
    bool spent;         /* the current press gives no more events */
    uint8_t flip;       /* samples in a row that disagree with `down` */
    uint8_t up;         /* debounced-released ticks, up to BTN_ARM_TICKS */
    uint16_t held;      /* ticks of the current press, from its first sample */
} button_t;

static inline btn_event_t button_step(button_t *b, bool raw_down)
{
    if (raw_down != b->down) {
        if (++b->flip < BTN_DEBOUNCE_TICKS) return BTN_NONE;
        b->flip = 0;
        b->down = raw_down;
        if (raw_down) {                                     /* pressed */
            b->held = BTN_DEBOUNCE_TICKS;
            b->spent = b->spent || !b->armed;
            b->up = 0;
            return BTN_NONE;
        }
        const bool press = !b->spent && b->held < BTN_HOLD_R1_TICKS;   /* released */
        b->spent = false;
        b->up = 0;
        return press ? BTN_PRESS : BTN_NONE;
    }
    b->flip = 0;
    if (!b->down) {
        b->spent = false;
        if (b->up < BTN_ARM_TICKS && ++b->up == BTN_ARM_TICKS) b->armed = true;
        return BTN_NONE;
    }
    if (b->held < UINT16_MAX) ++b->held;
    if (b->spent) return BTN_NONE;
    if (b->held == BTN_HOLD_R1_TICKS) return BTN_HOLD_R1;
    if (b->held == BTN_HOLD_CAL_TICKS) return BTN_HOLD_CAL;
    if (b->held == BTN_STUCK_TICKS) {
        b->armed = false;
        b->spent = true;
        return BTN_STUCK;
    }
    return BTN_NONE;
}

/* The current press, if any, gives no events (boot window, calibration). */
static inline void button_cancel(button_t *b)
{
    if (b->down || b->flip) b->spent = true;
}
