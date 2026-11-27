/* BOOT button debounce and events (main/button.h), one sample per 50 ms. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "button.h"

static button_t b;
static unsigned n_press, n_r1, n_cal, n_stuck;

/* Feed a pattern: '_' = pressed (low), '-' = released, each one tick. */
static void feed(const char *p)
{
    for (; *p; ++p) {
        switch (button_step(&b, *p == '_')) {
        case BTN_PRESS: ++n_press; break;
        case BTN_HOLD_R1: ++n_r1; break;
        case BTN_HOLD_CAL: ++n_cal; break;
        case BTN_STUCK: ++n_stuck; break;
        default: break;
        }
    }
}
static void level(char c, int ticks) { char s[2] = {c, 0}; while (ticks--) feed(s); }
static void reset(void) { memset(&b, 0, sizeof(b)); n_press = n_r1 = n_cal = n_stuck = 0; }
static void armed(void) { reset(); level('-', BTN_ARM_TICKS); }

int main(void)
{
    /* held at start: nothing, not even after release; then it works */
    reset();
    level('_', 100);
    level('-', 3);
    assert(n_press + n_r1 + n_cal + n_stuck == 0);
    level('-', BTN_ARM_TICKS);
    feed("___---");
    assert(n_press == 1);

    /* a clean press */
    armed();
    feed("____----");
    assert(n_press == 1 && n_r1 == 0);

    /* one-sample glitches (bounce, noise) never count */
    armed();
    feed("-_-_-_-_-_-_-_-_-_-_-_-_--------");
    assert(n_press == 0);

    /* bounce at press and at release: one press */
    armed();
    feed("_-___________-_-----");
    assert(n_press == 1);

    /* a one-sample dropout while held does not split the press */
    armed();
    feed("______-______-______----");
    assert(n_press == 1);

    /* three presses in a row: three channels */
    armed();
    feed("___---___---___------");
    assert(n_press == 3);

    /* hold 2 s: R1 at 2 s, no press on release */
    armed();
    level('_', BTN_HOLD_R1_TICKS + 5);
    assert(n_r1 == 1 && n_press == 0);
    level('-', 5);
    assert(n_r1 == 1 && n_press == 0 && n_cal == 0);

    /* hold 8 s: R1, then calibration, once each */
    armed();
    level('_', BTN_HOLD_CAL_TICKS + 20);
    level('-', 5);
    assert(n_r1 == 1 && n_cal == 1 && n_press == 0 && n_stuck == 0);

    /* stuck: one report, then ignored until released for the arm time */
    armed();
    level('_', 2000);
    assert(n_r1 == 1 && n_cal == 1 && n_stuck == 1);
    level('-', BTN_ARM_TICKS - 1);
    feed("___---");
    assert(n_press == 0);
    level('-', BTN_ARM_TICKS);
    feed("___---");
    assert(n_press == 1);

    /* cancelled while held (boot window, calibration): nothing on release */
    armed();
    feed("___");
    button_cancel(&b);
    feed("___----");
    assert(n_press == 0);
    feed("___----");
    assert(n_press == 1);

    /* cancelled while a press is still debouncing */
    armed();
    feed("_");
    button_cancel(&b);
    feed("___----");
    assert(n_press == 0);

    /* a cancelled glitch does not eat the next press */
    armed();
    feed("_");
    button_cancel(&b);
    feed("----___----");
    assert(n_press == 1);

    puts("button ok");
    return 0;
}
