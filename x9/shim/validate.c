#include "loader.h"
#include "hw.h"

_Static_assert(offsetof(struct payload, segs) == 0x2a8, "payload layout");
_Static_assert(offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

static int within(uint64_t start, uint64_t size, uint64_t limit)
{
	return start <= limit && size <= limit - start;
}

const char *payload_validate(const void *data, size_t size, uint64_t *adt_pa,
			     uint64_t *args_pa)
{
	if (!data || !adt_pa || !args_pa || ((uintptr_t)data & 7) ||
	    size < sizeof(struct payload))
		return "unaligned or truncated payload";
	const struct payload *p = data;
	const char magic[] = "SWEETXNU";
	for (unsigned i = 0; i < 8; i++)
		if (p->magic[i] != magic[i])
			return "bad payload magic";
	if (p->version != PAYLOAD_VERSION || !p->nsegs || p->nsegs > 128)
		return "bad payload version or segment count";
	uint64_t table_end = sizeof(*p) + p->nsegs * sizeof(struct payload_seg);
	if (table_end > size)
		return "truncated segment table";
	if (!((p->phys_base == KERNEL_RAM_LO &&
	       p->mem_size <= KERNEL_RAM_HI - KERNEL_RAM_LO) ||
	      (p->phys_base == KERNEL_LARGE_RAM_LO &&
	       p->mem_size <= KERNEL_LARGE_RAM_HI - KERNEL_LARGE_RAM_LO)) ||
	    !p->mem_size ||
	    ((p->phys_base | p->mem_size) & (PAGE_16K - 1)) ||
	    (p->virt_base & (0x2000000UL - 1)))
		return "kernel RAM outside X9's contiguous range";
	if (p->adt_off < table_end || (p->adt_off & 7) || !p->adt_size ||
	    p->adt_size > UINT32_MAX || !within(p->adt_off, p->adt_size, size))
		return "ADT outside payload";
	if (p->virt_base > UINT64_MAX - p->mem_size ||
	    p->kernel_end < p->virt_base ||
	    (p->kernel_end & (PAGE_16K - 1)))
		return "bad kernel end";
	uint64_t end = p->kernel_end - p->virt_base;
	if (!within(end, p->adt_size, p->mem_size))
		return "ADT outside kernel RAM";
	uint64_t ba = (end + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	if (!within(ba, PAGE_16K, p->mem_size))
		return "boot_args outside kernel RAM";
	if (p->cmdline[BOOT_LINE_LENGTH - 1] != 0)
		return "unterminated boot arguments";

	int entry_found = 0;
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		if (!s->vmsize || s->vmaddr < p->virt_base ||
		    (s->vmaddr & (PAGE_16K - 1)) || s->file_size > s->vmsize)
			return "bad segment address or size";
		uint64_t off = s->vmaddr - p->virt_base;
		if (!within(off, s->vmsize, end))
			return "segment outside kernel image";
		if ((s->file_off & 7) || s->file_off < table_end ||
		    !within(s->file_off, s->file_size, p->adt_off))
			return "segment data outside payload";
		for (uint32_t j = 0; j < i; j++) {
			const struct payload_seg *t = &p->segs[j];
			if (s->vmaddr < t->vmaddr + t->vmsize &&
			    t->vmaddr < s->vmaddr + s->vmsize)
				return "overlapping kernel segments";
		}
		if (p->entry_va >= s->vmaddr &&
		    p->entry_va - s->vmaddr < s->file_size)
			entry_found = 1;
	}
	if (!entry_found || (p->entry_va & 3))
		return "entry outside loaded segment data";
	*adt_pa = p->phys_base + end;
	*args_pa = p->phys_base + ba;
	return 0;
}
