#pragma once
#include <stdint.h>
/* Preserve LK's configured scanout. Bounded refresh, then return to loader. */
void display_lk_preserve(uintptr_t dtb);

int display_lk_console_init(uintptr_t dtb);
int display_lk_present(void);
void display_lk_stop(void);
