// SPDX-License-Identifier: MIT
// x9tile=1: app open/close seam diagnostic, included by NativeBridgeMetalFrontend.mm.
//
// Historical photo estimate (2026-11-08): four see-through seams roughly
// 256 screen pixels apart during app transitions. This is not a measured
// texture-space pitch or proof of a 1024-byte memory/cache boundary.
// 20H392 QuartzCore's get_tiled_texture_size starts wide, tall images at
// 256x256 (then adjusts for short images); TiledImageGeometry binds them
// with TextureEdgeMode 0 = ClampToEdge. Neither proves this scene took that
// path. ClampToEdge does not repair transparent edge texels or missing quads.
// This logs creation and explicit CPU/blit uploads for textures whose width
// OR height is 250..270, plus distinct samplers. It does NOT observe render/
// compute writes, texture-view creation, draw bindings or geometry; there is
// a shared 600-line process budget and no scene trigger or exhaustion marker.
// Thus absence of uploads is not evidence excluding GPU-generated tiles,
// atlas subrects or draws after the logging budget was spent.
// Read-only: every call keeps its original behaviour.
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

static int x9tp_enabled = -1;
static int x9tp_lines;
static bool x9tp_on(void) {
 if (x9tp_enabled < 0) {
  char args[2048] = {0}; size_t bytes = sizeof(args) - 1; x9tp_enabled = 0;
  if (!sysctlbyname("kern.bootargs", args, &bytes, nullptr, 0)) {
   char *save = nullptr;
   for (char *t = strtok_r(args, " \t\r\n", &save); t; t = strtok_r(nullptr, " \t\r\n", &save))
    if (!strcmp(t, "x9tile=1")) x9tp_enabled = 1;
  }
 }
 return x9tp_enabled == 1;
}
static bool x9tp_width(uint64_t w) { return w >= 250 && w <= 270; }
static void x9tp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void x9tp_log(const char *fmt, ...) {
 if (x9tp_lines >= 600) return;
 x9tp_lines++;
 char line[480];
 va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
 int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
 if (fd >= 0) { dprintf(fd, "X9 TILEPROBE %s[%d]: %s\n", getprogname(), getpid(), line); close(fd); }
}
static void x9tp_texture(const char *how, const struct NmbTextureDescriptor *d, const void *handle,
                         uint64_t offset, uint64_t row) {
 if (!x9tp_on() || !d || !(x9tp_width(d->width) || x9tp_width(d->height))) return;
 x9tp_log("new %s tex=%p %llux%llu fmt=%llu type=%llu levels=%llu usage=0x%llx options=0x%llx offset=%llu row=%llu %s",
     how, handle, (unsigned long long)d->width, (unsigned long long)d->height, (unsigned long long)d->format,
     (unsigned long long)d->type, (unsigned long long)d->levels, (unsigned long long)d->usage,
     (unsigned long long)d->options, (unsigned long long)offset, (unsigned long long)row, handle ? "ok" : "REFUSED");
}
static void x9tp_upload(const char *how, const struct NmbTextureDescriptor *d, const void *handle,
                        uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint64_t row, uint64_t offset, bool ok) {
 if (!x9tp_on() || !d || !(x9tp_width(d->width) || x9tp_width(d->height))) return;
 x9tp_log("%s tex=%p (%llux%llu fmt=%llu) region=%llu,%llu %llux%llu row=%llu src_offset=%llu %s", how, handle,
     (unsigned long long)d->width, (unsigned long long)d->height, (unsigned long long)d->format,
     (unsigned long long)x, (unsigned long long)y, (unsigned long long)w, (unsigned long long)h,
     (unsigned long long)row, (unsigned long long)offset, ok ? "ok" : "REFUSED");
}
static void x9tp_sampler(uint64_t mn, uint64_t mg, uint64_t mip, uint64_t s, uint64_t t, uint64_t norm, bool ok) {
 if (!x9tp_on()) return;
 static uint64_t seen[48]; static unsigned count;
 const uint64_t key = mn | mg << 4 | mip << 8 | s << 12 | t << 16 | norm << 20 | (uint64_t)ok << 24;
 for (unsigned i = 0; i < count; i++) if (seen[i] == key) return;
 if (count < 48) seen[count++] = key;
 x9tp_log("sampler min=%llu mag=%llu mip=%llu s=%llu t=%llu normalized=%llu %s", (unsigned long long)mn,
     (unsigned long long)mg, (unsigned long long)mip, (unsigned long long)s, (unsigned long long)t,
     (unsigned long long)norm, ok ? "ok" : "REFUSED");
}
