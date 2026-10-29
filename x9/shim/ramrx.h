#pragma once
#include <stddef.h>
#include <stdint.h>

#define RAMRX_BLOCK 32768U
#define RAMRX_FRAME_MAX (RAMRX_BLOCK + 24U)
#define RAMRX_BEGIN 0x40
#define RAMRX_DATA 0x41
#define RAMRX_COMMIT 0x42
#define RAMRX_BOOT 0x43
#define RAMRX_ABORT 0x44
#define RAMRX_STATUS 0x45

struct ramrx {
    uint8_t *destination;
    uint64_t capacity, total, received;
    uint32_t expected_crc, crc, state, error;
    uint8_t scratch[RAMRX_BLOCK];
};
void ramrx_init(struct ramrx *, void *, uint64_t);
int ramrx_command(struct ramrx *, unsigned, const uint8_t *, size_t);
void ramrx_status(const struct ramrx *, uint8_t out[48]);
uint32_t ramrx_crc32(uint32_t, const uint8_t *, size_t);
int ramrx_lz4(const uint8_t *, size_t, uint8_t *, size_t);
