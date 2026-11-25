#pragma once
#include <stdint.h>
#include <string.h>
typedef int uart_dev_t;
#define UART_LL_GET_HW(n) ((uart_dev_t *)0)
extern uint8_t stub_rxbuf[256], stub_txbuf[4096];
extern uint32_t stub_rxn, stub_txn;
static inline uint32_t uart_ll_get_rxfifo_len(uart_dev_t *h) { (void)h; return stub_rxn; }
static inline void uart_ll_read_rxfifo(uart_dev_t *h, uint8_t *b, uint32_t n)
{ (void)h; memcpy(b, stub_rxbuf, n); stub_rxn -= n; memmove(stub_rxbuf, stub_rxbuf + n, stub_rxn); }
static inline uint32_t uart_ll_get_txfifo_len(uart_dev_t *h) { (void)h; return 128; }
static inline void uart_ll_write_txfifo(uart_dev_t *h, const uint8_t *b, uint32_t n)
{ (void)h; memcpy(stub_txbuf + stub_txn, b, n); stub_txn += n; }
