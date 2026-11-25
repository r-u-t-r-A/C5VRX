/* c5div-rx channel link: one line of text, master -> slave and Pi.
 *
 *   $C5,R3,5732*hh\r\n      hh = XOR of the characters between $ and *
 *
 * Accepted only if the checksum, the channel and its frequency all agree.
 * Pure logic, host-tested (test/test_link.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINK_BAUD       9600
#define LINK_LINE_MAX   24      /* longest accepted line, without $ and line end */
#define LINK_CHANNELS   8u

/* Raceband, as in rf.c. */
static const uint16_t link_mhz[LINK_CHANNELS] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};

typedef struct {
    char line[LINK_LINE_MAX + 1];
    uint8_t n;
    bool in_line;
} link_parser_t;

static inline uint8_t link_sum(const char *s, unsigned n)
{
    uint8_t x = 0;
    while (n--) x ^= (uint8_t)*s++;
    return x;
}

/* Message for channel index 0..7 (R1..R8); returns its length. */
static inline int link_format(char *out, unsigned size, unsigned channel)
{
    char body[16];
    if (channel >= LINK_CHANNELS) return 0;
    const int n = snprintf(body, sizeof(body), "C5,R%u,%u", channel + 1u, link_mhz[channel]);
    return snprintf(out, size, "$%s*%02X\r\n", body, link_sum(body, (unsigned)n));
}

/* Feed one received byte; the channel index when it completes a valid
 * message, otherwise -1. Anything malformed is dropped until the next $. */
static inline int link_feed(link_parser_t *p, uint8_t c)
{
    if (c == '$') { p->n = 0; p->in_line = true; return -1; }
    if (!p->in_line) return -1;
    if (c != '\r' && c != '\n') {
        if (c < 0x20 || c > 0x7e || p->n >= LINK_LINE_MAX) { p->in_line = false; return -1; }
        p->line[p->n++] = (char)c;
        return -1;
    }
    p->in_line = false;
    p->line[p->n] = 0;
    char want[LINK_LINE_MAX + 4];
    for (unsigned ch = 0; ch < LINK_CHANNELS; ++ch) {
        const int n = link_format(want, sizeof(want), ch);
        /* want: $ line \r\n */
        if (n == p->n + 3 && memcmp(want + 1, p->line, p->n) == 0) return (int)ch;
    }
    return -1;
}

/* When to send: at once on a change and twice more, then every 5 s. */
#define LINK_PERIOD_US  5000000
#define LINK_REPEAT_US  100000
#define LINK_REPEATS    2
typedef struct {
    int channel;                /* last sent, -1 none */
    uint8_t repeats;
    int64_t sent_us;
} link_sender_t;

static inline bool link_due(link_sender_t *s, int channel, int64_t now_us)
{
    if (channel != s->channel) { s->channel = channel; s->repeats = LINK_REPEATS; }
    else if (s->repeats && now_us - s->sent_us >= LINK_REPEAT_US) --s->repeats;
    else if (now_us - s->sent_us < LINK_PERIOD_US) return false;
    s->sent_us = now_us;
    return true;
}
