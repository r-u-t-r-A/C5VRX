/* Channel link: message format, parser and send schedule (link_proto.h). */
#include <assert.h>
#include <stdlib.h>
#include "link_proto.h"

static int feed(link_parser_t *p, const char *s)
{
    int r = -1;
    for (; *s; ++s) { const int c = link_feed(p, (uint8_t)*s); if (c >= 0) r = c; }
    return r;
}

int main(void)
{
    char m[32];
    link_parser_t p = {0};
    for (unsigned ch = 0; ch < LINK_CHANNELS; ++ch) {
        assert(link_format(m, sizeof(m), ch) > 0 && strlen(m) < 64);
        assert(feed(&p, m) == (int)ch);
    }
    assert(link_format(m, sizeof(m), LINK_CHANNELS) == 0);
    /* shared with the Pi's test: keep this line */
    link_format(m, sizeof(m), 2);
    assert(strcmp(m, "$C5,R3,5732*14\r\n") == 0);

    /* every single-byte corruption and every truncation is refused */
    for (unsigned ch = 0; ch < LINK_CHANNELS; ++ch) {
        const int n = link_format(m, sizeof(m), ch);
        for (int i = 0; i < n - 2; ++i)
            for (int v = 0; v < 256; ++v) {
                if ((char)v == m[i]) continue;
                char x[32];
                memcpy(x, m, (size_t)n + 1);
                x[i] = (char)v;
                link_parser_t q = {0};
                for (int k = 0; k < n; ++k) assert(link_feed(&q, (uint8_t)x[k]) < 0);
            }
        for (int cut = 1; cut < n - 2; ++cut) {
            link_parser_t q = {0};
            int r = -1;
            for (int k = 0; k < n; ++k) if (k != cut) { const int c = link_feed(&q, (uint8_t)m[k]); if (c >= 0) r = c; }
            assert(r < 0);
        }
    }
    /* frequency and channel must agree even with a correct checksum */
    char body[] = "C5,R3,5769", line[32];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, link_sum(body, (unsigned)strlen(body)));
    p = (link_parser_t){0};
    assert(feed(&p, line) < 0);

    /* boot log, button bounce, noise, overlong lines before a good message */
    assert(feed(&p, "ESP-ROM:esp32c5 rst:0x1\r\n$$$C5,R") < 0);
    link_format(m, sizeof(m), 6);
    assert(feed(&p, m) == 6);
    assert(feed(&p, "$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n") < 0 && feed(&p, m) == 6);
    srand(1);
    for (int i = 0; i < 2000000; ++i) assert(link_feed(&p, (uint8_t)(rand() >> 7)) < 0);
    feed(&p, "\n");
    assert(feed(&p, m) == 6);

    /* schedule: at once, two repeats, then every 5 s; again on a change */
    link_sender_t s = {.channel = -1};
    int sent = 0;
    for (int t = 0; t <= 12000; t += 50) {
        const bool due = link_due(&s, t < 11000 ? 2 : 3, (int64_t)t * 1000);
        if (due) ++sent;
        if (t == 0 || t == 100 || t == 200 || t == 5200 || t == 10200 || t == 11000 || t == 11100 || t == 11200) assert(due);
    }
    assert(sent == 8);
    puts("link ok");
    return 0;
}
