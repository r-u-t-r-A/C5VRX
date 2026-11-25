/* c5div-rx channel link (link_proto.h): keeps the two receivers of a
 * diversity pair on one channel and tells the Pi which one.
 *
 *   master D6 (GPIO11, open drain) ---> slave D10 (GPIO10)
 *                                  \--> Pi GPIO15 / RXD (pin 10)
 *
 * The same firmware runs on both. A receiver that gets a valid message on
 * D10 is the slave: it follows the messages, sends nothing and does not use
 * D10 as a button any more. A receiver that gets none is the master (or on
 * its own): D10 is its button. D0/D1/D2 carry IQ, and D6 is the ROM's boot
 * log output, hence the different pins for sending and receiving.
 *
 * Polled from the control task through the UART's FIFOs (128 bytes, 133 ms
 * at 9600 baud): no driver, no interrupt, no buffers. */
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "hal/uart_ll.h"
#include "link.h"
#include "link_proto.h"
#include "rf.h"

#define LINK_UART       UART_NUM_1
#define LINK_TX_GPIO    GPIO_NUM_11     /* D6 */
#define LINK_RX_GPIO    GPIO_NUM_10     /* D10 */
#define LINK_ALIVE_US   15000000        /* three periods */

static link_parser_t s_parser;
static link_sender_t s_sender = {.channel = -1};
static bool s_started, s_slave;
static int s_request = -1;
static int64_t s_heard_us;
static uint32_t s_messages;

void link_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_XTAL,
    };
    esp_err_t err = uart_param_config(LINK_UART, &cfg);
    if (err == ESP_OK) err = uart_set_pin(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK) {
        /* Open drain: two senders on one wire, or a button still connected to
         * it, cannot fight. */
        gpio_od_enable(LINK_TX_GPIO);
        gpio_pullup_en(LINK_TX_GPIO);
        gpio_pullup_en(LINK_RX_GPIO);
        s_started = true;
    } else {
        printf("[LINK] unavailable: %s\n", esp_err_to_name(err));   /* video works without it */
    }
}

/* Once per control tick. */
void link_poll(void)
{
    if (!s_started) return;
    uart_dev_t *hw = UART_LL_GET_HW(LINK_UART);
    for (uint32_t n = uart_ll_get_rxfifo_len(hw); n; --n) {
        uint8_t c;
        uart_ll_read_rxfifo(hw, &c, 1);
        const int ch = link_feed(&s_parser, c);
        if (ch < 0) continue;
        if (!s_slave) printf("[LINK] slave: following the master on D10\n");
        s_slave = true;
        s_heard_us = esp_timer_get_time();
        s_request = ch;
        ++s_messages;
    }
    if (s_slave) return;
    const size_t index = rf_channel_index();
    if (index >= LINK_CHANNELS) return;
    char msg[LINK_LINE_MAX + 4];
    if (link_due(&s_sender, (int)index, esp_timer_get_time())) {
        const int n = link_format(msg, sizeof(msg), (unsigned)index);
        if (uart_ll_get_txfifo_len(hw) >= (uint32_t)n) {
            uart_ll_write_txfifo(hw, (const uint8_t *)msg, (uint32_t)n);
            ++s_messages;
        }
    }
}

bool link_is_slave(void) { return s_slave; }
bool link_alive(void) { return s_slave && esp_timer_get_time() - s_heard_us < LINK_ALIVE_US; }

int link_take_channel(void)
{
    const int ch = s_request;
    s_request = -1;
    return ch;
}

const char *link_role(void) { return !s_started ? "off" : !s_slave ? "master" : link_alive() ? "slave" : "slave_silent"; }
