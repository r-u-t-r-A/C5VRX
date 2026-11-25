#pragma once
#include "esp_err.h"
enum { UART_NUM_1 = 1, UART_DATA_8_BITS = 3, UART_PARITY_DISABLE = 0, UART_STOP_BITS_1 = 1, UART_HW_FLOWCTRL_DISABLE = 0,
       UART_SCLK_XTAL = 1, UART_PIN_NO_CHANGE = -1 };
typedef struct { int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk; } uart_config_t;
extern int stub_baud, stub_tx, stub_rx;
static inline esp_err_t uart_param_config(int u, const uart_config_t *c) { (void)u; stub_baud = c->baud_rate; return ESP_OK; }
static inline esp_err_t uart_set_pin(int u, int tx, int rx, int a, int b)
{ (void)u; (void)a; (void)b; stub_tx = tx; stub_rx = rx; return ESP_OK; }
