#!/usr/bin/env bash
# Host tests of the receiver core's pure logic (from C5VRX-5's CI, minus the
# tests of modules this fork removed: menu, raster, DAC test, diag, labs,
# AUTO OUTPUT). Needs cc and python3.
set -euo pipefail
cd "$(dirname "$0")/../test"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
CC="cc -O2 -std=c11 -Wall -Wextra -Werror -I../main"
run() { local t=$1; shift; echo "== $t"; "$T/$t" "$@" >/dev/null; }
for t in test_quiet test_profile test_afc test_afc_measure test_control_window test_cn_observation \
         test_transport_recovery test_calibration_noise test_sphase test_dco_slots test_dc_servo \
         test_flightlog test_link; do
  $CC "$t.c" -lm -o "$T/$t"; run "$t"
done
$CC test_dc_trim.c -o "$T/test_dc_trim"; run test_dc_trim
$CC test_dc_live.c -lm -o "$T/test_dc_live"; run test_dc_live ../research/data/walk_2026-10-13
$CC test_agc_table.c ../main/arc_phy.c -o "$T/test_agc_table"; run test_agc_table
$CC -DEXPECT_FADE_HOLD test_gain_fade.c ../main/gain.c ../main/arc_phy.c -lm -o "$T/test_gain_fade"; run test_gain_fade
$CC test_gain_preset.c ../main/gain.c ../main/arc_phy.c -lm -o "$T/test_gain_preset"; run test_gain_preset
for t in test_gain_v7 test_rx_level test_keel_track test_gain_measured test_gain_track test_gain_measure; do
  $CC "$t.c" ../main/gain.c ../main/arc_phy.c -lm -o "$T/$t"
done
for t in test_gain_v7 test_rx_level test_keel_track test_gain_measured test_gain_track; do run "$t"; done
run test_gain_measure ../research/data/walk_2026-10-13 ../research/data/afc_2026-10-18 ../research/data/edge_dc_2026-10-19
$CC -DSKIP_BOUNDS -DAGC_CHECKS -DPICTURE_Q -DWRITE_COST_US=7000 test_agc_compare.c ../main/agc.c ../main/gain.c \
    ../main/arc_phy.c -lm -o "$T/test_agc_compare"; run test_agc_compare
$CC test_agc_options.c ../main/agc.c -lm -o "$T/test_agc_options"; run test_agc_options
$CC test_agc_strong.c ../main/agc.c ../main/gain.c ../main/arc_phy.c -lm -o "$T/test_agc_strong"; run test_agc_strong
$CC test_strong_lane.c -o "$T/test_strong_lane"; run test_strong_lane
$CC test_fold_radius.c ../main/agc.c -lm -o "$T/test_fold_radius"; run test_fold_radius
$CC test_cn_auto.c -lm -o "$T/test_cn_auto"; run test_cn_auto
$CC test_video_quality.c -lm -o "$T/test_video_quality"
run test_video_quality ../research/data/afc_2026-10-18 ../research/data/edge_dc_2026-10-19
$CC test_afc_evidence.c -o "$T/test_afc_evidence"; run test_afc_evidence
cc -O2 -std=gnu11 -Wall -Wextra -Werror -Ilink_stubs -I../main test_link_role.c -o "$T/test_link_role"; run test_link_role
python3 ../tools/dco_preset.py --check ../main/dco_preset.h ../research/data/dco_preset_2026-10-26/*.txt
# Source-level invariants of the receiver core and the fork's boot/settings rules.
cd ..
for t in test_11p_runtime test_afc_telemetry test_bank1_watch_runtime test_boot_memory_runtime \
         test_calibration_search_runtime test_dfilt_runtime test_flash_tool test_gain_map_context \
         test_manual_calibration_runtime test_phase_repair_runtime test_phy_observation_hook \
         test_settings test_sphase_runtime; do
  echo "== $t"; python3 "test/$t.py" >/dev/null 2>&1 || { python3 "test/$t.py"; exit 1; }
done
echo "host tests passed"
