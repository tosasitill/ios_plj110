/* SPDX-License-Identifier: MIT */
#pragma once
#include "X9RenderKernel.h"
/* AArch64 4-KiB GPU tables backed by prepared 16-KiB XNU allocations.
 * The engine must detach the AS and confirm cache/TLB completion before edits
 * or destruction. Binding and detaching hardware remain engine operations. */
struct X9RenderPageTable;
X9RenderPageTable *x9_render_vm_create();
IOReturn x9_render_vm_map(X9RenderPageTable *,const X9RenderVMEntry *);
IOReturn x9_render_vm_unmap(X9RenderPageTable *,const X9RenderVMEntry *);
uint64_t x9_render_vm_publish(X9RenderPageTable *);
IOReturn x9_render_vm_map_live(X9RenderPageTable *,const X9RenderVMEntry *);
IOReturn x9_render_vm_unmap_live(X9RenderPageTable *,const X9RenderVMEntry *);
uint64_t x9_render_vm_publish_live(X9RenderPageTable *);
void x9_render_vm_mark_detached(X9RenderPageTable *);
bool x9_render_vm_destroy(X9RenderPageTable *);

/* Kernel-owned ring/body/fence mappings below the user VA window. Never
 * reachable through user-client Map: resource.handle must be0 and executable
 * mappings are forbidden. Owner retains descriptor until confirmed detach. */
IOReturn x9_render_vm_map_kernel(X9RenderPageTable *,const X9RenderVMEntry *);
IOReturn x9_render_vm_unmap_kernel(X9RenderPageTable *,const X9RenderVMEntry *);
