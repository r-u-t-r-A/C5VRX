/* C5VRX-5 by Twotoz and the C5VRX contributors: 50 ms control loop.
 * BOOT button and menu, AFC acquisition, PAL/NTSC detection, signal
 * strength and the channel scanner. Observes completed RX descriptors only.
 *
 * c5div-rx modifications: no menu, no channel scanner, no console commands,
 * no AUTO OUTPUT/decoder switching/flight log; the BOOT button cycles the
 * eight raceband channels (see control_task). AFC, standard detection,
 * signal strength, video quality and the live profile switch are unchanged. */
#include "receiver.h"
#define PICTURE_YIELD() taskYIELD()   /* the gain observer shares priority 3 */
#include "picture_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "afc_ctrl.h"
#include "output_level.h"
#include "profile_logic.h"
#include "cn_estimate.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "phase8_gain_lut.h"
#include "phy.h"
#include "snapshot.h"
#include "rf.h"

#define BOOT_BUTTON      GPIO_NUM_28    /* XIAO ESP32-C5 BOOT */
/* c5div-rx: external button to GND on XIAO D10 (free: no clock export). One
 * button wired to D10 of both receivers of a diversity pair drives both. */
#define EXT_BUTTON       GPIO_NUM_10
#define TICK_MS          50
#define HOLD_R1_TICKS    40             /* 2 s: back to R1 */
#define HOLD_CAL_TICKS   160            /* 8 s: one-time filter calibration */
#define ARM_TICKS        10             /* an input counts only after 0.5 s released */
#define STUCK_TICKS      300            /* 15 s held: ignore that input until released */

/* An input becomes a button only after it has been seen released (high)
 * for ARM_TICKS, and a hold of STUCK_TICKS disarms it until it is released
 * again. A D10 line that is shorted or pulled to ground can then never
 * start calibration after calibration (seen on the first board: an input
 * read low from boot, every ~13 s a "long hold" took the video for ~10 s). */
static unsigned s_btn_high[2];
static bool s_btn_armed[2];
static bool button_input(int k, gpio_num_t pin)
{
    if (gpio_get_level(pin)) {
        if (s_btn_high[k] < ARM_TICKS && ++s_btn_high[k] == ARM_TICKS) s_btn_armed[k] = true;
        return false;
    }
    s_btn_high[k] = 0;
    return s_btn_armed[k];
}
#define RETUNE_SETTLE_TICKS 10          /* 500 ms after a PHY transaction */
#define CONTROL_STACK_BYTES 6144u

typedef struct {
    StaticTask_t task;
    StackType_t stack[CONTROL_STACK_BYTES/sizeof(StackType_t)] __attribute__((aligned(portBYTE_ALIGNMENT)));
} control_memory_t;
static control_memory_t *s_control_memory;

bool control_prepare(void)
{
    if(!s_control_memory) {
        s_control_memory=heap_caps_aligned_alloc(portBYTE_ALIGNMENT,sizeof(*s_control_memory),
            MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(s_control_memory)memset(s_control_memory,0,sizeof(*s_control_memory));
    }
    return s_control_memory!=NULL;
}

volatile int s_signal_strength;
volatile bool s_scan_active;
volatile int s_cfo_khz;
volatile bool s_afc_locked;
volatile bool s_afc_enabled = true;
volatile std_mode_t s_std_mode = STD_MODE_AUTO;
volatile bool s_detected_std_valid;
volatile video_standard_t s_detected_std = VIDEO_STD_NTSC;
volatile int64_t s_last_video_us;

/* One control-task scratch buffer: the control window (gain), then the
 * longer AFC window over it. */
#define AFC_WINDOW_BYTES C5V4_LEVEL_SAMPLE_BYTES
#define CONTROL_SCRATCH_BYTES (CONTROL_BYTES > AFC_WINDOW_BYTES ? CONTROL_BYTES : AFC_WINDOW_BYTES)
static uint8_t s_control_scratch[CONTROL_SCRATCH_BYTES];
static uint8_t *const s_window = s_control_scratch;
static uint8_t *const s_afc_window = s_control_scratch;
static uint8_t s_pal_votes, s_ntsc_votes;
static unsigned s_afc_corrections, s_afc_weak_skips;
typedef struct {
    afc2_evidence_t history;
    uint64_t sampled_us, qualified_us;
    uint32_t context, qualified_gain_epoch;
    int32_t raw_porch_khz, residual_khz;
    int cn_x10;
    bool latest_valid, sticky_lock;
    const char *window, *held;
} afc_evidence_snapshot_t;
static afc_evidence_snapshot_t s_afc_evidence = {.window="boot", .held="boot", .cn_x10=CN_UNKNOWN_X10};
/* Learned offset sign (afc_ctrl.h sign check), persisted once observed. */
static volatile int8_t s_afc_sign = 1;
static volatile uint8_t s_afc_sign_state;
static bool s_afc_sign_saved;

static void standard_reset(void)
{
    s_pal_votes = s_ntsc_votes = 0;
    s_detected_std_valid = false;
}

/* Burst-confirmed standard from the AFC estimator; a few agreeing
 * windows make it stable. */
static void standard_vote(unsigned afc_standard)
{
    bool pal = afc_standard == 2u;
    if (pal) { if (s_pal_votes < 8u) ++s_pal_votes; if (s_ntsc_votes) --s_ntsc_votes; }
    else { if (s_ntsc_votes < 8u) ++s_ntsc_votes; if (s_pal_votes) --s_pal_votes; }
    if (s_pal_votes >= 3u && s_pal_votes >= s_ntsc_votes + 2u) { s_detected_std = VIDEO_STD_PAL; s_detected_std_valid = true; }
    else if (s_ntsc_votes >= 3u && s_ntsc_votes >= s_pal_votes + 2u) { s_detected_std = VIDEO_STD_NTSC; s_detected_std_valid = true; }
}

/* 0..100 from phase coherence, envelope and gain headroom. */
static int strength(const metrics_t *m, uint8_t gain)
{
    if (m->q_phase < 18 || m->origin_pm > 850) return 0;
    int coherence = (m->q_phase - 18) * 100 / 62;
    int power = ((m->p_median >> 4) - 6) * 100 / 28;   /* ultrafine lane: 4^2 */
    int max_gain = rf_gain_table()->max_index;
    int headroom = (max_gain - (int)gain) * 100 / (max_gain > 2 ? max_gain - 2 : 1);
    coherence = coherence < 0 ? 0 : coherence > 100 ? 100 : coherence;
    power = power < 0 ? 0 : power > 100 ? 100 : power;
    headroom = headroom < 0 ? 0 : headroom > 100 ? 100 : headroom;
    return (coherence * 2 + power + headroom) / 4;
}

static void after_retune(void)
{
    ++s_control_epoch;
    s_cfo_khz = 0;
    s_afc_locked = false;
    standard_reset();
    settings_save();
    profile_channel_changed();   /* another channel may carry another VTX */
    printf("[CHANNEL] %s %u MHz%s\n", rf_channel()->name, rf_channel()->freq_mhz,
           rf_freq_in_spec(rf_channel()->freq_mhz) ? "" : " (above the documented C5 range, not characterised)");
}

static uint32_t afc_context(void)
{
    const uint32_t parts[5] = {(uint32_t)rf_channel_index(), s_control_epoch, (uint32_t)rf_offset_khz(),
                               rf_table_generation(), phy_generation()};
    uint32_t h = 2166136261u;
    for (unsigned k = 0; k < 5u; ++k) h = (h ^ parts[k]) * 16777619u;
    return h;
}

/* End-to-end video quality, one-second periods (video_quality.h). */
static vq_acc_t s_vq_acc;
static vq_t s_vq;
static uint32_t s_vq_periods;

/* copy_snapshot() under the same scheduler hold as copy_control_window():
 * a preempted copy fails its overrun check. */
/* Picture measure totals and its CPU cost per window (T PICTURE). */
static struct { picture_state_t state; uint32_t found, expected, windows, us_max; uint64_t us_sum; } s_pic;

static bool copy_afc_window(uint8_t *dst)
{
    vTaskSuspendAll();
    const bool ok = copy_snapshot(dst);
    (void)xTaskResumeAll();
    return ok;
}

static void control_task(void *arg)
{
    (void)arg;
    afc2_ctrl_t afc = {.target_khz = AFC_PORCH_TARGET_KHZ, .sign = s_afc_sign, .sign_state = s_afc_sign_state};
    uint8_t seen_sign_state = afc.sign_state;
    bool own_write = false;
    uint8_t afc_lost = 0;
    unsigned settle = RETUNE_SETTLE_TICKS, btn = 0, boot_grace = 20;
    bool was_locked = false;
    uint32_t seen_phy = phy_generation(), seen_control = s_control_epoch;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        /* c5div-rx: the BOOT button is the only control.
         *   press           next raceband channel (R1..R8, then R1)
         *   hold 2 s        back to R1: with one button wired to both
         *                   receivers of a diversity pair this re-aligns them
         *   hold 8 s        one-time filter calibration, video stops ~30 s
         *                   (C5VRX-5 menu CALIBRATE > QUICK / console F)
         * BOOT and D10 (external, to GND) act the same. Calibration and the
         * boot window own TX: no button, no AFC. */
        if (status_hold_active() || boot_window_active()) { btn = 0; continue; }
        const bool b_boot = button_input(0, BOOT_BUTTON), b_ext = button_input(1, EXT_BUTTON);
        if (boot_grace) { --boot_grace; btn = 0; }
        else if (b_boot || b_ext) {
            ++btn;
            if (btn == STUCK_TICKS) {
                if (b_boot) s_btn_armed[0] = false;
                if (b_ext) s_btn_armed[1] = false;
                printf("[BUTTON] %s held 15 s: ignored until released\n", b_ext ? "D10" : "BOOT");
                btn = 0;
            } else if (btn == HOLD_R1_TICKS) {
                printf("[BUTTON] hold: R1\n");
                if (rf_channel_index() != 0 && rf_set_channel(0) == ESP_OK) after_retune();
            } else if (btn == HOLD_CAL_TICKS) {
                printf("[BUTTON] long hold: filter calibration\n");
                filter_request();
            }
        } else if (btn) {
            if (btn >= 2 && btn < HOLD_R1_TICKS) { rf_next_channel(); after_retune(); }
            btn = 0;
        }

        transport_poll_faults();
        if (phy_generation() != seen_phy || s_control_epoch != seen_control) {
            seen_phy = phy_generation();
            seen_control = s_control_epoch;
            s_cfo_khz = 0;
            afc2_ctrl_invalidate(&afc);
            settle = own_write ? 2u : RETUNE_SETTLE_TICKS;
        }
        uint32_t gain_epoch = s_gain_epoch, ctx = afc_context();
        if (s_scan_active) {
            afc2_ctrl_invalidate(&afc);
            memset(&s_afc_evidence, 0, sizeof(s_afc_evidence));
            s_afc_evidence.window="scan"; s_afc_evidence.held="scan";
            s_afc_evidence.cn_x10=CN_UNKNOWN_X10;
            continue;
        }
        const bool copied = copy_control_window(s_window, CONTROL_BYTES);
        metrics_t m = {0};
        if (copied) {
            m = measure_window(s_window, CONTROL_BYTES);
            s_signal_strength = (s_signal_strength * 3 + strength(&m, s_current_gain) + 2) / 4;
        }

        /* AFC: burst/line-timing-confirmed sync/porch levels, never the
         * scene-biased FM slope; acquisition only, sticky once locked. */
        if (afc2_ctrl_sync(&afc, ctx, own_write)) { s_afc_locked = false; afc_lost = 0; }
        own_write = false;
        int64_t now = esp_timer_get_time();
        /* Preserve the receiver-settling gate independently of the retired
         * kp ladder: AFC must never use samples spanning a gain/context write. */
        const bool receiver_ok = copied && settle == 0 && ctx == afc_context() && !phy_busy() &&
                                 now - s_last_gain_write_us >= 5000;
        /* The AFC window holds 3.2 lines, not the 1.6 of one descriptor:
         * without a detected burst a sync counts only with a second one a
         * line earlier, which one descriptor contains in ~43 % of windows
         * (board IQ replay 2026-10-26: 45 % vs 30/30 snapshots). It ends
         * where the active descriptor began, so it covers s_window. */
        const bool afc_copied = receiver_ok && copy_afc_window(s_afc_window);
        bool window_ok = afc_copied && afc2_envelope_stationary(s_afc_window, AFC_WINDOW_BYTES);
        /* Picture measure (picture_sync.h): every live window, also those the
         * AFC leaves out (a gain write, settle, fading): the goggles see those. */
        picture_sync_t ps = {0, 0};
        if (copied && (afc_copied || copy_afc_window(s_afc_window))) {
            unsigned bits;
            const uint16_t *lut = transport_decoder_lut(&bits);
            if (lut) {
                const int64_t t0 = esp_timer_get_time();
                ps = picture_sync_window(&s_pic.state, s_afc_window, AFC_WINDOW_BYTES, lut, bits);
                const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
                s_pic.found += ps.found; s_pic.expected += ps.expected; ++s_pic.windows;
                s_pic.us_sum += us;
                if (us > s_pic.us_max) s_pic.us_max = us;
            }
        }
        const char *afc_window_reason = !copied ? "copy_missing" : settle ? "settle" :
            ctx != afc_context() ? "context" : phy_busy() ? "phy_busy" : !receiver_ok ? "gain_settle" :
            !afc_copied ? "afc_copy_missing" : !window_ok ? "nonstationary" : "measured";
        if (settle) --settle;
        afc2_result_t r = {0};
        if (window_ok) {
            r = afc2_measure(s_afc_window, AFC_WINDOW_BYTES, c5vrx_phase8_gain_lut);
            afc2_ctrl_observe_at(&afc, &r, (uint64_t)now);
            vq_add(&s_vq_acc, &r);
        } else afc2_ctrl_miss(&afc);   /* settle and context changes reset via afc2_ctrl_sync */
        if (vq_close(&s_vq_acc, now, &s_vq)) ++s_vq_periods;
        if (copied) s_cfo_khz = r.porch_pairs ? r.porch_khz : 0;
        /* Burst or line timing confirmed the syncs (afc2_measure). */
        bool valid = window_ok && r.lines && r.standard && r.porch_pairs && r.sync_pairs;
        const bool qualified=valid && r.polarity && gain_epoch==s_gain_epoch && ctx==afc_context();
        if (s_afc_evidence.context != ctx) s_afc_evidence.qualified_us=0;
        if (qualified) {
            s_afc_evidence.qualified_us=(uint64_t)now;
            s_afc_evidence.qualified_gain_epoch=gain_epoch;
            s_afc_evidence.raw_porch_khz=r.porch_khz;
            s_afc_evidence.residual_khz=r.porch_khz-afc.target_khz;
        }
        if (valid) { s_last_video_us = now; standard_vote(r.standard); profile_observe(&r); }
        if (!s_afc_enabled && rf_offset_khz() != 0) {
            rf_set_offset_khz(0);
            s_last_gain_write_us = esp_timer_get_time();
            ++s_gain_epoch;
        }
        bool was = s_afc_locked;
        if (!s_afc_enabled) { s_afc_locked = false; afc_lost = 0; }
        else if (copied) s_afc_locked = afc2_native_lock(s_afc_locked, valid, afc2_ctrl_can_lock(&afc, true), afc.n, &afc_lost);
        if (was && !s_afc_locked) afc2_ctrl_reacquire(&afc);
        /* Re-acquisition retunes only on a strong carrier: at the range edge
         * the porch estimate is pulled toward the receiver centre and would
         * walk the offset the wrong way (cn_estimate.h, afc_bias.py). A weak
         * window still feeds the estimate history, it just cannot step. */
        const int cn_x10 = receiver_obs_cn_x10();
        const char *afc_held = !s_afc_enabled ? "afc_off" : s_afc_locked ? "sticky_lock" : !valid ? "window" : "actuator";
        if (s_afc_enabled && !s_afc_locked && valid &&
            phy_try_actuator(phy_generation())) {
            int32_t step = 0;
            const bool strong = receiver_obs_cn_x10() >= AFC_MIN_CN_X10;   /* noise-referenced power C/N */
            if (afc2_ctrl_decide(&afc, gain_epoch == s_gain_epoch && strong, &step)) {
                rf_set_offset_khz(rf_offset_khz() + (int)step);
                s_last_gain_write_us = esp_timer_get_time();   /* restore re-forces gain */
                ++s_gain_epoch;
                ++s_afc_corrections;
                own_write = true;
                afc_held="stepped";
                printf("[AFC] step_khz=%ld offset_khz=%d cn_est_x10=%d coherence=%d gain=%u\n", (long)step,
                       rf_offset_khz(), cn_x10, s_obs_coherence, s_current_gain);
            } else if (!strong && afc2_ctrl_stable(&afc, &step) &&
                       (step > AFC2_CTRL_DEADBAND_KHZ || step < -AFC2_CTRL_DEADBAND_KHZ)) {
                ++s_afc_weak_skips;    /* would have stepped on a weak carrier */
            }
            if (!own_write) {
                int32_t median=0;
                afc_held=gain_epoch != s_gain_epoch ? "gain_changed" : !strong ? "weak_or_unknown_cn" :
                    afc.n<AFC2_CTRL_SAMPLES ? "collecting" : !afc2_ctrl_stable(&afc,&median) ? "unstable" :
                    (median>=-AFC2_CTRL_DEADBAND_KHZ && median<=AFC2_CTRL_DEADBAND_KHZ) ? "deadband" : "correction_budget";
            }
            phy_end_actuator();
        }
        s_afc_evidence.history=afc2_ctrl_evidence(&afc,(uint64_t)now);
        s_afc_evidence.sampled_us=(uint64_t)now;
        s_afc_evidence.context=ctx;
        s_afc_evidence.cn_x10=cn_x10;
        s_afc_evidence.latest_valid=qualified;
        s_afc_evidence.sticky_lock=s_afc_locked;
        s_afc_evidence.window=qualified ? "valid" : valid ? "stale_gain_or_context" : window_ok ? "no_confirmed_sync" : afc_window_reason;
        s_afc_evidence.held=afc_held;
        if (afc.sign_state != seen_sign_state) {
            seen_sign_state = afc.sign_state;
            s_afc_sign_state = afc.sign_state;
            printf("[AFC] offset sign %s: %+d\n", afc.sign_state == AFC2_SIGN_FLIPPED ? "FLIPPED" : "confirmed",
                   afc.sign < 0 ? -1 : 1);
            int8_t sign = afc.sign < 0 ? -1 : 1;
            if (!s_afc_sign_saved || sign != s_afc_sign) {
                s_afc_sign = sign;
                s_afc_sign_saved = blob_store("afc_sign", &sign, 1);
            }
        }
        if (s_afc_locked && !was_locked)
            printf("[VIDEO] locked %s %u MHz %s offset_khz=%d gain=%u\n", rf_channel()->name,
                   rf_channel()->freq_mhz, r.standard == 2 ? "PAL" : "NTSC", rf_offset_khz(), s_current_gain);
        was_locked = s_afc_locked;
    }
}

void control_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON) | (1ULL << EXT_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    printf("BUTTON inputs at start: BOOT(GPIO28)=%d D10(GPIO10)=%d (1 = released)\n", gpio_get_level(BOOT_BUTTON),
           gpio_get_level(EXT_BUTTON));
    /* AFC is always on (C5VRX-5's saved "afc_off" menu choice is ignored);
     * the live profile switch stays in its C5VRX-5 default (report only). */
    profile_set_live_mode(false);
    int8_t sign;
    if (blob_load("afc_sign", &sign, 1) && (sign == 1 || sign == -1)) {
        s_afc_sign = sign;
        s_afc_sign_state = AFC2_SIGN_CONFIRMED;
        s_afc_sign_saved = true;
    }
    /* IDF task stack depths are bytes. Reserve before driver allocations,
     * then supply that same 6144-byte stack and TCB without another malloc. */
    ESP_ERROR_CHECK(control_prepare() &&
        xTaskCreateStatic(control_task,"control",CONTROL_STACK_BYTES,NULL,3,
            s_control_memory->stack,&s_control_memory->task)!=NULL ? ESP_OK:ESP_ERR_NO_MEM);
}

/* Status beat for the web menu (web/receiver-console.js): one short line
 * every 2 s from the console task, so the menu follows the receiver
 * without a status request. Read-only. */
void control_print_beat(void)
{
    /* video: confirmed sync within 0.5 s (the web menu's VIDEO/SEARCH);
     * afc_locked alone said SEARCH over a good picture without a lock. */
    const int64_t video_at = s_last_video_us;
    printf("STATUS channel=%s freq=%u afc_locked=%u video=%u standard=%s detected=%s signal=%d hold=%u mode=%s demod=%s "
           "output=%s live=%u busy=%u fw=%s\n",
           rf_channel()->name, rf_frequency_mhz(), s_afc_locked,
           video_at && esp_timer_get_time() - video_at < 500000,
           s_std_mode == STD_MODE_AUTO ? "AUTO" : s_std_mode == STD_MODE_PAL ? "PAL" : "NTSC",
           s_detected_std_valid ? (s_detected_std == VIDEO_STD_PAL ? "PAL" : "NTSC") : "none",
           s_signal_strength, tx_hold_active(), profile_name(profile_active()), transport_program_name(), transport_output_name(),
           profile_live_mode(), status_hold_active(), firmware_id());
}

/* End-to-end video quality of the last second (video_quality.h). */
void control_video_quality(vq_t *out) { vTaskSuspendAll(); *out = s_vq; (void)xTaskResumeAll(); }

void control_print(void)
{
    vTaskSuspendAll();
    const afc_evidence_snapshot_t afc_snapshot=s_afc_evidence;
    const uint64_t afc_now=(uint64_t)esp_timer_get_time();
    const bool afc_context_current=afc_snapshot.context==afc_context();
    const bool afc_gain_current=afc_snapshot.qualified_us && afc_snapshot.qualified_gain_epoch==s_gain_epoch;
    (void)xTaskResumeAll();
    const afc2_evidence_t evidence=afc_snapshot.history;
    printf("AFC_EVIDENCE latest_valid=%u context_current=%u qualified_gain_current=%u sticky_lock=%u target_khz=%d "
           "last_qualified_raw_porch_khz=%ld last_qualified_residual_khz=%ld qualified_age_ms=%lld sample_age_ms=%lld "
           "n=%u timestamped=%u stable=%u median_residual_khz=%ld mad_khz=%ld oldest_age_ms=%lld newest_age_ms=%lld "
           "cn_x10=%d window=%s held=%s reference=porch_not_rf_truth\n",
           afc_snapshot.latest_valid, afc_context_current, afc_gain_current, afc_snapshot.sticky_lock, AFC_PORCH_TARGET_KHZ,
           (long)afc_snapshot.raw_porch_khz, (long)afc_snapshot.residual_khz,
           (long long)afc2_evidence_age_ms(afc_context_current && afc_gain_current ? afc_snapshot.qualified_us : 0,afc_now),
           (long long)afc2_evidence_age_ms(afc_snapshot.sampled_us,afc_now),
           evidence.n, evidence.timestamped, evidence.stable, (long)evidence.median_khz, (long)evidence.mad_khz,
           (long long)afc2_evidence_age_ms(evidence.oldest_us,afc_now),
           (long long)afc2_evidence_age_ms(evidence.newest_us,afc_now),
           afc_snapshot.cn_x10, afc_snapshot.window, afc_snapshot.held);
    printf("CONTROL channel=%s freq=%u offset_khz=%d cfo_khz=%d afc_locked=%u afc_corrections=%u "
           "afc_weak_skips=%u afc_sign=%+d(%s) standard=%s detected=%s signal=%d menu=%u deviation_x100=%d mode=%s "
           "demod=%s busy=%u\n",
           rf_channel()->name, rf_frequency_mhz(), rf_offset_khz(), s_cfo_khz, s_afc_locked,
           s_afc_corrections, s_afc_weak_skips, s_afc_sign,
           s_afc_sign_state == AFC2_SIGN_FLIPPED ? "flipped" : s_afc_sign_state == AFC2_SIGN_CONFIRMED ? "confirmed" :
           "unverified", s_std_mode == STD_MODE_AUTO ? "AUTO" : s_std_mode == STD_MODE_PAL ? "PAL" : "NTSC",
           s_detected_std_valid ? (s_detected_std == VIDEO_STD_PAL ? "PAL" : "NTSC") : "none",
           s_signal_strength, tx_hold_active(), profile_deviation_x100(rf_channel_index()),
           profile_name(profile_active()), transport_program_name(), status_hold_active());
    vq_t vq;
    control_video_quality(&vq);
    printf("VIDEO_Q known=%u snr_x10=%d lines_x100=%u lines=%lu windows=%lu periods=%lu (end-to-end: sync/porch "
           "spread of the demodulated video, last second)\n", vq.known, vq.snr_x10, vq.lines_x100,
           (unsigned long)vq.lines, (unsigned long)vq.windows, (unsigned long)s_vq_periods);
    printf("PICTURE found=%lu expected=%lu windows=%lu us_mean=%lu us_max=%lu (picture_sync.h: sync pulses in the "
           "decoded control windows since boot, apart from the AFC's video_pct; CPU per window)\n",
           (unsigned long)s_pic.found, (unsigned long)s_pic.expected, (unsigned long)s_pic.windows,
           (unsigned long)(s_pic.windows ? s_pic.us_sum / s_pic.windows : 0), (unsigned long)s_pic.us_max);
}

