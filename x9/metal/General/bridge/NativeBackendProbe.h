// SPDX-License-Identifier: MIT
/* Backend discovery boundary for the Metal runtime.
 *
 * The Metal API layer (factory and frontend) asks these questions and never
 * includes IOKit, the native render transport, or a Vulkan loader path. The
 * bridge side answers them: it is the only layer that knows how the native
 * service is probed and which loader this build installs.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __OBJC__
#import <Foundation/Foundation.h>

struct NmbBackendProbe {
 uint64_t shader_present;
};

extern "C" {
/* True only when the native render service exists, advertises ABI v1 and
 * ready, a probe connection opens, and it reports at least one shader core.
 * The probe connection is closed before returning; no device is created. */
bool nmb_backend_probe(struct NmbBackendProbe *out);
/* The loader path this build installs; for the discovery log only. */
const char *nmb_backend_loader_path(void);
/* Open the process's one device through that loader. Resolves the accelerator
 * port's registry entry, as the runtime's device identity requires. */
id nmb_backend_open(const char *cache, unsigned accelerator_port,
                    void (*missing)(const char *, const char *));
/* Cache scope: X9ScopeRegister/Unregister/Acquire/Release/HoldAll/ReleaseAll
 * (X9RenderABI.h values 1..6). False when the kernel has no scope client. */
bool nmb_cache_scope(uint32_t operation, uint64_t address, uint64_t bytes);
}
#endif
