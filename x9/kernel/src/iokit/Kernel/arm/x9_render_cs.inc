/* Generated normal build input: frozen Mesa v14 CS XML. */
#pragma once
#include <stdint.h>
static inline uint64_t x9_cs_move48(uint64_t reg,uint64_t value) { return ((uint64_t)(value)<<0)|((uint64_t)(reg)<<48)|0x100000000000000ULL; }
static inline uint64_t x9_cs_move32(uint64_t reg,uint64_t value) { return ((uint64_t)(value)<<0)|((uint64_t)(reg)<<48)|0x200000000000000ULL; }
static inline uint64_t x9_cs_call(uint64_t address,uint64_t length) { return ((uint64_t)(length)<<32)|((uint64_t)(address)<<40)|0x2000000000000000ULL; }
static inline uint64_t x9_cs_wait(uint64_t mask) { return ((uint64_t)(mask)<<16)|0x300000000000000ULL; }
static inline uint64_t x9_cs_flush_cache2(uint64_t reg) { return 0x3ULL|0x30ULL|0x200ULL|((uint64_t)(reg)<<40)|0x2400000000000000ULL; }
static inline uint64_t x9_cs_sync_set64(uint64_t address,uint64_t data) { return ((uint64_t)(data)<<32)|((uint64_t)(address)<<40)|0x3400000000000000ULL; }
