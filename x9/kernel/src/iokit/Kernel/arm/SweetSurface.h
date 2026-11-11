/*
 * Built-in IOSurface for SWEET (see SweetSurface.cpp).
 *
 * Shared with SweetFramebuffer.cpp: the framebuffer looks surfaces up by
 * id when a swap names them and publishes the panel memory itself as the
 * default framebuffer surface.
 */
#ifndef _SWEET_SURFACE_H
#define _SWEET_SURFACE_H

#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include <mach/mach_types.h>

#define SWEET_SURFACE_MAX_PLANES        20

struct sweet_surface_plane {
	uint32_t width, height, bytesPerRow, offset, size;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
};

class SweetSurface : public OSObject {
	OSDeclareDefaultStructors(SweetSurface);

public:
	uint32_t id;
	uint32_t width, height, bytesPerRow, offset, pixelFormat, allocSize;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
	uint32_t cacheMode, planeCount, parentID;
	struct sweet_surface_plane planes[SWEET_SURFACE_MAX_PLANES];

	IOMemoryDescriptor       *memory;       /* pixels (buffer or physical) */
	IOOptionBits              mapCache;     /* cache mode for user mappings */
	IOBufferMemoryDescriptor *state;        /* shared state page, read-only for users */
	IOMemoryMap              *kernelMap;    /* lazily created for kernel copies */
	uint64_t                  scanoutPhys;  /* topaz: physically contiguous scanout slot, or 0 */
	int                       scanoutSlot;  /* pool slot, -1 when not from the pool */
	OSDictionary             *values;       /* IOSurfaceSetValue & co. */
	uint32_t                  purgeable;    /* IOSurfaceSetPurgeable state */
	uint32_t                  wrapDelta;    /* wrapped client memory: where it starts in its first page */
	bool                      wrapped;      /* memory is a prepared range of a task (IOSurfaceWrapClient*) */
	/* x9lc GPU coherency by IOSurface lock protocol (see SweetSurface.cpp). */
	uint32_t                  gpuCleanSeed; /* surface seed at the last GPU-side CPU clean */
	bool                      gpuCleanValid;
	bool                      gpuDirty;     /* GPU may have written since the last CPU acquire */

	bool init() APPLE_KEXT_OVERRIDE { scanoutPhys = 0; scanoutSlot = -1; values = NULL; purgeable = 0; wrapDelta = 0; wrapped = false;
		gpuCleanSeed = 0; gpuCleanValid = false; gpuDirty = false; return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;

	/* Kernel address of the pixels (mapped on first use), or 0. */
	uintptr_t kernelAddress();
};

/* Retained surface for an id, or NULL. */
SweetSurface *sweet_surface_copy(uint32_t id);

/*
 * x9lc: CPU cache maintenance of GPU-imported IOSurfaces by the IOSurface lock
 * protocol instead of on every GPU job. sweet_surface_mapping_id: the surface
 * whose entire (non-wrapped, cacheable) pixel mapping in task equals [addr, addr+bytes),
 * or 0. gpu_needs_clean: whether the CPU may have written since the last GPU-side
 * clean (seed changed on a write unlock, or never cleaned); the seed to record.
 * gpu_wrote: the GPU may have written; the next CPU acquire (IOSurfaceLock or a
 * kernel read such as the framebuffer present copy) cleans+invalidates first.
 */
uint32_t sweet_surface_mapping_id(task_t task, uint64_t addr, uint64_t bytes);
bool sweet_surface_gpu_needs_clean(uint32_t id, uint32_t *seed);
void sweet_surface_gpu_cleaned(uint32_t id, uint32_t seed);
void sweet_surface_gpu_wrote(uint32_t id);
void sweet_surface_cpu_acquire(SweetSurface *s);

/*
 * Publish physical memory (the panel framebuffer) as a surface. Returns a
 * retained surface that stays in the id table until released by the caller
 * and every user.
 */
SweetSurface *sweet_surface_create_physical(uint64_t phys, uint32_t width,
    uint32_t height, uint32_t bytesPerRow, uint32_t pixelFormat);

/*
 * topaz scanout pool: physically contiguous low memory (below 4 GiB, unused by
 * xnu, which runs in the upper DDR bank) so the MDP can fetch a surface
 * directly. The framebuffer sets the full-screen geometry it can scan out.
 */
void sweet_scanout_set_geometry(uint32_t w1, uint32_t h1, uint32_t w2, uint32_t h2);

/* Send right for the surface in task (IOSurfaceLookupFromMachPort). */
mach_port_name_t sweet_surface_port_in_task(SweetSurface *surface, task_t task);

/* Owned RAM surface for the opt-in X9 render target; never a panel alias. */
SweetSurface *sweet_surface_create_buffer(uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat);

#endif /* SWEET */
#endif /* _SWEET_SURFACE_H */
