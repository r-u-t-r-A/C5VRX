/* c5div-rx: the C5VRX-5 onboard flight log (flash recorder, console dump,
 * IQ snapshots) is removed from this build. These no-ops keep the record
 * points of the receiver core (gain, DC servo, calibration, profile) unchanged;
 * the payloads they would have built become dead code the compiler drops.
 * Original: C5VRX-5 main/flightlog.c/.h by Twotoz and the C5VRX contributors. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline void flightlog_start(void) {}
static inline bool flightlog_ready(void) { return false; }
static inline bool flightlog_dumping(void) { return false; }
static inline void flightlog_record(uint8_t type, const void *payload, uint8_t len)
{
    (void)type; (void)payload; (void)len;
}
static inline void flightlog_event(uint8_t kind, const void *data, uint8_t len)
{
    (void)kind; (void)data; (void)len;
}
static inline void flightlog_snapshot_request(uint8_t trigger) { (void)trigger; }
