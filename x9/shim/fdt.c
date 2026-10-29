#include "fdt.h"

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

static uint32_t be32(uintptr_t p)
{
	volatile const uint8_t *b = (volatile const uint8_t *)p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
	       (uint32_t)b[2] << 8 | b[3];
}

static int range(uint32_t off, uint32_t len, uint32_t size)
{
	return off <= size && len <= size - off;
}

static int name_eq(uintptr_t p, uintptr_t end, const char *want)
{
	while (p < end && *(volatile const char *)p == *want) {
		if (!*want)
			return 1;
		p++;
		want++;
	}
	return 0;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || (fdt & 3) || be32(fdt) != FDT_MAGIC)
		return 0;
	uint32_t total = be32(fdt + 4);
	uint32_t off = be32(fdt + 8), strings = be32(fdt + 12);
	uint32_t str_size = be32(fdt + 32), struct_size = be32(fdt + 36);
	if (total < 40 || total > 0x400000 || be32(fdt + 20) < 17 ||
	    (off & 3) || !range(off, struct_size, total) ||
	    !range(strings, str_size, total) || fdt > UINTPTR_MAX - total)
		return 0;
	uintptr_t p = fdt + off, end = p + struct_size;
	uintptr_t str = fdt + strings, str_end = str + str_size;
	unsigned depth = 0, chosen = 0;
	while (end - p >= 4) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			if (++depth == 2 && name_eq(p, end, "chosen"))
				chosen = depth;
			while (p < end && *(volatile const char *)p)
				p++;
			if (p == end)
				return 0;
			p = (p + 4) & ~(uintptr_t)3;
			break;
		case FDT_END_NODE:
			if (!depth || depth == chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			if (end - p < 8)
				return 0;
			uint32_t len = be32(p), name = be32(p + 4);
			p += 8;
			if (len > end - p || name >= str_size)
				return 0;
			if (depth == chosen && chosen && name_eq(str + name, str_end, prop)) {
				if (len == 4)
					*out = be32(p);
				else if (len == 8)
					*out = (uint64_t)be32(p) << 32 | be32(p + 4);
				else
					return 0;
				return 1;
			}
			p = (p + len + 3) & ~(uintptr_t)3;
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
		if (p > end)
			return 0;
	}
	return 0;
}
