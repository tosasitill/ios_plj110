#pragma once
#include <string.h>
/* Compute is available to all clients except the system processes whose
 * required rendering path this compute-only backend does not implement. */
static inline int x9_metal_render_owner(const char *name) {
 return name&&(!strcmp(name,"backboardd")||!strcmp(name,"SpringBoard")||
  !strcmp(name,"chronod")||!strcmp(name,"mediaserverd")||!strcmp(name,"renderserverd")||!strcmp(name,"RenderServer"));
}

/* Shared userspace token contract:1=compute,2=compute+private offscreen copy.
 * Exact tokens only; absent,0,other values and prefixes remain disabled. */
static inline unsigned x9_metal_mode_from_token(const char *word) {
 if(!word)return 0;
 if(!strcmp(word,"x9_metal_system=1"))return 1;
 if(!strcmp(word,"x9_metal_system=2"))return 2;
 return 0;
}
