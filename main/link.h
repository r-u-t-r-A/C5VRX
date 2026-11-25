/* c5div-rx channel link between the receivers of a pair (link.c). Control
 * task only, except link_role(). */
#pragma once
#include <stdbool.h>

void link_start(void);
void link_poll(void);
bool link_is_slave(void);       /* a master has been heard since boot: D10 is data */
bool link_alive(void);          /* slave, and heard within 15 s */
int link_take_channel(void);    /* channel the master asked for (0..7), or -1 */
const char *link_role(void);
