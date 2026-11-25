#pragma once
#include "esp_err.h"
enum { GPIO_NUM_10 = 10, GPIO_NUM_11 = 11 };
extern int stub_open_drain;
static inline esp_err_t gpio_od_enable(int p) { stub_open_drain = p; return ESP_OK; }
static inline esp_err_t gpio_pullup_en(int p) { (void)p; return ESP_OK; }
