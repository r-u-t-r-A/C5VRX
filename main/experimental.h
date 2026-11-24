/* C5VRX-5 by Twotoz and the C5VRX contributors: field A/B controls.
 * IDs are flight-log wire identities; append, never reorder. Index zero in
 * each value list is the baseline.
 *
 * c5div-rx: the EXPERIMENTAL menu is removed. Every item is fixed on its
 * C5VRX-5 baseline (value 0: IQ CORR ON, PHASE REPAIR ON, FAST GAIN ON,
 * STRONG LANE ON, LEVEL TOP 9, LOW DC ON, AUTO OUTPUT OFF, IQ LANE ULTRA,
 * stock PHY), stored choices are ignored and no PHY patch is ever applied.
 * The receiver core keeps reading the items through experimental_get().
 * DAC bit 2 is fixed on D3/GPIO7 (C5VRX-5's Clock Pin D10 wiring, without
 * the clock export): see transport.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum {
    EXP_GAIN, EXP_DFILT, EXP_AFC, EXP_DEMOD, EXP_IQ_CORR, EXP_PHASE_REPAIR,
    EXP_AUTO_PROFILE, EXP_FAST_GAIN, EXP_FOLD_RADIUS, EXP_FOLD_MEMORY,
    EXP_LEVEL_TARGET, EXP_LEVEL_RELEASE, EXP_GAIN_RATE, EXP_GAIN_CAP,
    EXP_LOG_CADENCE, EXP_LNA_BIAS, EXP_CURRENT, EXP_LOOP_CAP, EXP_SPUR0, EXP_IQ_LANE, EXP_CLOCK_D10,
    EXP_OUTPUT_AUTO, EXP_STRONG_LANE, EXP_LEVEL_TOP, EXP_LOW_DC, EXP_ITEMS
};
uint8_t experimental_get(unsigned item);        /* always the baseline, 0 */
void experimental_load(void);                   /* no-op: nothing is stored */
bool experimental_phy_restore(void);            /* no-op: stock PHY */
void experimental_phy_prepare_restore(void);    /* no-op: no patch to undo */
unsigned experimental_boot_lane(void);          /* ultrafine */
/* Noise/calibration records predate experimental transfer controls. Never
 * treat a baseline measurement as exact under a different transfer: with all
 * items on baseline only a STRONG LANE step changes the transfer. */
bool experimental_transfer_active(void);
