#pragma once
#include <stdint.h>
#include <stddef.h>
struct sha256 {uint32_t h[8];uint64_t len;unsigned char buf[64];unsigned n;};
void sha256_init(struct sha256 *);
void sha256_update(struct sha256 *,const unsigned char *,size_t);
void sha256_final(struct sha256 *,unsigned char [32]);
