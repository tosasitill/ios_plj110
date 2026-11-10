/* SWEETXNU v1, shared with x676c/tools/mkpayload.py; no EFI/XNUP container. */
#include "loader.h"
#include "fdt.h"
#include "hw.h"
#include "plog.h"
#include "display_lk.h"
#include "fbconsole.h"
#include "x9_ram_ranges.h"
#if X9_USB_RECEIVER || X9_UFS_ROOT
#include "resident.h"
#endif

extern char _head[], _image_end[];

#if !X9_DIAG_ONLY
static void copy(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;
	volatile const uint8_t *s = (volatile const uint8_t *)src;
	/* Device memory with MMU off: keep accesses aligned and never over-read. */
	while (n && ((dst | src) & 7)) {
		*d++ = *s++;
		dst++;
		src++;
		n--;
	}
	while (n >= 8) {
		*(volatile uint64_t *)d = *(volatile const uint64_t *)s;
		d += 8;
		s += 8;
		n -= 8;
		if (!((uintptr_t)d & 0xffff))
			wdt_kick();
	}
	while (n--)
		*d++ = *s++;
}

static void zero(uintptr_t dst, uint64_t n)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;
	while (n && ((uintptr_t)d & 7)) {
		*d++ = 0;
		n--;
	}
	while (n >= 8) {
		*(volatile uint64_t *)d = 0;
		d += 8;
		n -= 8;
		if (!((uintptr_t)d & 0xffff))
			wdt_kick();
	}
	while (n--)
		*d++ = 0;
}
#endif /* !X9_DIAG_ONLY */

static int overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{
	return a < b + m && b < a + n;
}

static const char *boot_impl(uintptr_t dtb, const void *embedded, size_t embedded_size,
                             uint64_t root_base, uint64_t root_size)
{
	const void *data = embedded;
	size_t size = embedded_size;
	uint64_t rd_start = 0, rd_end = 0;

	/* Default: container embedded in Image, independent of vendor initrd layout. */
	if (!size) {
		if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
		    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end) ||
		    rd_end <= rd_start || rd_end - rd_start > 0x4000000)
			return "no embedded payload or bounded initrd";
		data = (const void *)(uintptr_t)rd_start;
		size = rd_end - rd_start;
	}
	uint64_t adt_pa, ba_pa;
	const char *why = payload_validate(data, size, &adt_pa, &ba_pa);
	if (why)
		return why;
	const struct payload *p = data;
 if(p->phys_base==X9_RAM_LO && p->mem_size==X9_RAM_SPAN) {
  if(!fdt_ram_build_live(dtb)) {
   uint64_t lo,n,a,z;
   plog_str("X9 RAM rejected: ");plog_str(fdt_ram_failure(&lo,&n,&a,&z));plog_str("\n");
   plog_hex("RAM query base",lo);plog_hex("RAM query size",n);
   plog_hex("RAM conflict or gap base",a);plog_hex("RAM conflict or gap size",z);
   for(unsigned i=0;fdt_ram_memory_tuple(i,&a,&z);i++) {
    plog_hex("FDT memory tuple index",i);plog_hex("FDT memory base",a);plog_hex("FDT memory size",z);
   }
   return "live FDT rejects segmented RAM ranges";
  }
  /* Kernel + ADT + boot args must stay in the first contiguous extent. */
  uint64_t first_lo,first_end;
  if(!fdt_ram_live_tuple(0,&first_lo,&first_end) || ba_pa+PAGE_16K>first_end)
   return "bootstrap crosses first RAM extent";
  plog_str("X9 RAM: live reservations subtracted; map verified\n");
  plog_hex("X9 RAM usable bytes",fdt_ram_live_bytes());
  for(unsigned i=0;;i++) {
   uint64_t a,z;if(!fdt_ram_live_tuple(i,&a,&z))break;
   plog_hex("X9 RAM extent base",a);plog_hex("X9 RAM extent end",z);
  }
 }
#if X9_USB_RECEIVER || X9_UFS_ROOT
    if (root_size) {
        uint64_t expected,capacity;
#if X9_UFS_ROOT
        why=resident_layout_ufs(data,size,&expected,&capacity);
#else
        why=resident_layout(data,size,&expected,&capacity);
#endif
        if (why) return why;
        if (root_base!=expected || root_size>capacity)
            return "resident root outside validated RAM";
        uint64_t rounded=(root_size+PAGE_16K-1)&~(PAGE_16K-1);
        adt_pa=root_base+rounded;
        ba_pa=(adt_pa+p->adt_size+PAGE_16K-1)&~(PAGE_16K-1);
    }
#else
    if (root_base || root_size) return "resident root not enabled";
#endif
	uint64_t top = ba_pa + PAGE_16K;
	if (overlap(p->phys_base, top - p->phys_base, (uintptr_t)data, size) ||
	    overlap(p->phys_base, top - p->phys_base, (uintptr_t)_head,
		    (size_t)(_image_end - _head)) ||
	    (rd_end && overlap(p->phys_base, top - p->phys_base,
			       rd_start, rd_end - rd_start)))
		return "payload or shim overlaps kernel destination";

	/* /arm-io timer properties must match the hardware value at this boot. */
	plog_hex("payload", (uintptr_t)data);
	plog_hex("phys base", p->phys_base);
	plog_hex("mem size", p->mem_size);
	plog_hex("entry va", p->entry_va);
#if X9_DIAG_ONLY
	/* Stop before the first store. A hang here proves the shim ran and the
	 * payload validated; a screen change would then not be ours. */
	plog_str("diag: validated, no copy, no jump\n");
	plog_flush();
	return "diagnostic stop before kernel copy";
#else
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = p->phys_base + (s->vmaddr - p->virt_base);
		copy(dst, (uintptr_t)data + s->file_off, s->file_size);
		zero(dst + s->file_size, s->vmsize - s->file_size);
	}
	copy(adt_pa, (uintptr_t)data + p->adt_off, p->adt_size);
 if(fdt_gpu_patch_adt(dtb,(void *)(uintptr_t)adt_pa,p->adt_size))
  return "GPU ADT handoff placeholder invalid";
 if(fdt_scp_patch_adt(dtb,(void *)(uintptr_t)adt_pa,p->adt_size)<0)
  plog_str("X9 SCP FDT: missing/invalid ADT placeholder; sensors remain pending\n");
 { int wd=fdt_wifi_dump_adt(dtb,(void *)(uintptr_t)adt_pa,p->adt_size);   /* never fatal */
   plog_str(wd==0?"X9 LK DT: Wi-Fi facts copied to ADT\n":wd>0?"X9 LK DT: no ADT placeholder\n":"X9 LK DT: FDT walk refused\n"); }
 if(p->phys_base==X9_RAM_LO && p->mem_size==X9_RAM_SPAN &&
    fdt_ram_patch_adt((void *)(uintptr_t)adt_pa,p->adt_size))
  return "live RAM map ADT handoff invalid";
#if X9_USB_RECEIVER || X9_UFS_ROOT
    if (root_size) {
        uint64_t rounded=(root_size+PAGE_16K-1)&~(PAGE_16K-1);
        zero(root_base+root_size,rounded-root_size);
        if (resident_patch_adt((void *)(uintptr_t)adt_pa,p->adt_size,root_base,root_size))
            return "resident RAMDisk ADT placeholder invalid";
    }
#endif
	zero(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
 ba->memSizeActual = p->mem_size==X9_RAM_SPAN ? fdt_ram_live_bytes() : p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = p->virt_base + (adt_pa - p->phys_base);
	ba->deviceTreeLength = p->adt_size;
	for (unsigned i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];

	uint64_t entry = p->phys_base + (p->entry_va - p->virt_base);
	plog_hex("boot_args", ba_pa);
	plog_hex("jump", entry);
	plog_str("XNU handoff: shim console stops here\n");
	plog_stage("X9: entering XNU\n");
	plog_flush();
	fbconsole_drain();
	display_lk_stop();
	plog_detach_console();
	wdt_kick();
	extern void x9_shim_usb_stop(void);
	x9_shim_usb_stop();
	xnu_enter(entry, ba_pa);
#endif
}

const char *loader_boot(uintptr_t dtb, const void *embedded, size_t size)
{
    return boot_impl(dtb,embedded,size,0,0);
}
#if X9_USB_RECEIVER || X9_UFS_ROOT
const char *loader_boot_resident(uintptr_t dtb, const void *embedded, size_t size,
                                 uint64_t base, uint64_t length)
{
    if (!length) return "empty resident root";
    return boot_impl(dtb,embedded,size,base,length);
}
#endif
