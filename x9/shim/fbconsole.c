/* 8x16 font from this repository's x676c shim, doubled for X9 readability.
 * Pixel writes only; hardware submission belongs to display_lk.c. */
#include <stdint.h>
#include "hw.h"
#include "font.h"
#include "display_lk.h"
#include "fbconsole.h"
#define MARGIN 24u
#define CW 16u
#define CH 32u
#define COLS ((FB_W - 2u * MARGIN) / CW)
#define ROWS ((FB_H - 2u * MARGIN) / CH)
static unsigned col, row, dirty;

void fbconsole_init(void)
{
 col = row = 0; dirty = 1;
 volatile uint32_t *p = (volatile uint32_t *)FB_BASE;
 for (unsigned y = 0; y < FB_H; y++) {
  for (unsigned x = 0; x < FB_STRIDE / 4; x++)
   p[(uint64_t)y * (FB_STRIDE / 4) + x] = 0xff000000u;
  if (!(y & 255u)) wdt_kick();
 }
}

static void newline(void)
{
 col = 0;
 if (++row < ROWS) return;
 /* Scroll only visible console rows, preserving margins and stride padding. */
 volatile uint32_t *p = (volatile uint32_t *)FB_BASE;
 for (unsigned y = MARGIN; y < MARGIN + (ROWS - 1) * CH; y++) {
  for (unsigned x = MARGIN; x < MARGIN + COLS * CW; x++)
   p[(uint64_t)y * (FB_STRIDE / 4) + x] = p[(uint64_t)(y + CH) * (FB_STRIDE / 4) + x];
  if (!(y & 255u)) wdt_kick();
 }
 row = ROWS - 1;
 for (unsigned y = MARGIN + row * CH; y < MARGIN + (row + 1) * CH; y++)
  for (unsigned x = MARGIN; x < MARGIN + COLS * CW; x++)
   p[(uint64_t)y * (FB_STRIDE / 4) + x] = 0xff000000u;
 dirty = 1;
}

void fbconsole_putc(char c)
{
 if (c == '\r') { col = 0; return; }
 if (c == '\n') { newline(); return; }
 if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
 if (col == COLS) newline();
 const unsigned char *glyph = font8x16[(unsigned char)c - 32];
 volatile uint32_t *p = (volatile uint32_t *)FB_BASE;
 for (unsigned y = 0; y < 32; y++)
  for (unsigned x = 0; x < 16; x++)
   p[(uint64_t)(MARGIN + row * CH + y) * (FB_STRIDE / 4) + MARGIN + col * CW + x] =
    (glyph[y / 2] & (0x80u >> (x / 2))) ? 0xffffffffu : 0xff000000u;
 col++; dirty = 1;
}

void fbconsole_flush(void)
{
 if (dirty && display_lk_present()) dirty = 0;
}

void fbconsole_drain(void)
{
 /* Finish publishing the last partial/busy log line before giving up shim
  * ownership. A stuck display must not prevent kernel startup. */
 for (unsigned t = 0; t < 200 && dirty; t++) {
  fbconsole_flush();
  if (!dirty) break;
  wait_ms(1);
  wdt_kick();
 }
}
