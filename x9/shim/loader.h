#pragma once
#include <stddef.h>
#include <stdint.h>

#define BOOT_LINE_LENGTH 608
#define PAYLOAD_VERSION 1

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

/* xnu-8796 pexpert/pexpert/arm64/boot.h, Revision/Version 2. */
struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

/* Pure validation: called on host fixtures as well as before any physical copy. */
const char *payload_validate(const void *data, size_t size, uint64_t *adt_pa,
			     uint64_t *args_pa);
const char *loader_boot(uintptr_t dtb, const void *embedded, size_t embedded_size);

const char *loader_boot_resident(uintptr_t dtb, const void *embedded, size_t size,
                                 uint64_t root_base, uint64_t root_size);
