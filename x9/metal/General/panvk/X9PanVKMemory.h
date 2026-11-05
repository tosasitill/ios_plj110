/* SPDX-License-Identifier: MIT */
#pragma once
#include "kmod/pan_kmod.h"
#include "../X9RenderABI.h"
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
/* Explicit console path: daemon stderr may not reach the persistent log. */
static inline __attribute__((format(printf, 1, 2))) void
x9_panvk_log(const char *format, ...) {
 int saved=errno;
 char line[768];va_list ap;va_start(ap,format);
 int n=vsnprintf(line,sizeof(line)-2,format,ap);va_end(ap);
 if(n>=0) {
  size_t count=(size_t)n<sizeof(line)-2?(size_t)n:sizeof(line)-3;
  line[count++]='\n';
  int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
  if(fd>=0){(void)write(fd,line,count);close(fd);}
 }
 errno=saved;
}
/* Explicit native constructor. Takes ownership of sync_ops only on success.
 * Not registered with DRM enumeration: the queue/timeline port must provide
 * its real sync provider before enabling this backend in device discovery. */
struct pan_kmod_dev *x9_pan_kmod_create(const struct pan_kmod_allocator *,struct util_sync_provider *);
/* Synchronous native queue entry. Callers must resolve dependency waits before
 * entry and signal their timeline only after a validated completion. */
int x9_pan_kmod_group_create(struct pan_kmod_dev *,const struct X9RenderGroup *,struct X9RenderGroup *);
int x9_pan_kmod_group_destroy(struct pan_kmod_dev *,uint32_t);
int x9_pan_kmod_submit(struct pan_kmod_dev *,uint32_t,const struct X9RenderStream *,uint32_t,struct X9RenderCompletion *);
/* Async submission (kernel X9RenderAsyncSubmit). */
bool x9_pan_kmod_async(struct pan_kmod_dev *);
uint64_t x9_pan_kmod_async_completed(struct pan_kmod_dev *);
uint64_t x9_pan_kmod_last_submission(struct pan_kmod_dev *,uint32_t *group);
int x9_pan_kmod_submit_async(struct pan_kmod_dev *,uint32_t,const struct X9RenderStream *,uint32_t,uint64_t *id);
int x9_pan_kmod_fence_wait(struct pan_kmod_dev *,uint32_t,uint64_t,uint64_t,struct X9RenderCompletion *);

int x9_pan_kmod_unmap_cpu(struct pan_kmod_bo *,void *);

int x9_pan_kmod_caps(struct pan_kmod_dev *,struct X9RenderCaps *);

const struct drm_panthor_csif_info *x9_pan_kmod_csif(struct pan_kmod_dev *);

int x9_pan_kmod_group_check(struct pan_kmod_dev *,uint32_t);

int x9_pan_kmod_sync_cpu(struct pan_kmod_bo *,uint64_t,uint64_t,enum pan_kmod_bo_sync_type);

struct pan_kmod_bo *x9_pan_kmod_import_host(struct pan_kmod_dev *,struct pan_kmod_vm *,void *,uint64_t);
