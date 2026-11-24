#!/usr/bin/env python3
"""C5VRX by Twotoz/contributors: exercise actual settings.c across reboot.
c5div-rx: assertions adapted to raceband-only settings (see settings.c).

Only NVS and receiver hardware boundaries are stubbed; persistence decisions
are compiled from the production source. No ESP-IDF SDK emulation required.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RECEIVER = r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { STD_MODE_AUTO, STD_MODE_NTSC, STD_MODE_PAL } std_mode_t;
extern std_mode_t s_std_mode;
bool transport_set_output(unsigned level);
bool blob_load(const char *, void *, size_t);
bool blob_store(const char *, const void *, size_t);
void settings_load(void);
void settings_save(void);
void settings_save_output(unsigned);
void receiver_set_gain_mode(uint8_t mode, bool save);
#define SETTINGS_PREFS_VERSION 1u
typedef struct { uint8_t version; int8_t filter_select; uint8_t gain_mode, dfilt_mode; } settings_prefs_t;
bool settings_prefs_load(settings_prefs_t *p);
void settings_prefs_store(const settings_prefs_t *p);
'''
NVS = r'''
#pragma once
#include <stddef.h>
typedef int nvs_handle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
const char *esp_err_to_name(esp_err_t);
'''
RF = r'''
#pragma once
#include <stdbool.h>
#define FPV_CHANNELS 48
#define FPV_BAND_R 0
unsigned rf_channel_index(void);
bool rf_set_channel(unsigned);
'''
HARNESS = r'''
#include "receiver.h"
#include "nvs.h"
#include "rf.h"
#include "output_level.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned char bytes[16]; size_t n; } slot_t;
static slot_t slots[5], pending;
static int pending_index = -1, writes, commits, fail_open, fail_set, fail_commit;
static unsigned output, channel, output_applied, gain_mode, gain_restores;
std_mode_t s_std_mode;
static int index_of(const char *key) {
    if (!strcmp(key, "settings")) return 0;
    if (!strcmp(key, "output")) return 1;
    if (!strcmp(key, "gain_mode")) return 2;
    if (!strcmp(key, "flywheel")) return 3;
    if (!strcmp(key, "prefs")) return 4;
    assert(0); return -1;
}
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    assert(!strcmp(ns, "c5vrx5"));
    if (fail_open) return 1;
    *h = mode; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *n) {
    (void)h; slot_t *s = &slots[index_of(key)];
    if (!s->n || *n < s->n) return 1;
    memcpy(data, s->bytes, s->n); *n = s->n; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t n) {
    assert(h == NVS_READWRITE && n <= sizeof(pending.bytes));
    ++writes;
    if (fail_set) return 1;
    pending_index = index_of(key); pending.n = n;
    memcpy(pending.bytes, data, n); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h == NVS_READWRITE); ++commits;
    if (fail_commit) return 1;
    assert(pending_index >= 0); slots[pending_index] = pending;
    pending_index = -1; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; pending_index = -1; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "TEST_FAILURE"; }
unsigned rf_channel_index(void) { return channel; }
bool rf_set_channel(unsigned c) { channel = c; return true; }
bool transport_set_output(unsigned o) { assert(o < OUTPUT_LEVELS); output = o; ++output_applied; return true; }
void receiver_set_gain_mode(uint8_t mode, bool save) { assert(!save); gain_mode = mode; ++gain_restores; }
static void boot(void) { output = 0; channel = 99; s_std_mode = STD_MODE_AUTO; output_applied = 0;
    gain_mode = 0; gain_restores = 0; settings_load(); }
static void seed(unsigned slot, const unsigned char *bytes, size_t n) {
    assert(n <= sizeof(slots[slot].bytes)); slots[slot].n = n; memcpy(slots[slot].bytes, bytes, n);
}
int main(void) {
    /* c5div-rx: empty NVS boots on R1 (raceband index 0). */
    boot(); assert(channel == 0 && output_applied == 0 && s_std_mode == STD_MODE_AUTO);
    /* A saved raceband channel comes back; the saved standard does not. */
    const unsigned char r6[] = {1, 5, STD_MODE_PAL, 0};
    seed(0, r6, sizeof(r6)); boot(); assert(channel == 5 && s_std_mode == STD_MODE_AUTO);
    /* Any other band left by C5VRX-5 (here index 17, B2) falls back to R1. */
    const unsigned char b2[] = {1, 17, STD_MODE_NTSC, 0};
    seed(0, b2, sizeof(b2)); boot(); assert(channel == 0 && s_std_mode == STD_MODE_AUTO);
    const unsigned char invalid[] = {1, FPV_CHANNELS, 255, 0};
    seed(0, invalid, sizeof(invalid)); boot(); assert(channel == 0);
    const unsigned char unknown_version[] = {2, 3, 0, 0};
    seed(0, unknown_version, sizeof(unknown_version)); boot(); assert(channel == 0);
    /* Saving: one write per change, none for an unchanged channel (wear). */
    slots[0].n = 0; channel = 3; writes = commits = 0; settings_save(); assert(writes == 1 && commits == 1);
    settings_save(); assert(writes == 1); boot(); assert(channel == 3);
    /* A stored OUTPUT level (C5VRX-5 menu) is never applied at boot. */
    for (unsigned level = 0; level < OUTPUT_LEVELS; ++level) {
        const unsigned char b = (unsigned char)level; seed(1, &b, 1);
        boot(); assert(output == 0 && output_applied == 0 && channel == 3);
    }
    /* settings_save_output still persists verified levels without rewrites. */
    slots[1].n = 0; writes = commits = 0; settings_save_output(2); assert(writes == 1 && commits == 1);
    settings_save_output(2); assert(writes == 1);
    settings_save_output(OUTPUT_LEVELS); settings_save_output(1000); assert(writes == 1);
    /* NVS failures never stop the boot on a valid channel. */
    fail_open = 1; boot(); assert(channel == 0); fail_open = 0; boot(); assert(channel == 3);
    channel = 4; writes = commits = 0; fail_set = 1; settings_save(); assert(writes == 1 && !commits);
    fail_set = 0; boot(); assert(channel == 3);
    channel = 4; fail_commit = 1; settings_save(); fail_commit = 0; boot(); assert(channel == 3);
    /* Legacy "gain_mode"/"flywheel" keys are never read or rewritten. */
    const unsigned char old_fixed = 42; seed(2, &old_fixed, 1);
    const unsigned char old_flywheel = 1; seed(3, &old_flywheel, 1);
    writes = commits = 0; boot(); assert(!writes && !commits && !gain_restores && gain_mode == 0);
    /* prefs blob helpers (unused by c5div-rx, kept compiling): round trip. */
    settings_prefs_t p = {SETTINGS_PREFS_VERSION, 1, 254, 0}, q;
    assert(!settings_prefs_load(&q));
    writes = commits = 0; settings_prefs_store(&p); assert(writes == 1 && commits == 1);
    assert(settings_prefs_load(&q) && !memcmp(&p, &q, sizeof(p)));
    settings_prefs_store(&p); assert(writes == 1);
    puts("PASS: actual settings.c (c5div-rx): raceband restore, R1 fallback, no OUTPUT/standard restore, wear and NVS failures");
    return 0;
}
'''


class SettingsTest(unittest.TestCase):
    def test_actual_production_source(self):
        with tempfile.TemporaryDirectory() as name:
            folder = Path(name)
            shutil.copyfile(ROOT / "main/settings.c", folder / "settings.c")
            shutil.copyfile(ROOT / "main/output_level.h", folder / "output_level.h")
            for file, text in (("receiver.h", RECEIVER), ("nvs.h", NVS), ("rf.h", RF), ("harness.c", HARNESS)):
                (folder / file).write_text(text, encoding="utf-8")
            binary = folder / "test_settings"
            subprocess.run([os.environ.get("CC", "cc"), "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(folder), str(folder / "settings.c"), str(folder / "harness.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
