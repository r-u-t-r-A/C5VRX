/* C5VRX-5 by Twotoz and the C5VRX contributors (main/experimental.c).
 * c5div-rx: reduced to the fixed baseline, see experimental.h. */
#include "experimental.h"
#include "rf.h"

uint8_t experimental_get(unsigned item) { (void)item; return 0; }
void experimental_load(void) {}
bool experimental_phy_restore(void) { return true; }
void experimental_phy_prepare_restore(void) {}
unsigned experimental_boot_lane(void) { return RF_LANE_ULTRAFINE; }
bool experimental_transfer_active(void) { return rf_lane() != RF_LANE_ULTRAFINE; }
