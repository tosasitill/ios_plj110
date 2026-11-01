/* Captured PLJ110 iomem + fdt.dtb, 16 KiB pages. End is exclusive.
 * Only this explicit 8+ GiB mode may use these discontiguous ranges. */
#define X9_RAM_LO 0x140000000ULL
#define X9_RAM_HI 0x36F000000ULL
#define X9_RAM_SPAN (X9_RAM_HI-X9_RAM_LO)
#define X9_RAM_USABLE 0x222140000ULL
static const unsigned long long x9_ram_ranges[][2] = {
 {0x140000000ULL,0x160000000ULL},
 {0x160DA0000ULL,0x1E03F0000ULL},
 {0x1E3270000ULL,0x1E33F0000ULL},
 {0x1E3540000ULL,0x1E43F0000ULL},
 {0x1EC150000ULL,0x1EC1F0000ULL},
 {0x1EC9D0000ULL,0x1FD9F0000ULL},
 {0x1FE3F0000ULL,0x1FFFF0000ULL},
 {0x200000000ULL,0x33FE00000ULL},
 {0x340000000ULL,0x36F000000ULL},
};
#define X9_RAM_NR (sizeof(x9_ram_ranges)/sizeof(x9_ram_ranges[0]))
