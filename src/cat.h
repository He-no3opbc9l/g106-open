/* CAT: Icom CI-V protocol on USART1 as the stock firmware (19200 8N1), see cat.c. */
#ifndef CAT_H
#define CAT_H

#include <stdbool.h>

void cat_init(void);
void cat_poll(void);            /* call from the main loop */
bool cat_ptt(void);             /* TX requested over CAT (CI-V 1C 00 01) */
bool cat_link(void);            /* frames received recently (CAT icon) */

#endif
