/* Channel link roles: main/link.c on a stub UART. */
#include <assert.h>
#include <stdio.h>
#include "../main/link.c"

static size_t stub_channel;
size_t rf_channel_index(void) { return stub_channel; }
int64_t stub_now;
int stub_open_drain, stub_baud, stub_tx, stub_rx;
uint8_t stub_rxbuf[256], stub_txbuf[4096];
uint32_t stub_rxn, stub_txn;

static void rx(const char *s) { memcpy(stub_rxbuf + stub_rxn, s, strlen(s)); stub_rxn += (uint32_t)strlen(s); }
static void run(int ms) { for (int t = 0; t < ms; t += 50) { link_poll(); stub_now += 50000; } }
static unsigned sent(void) { unsigned n = 0; for (uint32_t i = 0; i < stub_txn; ++i) n += stub_txbuf[i] == '$'; stub_txn = 0; return n; }

int main(void)
{
    link_poll();                                    /* before start: nothing */
    assert(stub_txn == 0);
    link_start();
    assert(stub_baud == 9600 && stub_tx == 11 && stub_rx == 10 && stub_open_drain == 11);

    /* alone: master. Three at start, then one every 5 s; three on a change. */
    stub_channel = 2;
    run(1000);
    assert(!memcmp(stub_txbuf, "$C5,R3,5732*14\r\n", 16) && sent() == 3 && !link_is_slave());
    run(20000);
    assert(sent() == 4);
    stub_channel = 3;
    run(1000);
    assert(!memcmp(stub_txbuf, "$C5,R4,", 7) && sent() == 3);

    /* button bounce, boot log and broken messages do not make it a slave */
    rx("\xff\x7f ESP-ROM:esp32c5\r\n$C5,R3,5732*15\r\n$C5,R9,5954*00\r\n");
    run(200);
    assert(!link_is_slave() && link_take_channel() < 0);

    /* a valid message, split over two polls: slave, silent from then on */
    sent();
    char want[32];
    link_format(want, sizeof(want), 6);
    char half[10] = {0};
    memcpy(half, want, 9);
    rx(half);
    run(50);
    assert(!link_is_slave());
    rx(want + 9);
    run(50);
    assert(link_is_slave() && link_alive() && link_take_channel() == 6 && link_take_channel() < 0);
    sent();
    run(14000);
    assert(sent() == 0 && link_alive());
    run(2000);
    assert(sent() == 0 && !link_alive() && link_is_slave());  /* master gone: buttons work again, still silent */

    /* several messages in one poll: the last one counts */
    for (unsigned ch = 0; ch < 3; ++ch) { link_format(want, sizeof(want), ch); rx(want); }
    run(50);
    assert(link_alive() && link_take_channel() == 2);
    puts("link role ok");
    return 0;
}
