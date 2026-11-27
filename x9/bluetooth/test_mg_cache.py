#!/usr/bin/env python3
"""Exercise the PID 1 cache refresh against a host-only file model."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MobileGestaltCache(unittest.TestCase):
    def test_epoch_transitions_and_failures(self):
        source = (ROOT / 'x9/kernel/src/bsd/kern/kern_exec.c').read_text()
        start = source.index('static int\nsweet_x9_mg_unlink(')
        end = source.index('\n}', source.index('sweet_x9_wipe_gestalt_cache(', start)) + 2
        harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

typedef int proc_t;
typedef bool boolean_t;
#ifndef __APPLE__
typedef uintptr_t user_addr_t;
#endif
typedef uint8_t SHA256_CTX[32];
struct unlink_args { user_addr_t path; };

static bool island = true, bluetooth = true, als = true, camera = true;
static bool writable = true, epoch_exists, fail_legacy;
static int mode = -1, refreshes, publications, rollbacks;
static uint8_t epoch_digest[32];

static int sweet_x9_arg(const char *name, int fallback) {
    if (!strcmp(name, "x9_ui")) return island;
    if (!strcmp(name, "x9_bt_init")) return bluetooth;
    if (!strcmp(name, "x9_als")) return als ? 3 : 0;
    if (!strcmp(name, "x9_camera")) return camera ? 2 : 1;
    assert(!strcmp(name, "x9_mgwipe"));
    return mode < 0 ? fallback : mode;
}

static boolean_t sweet_x9_ufs_root_writable(void) { return writable; }
static bool proc_is64bit(proc_t proc) { return proc == 1; }
static int copyout(const void *data, user_addr_t addr, size_t size) {
    memcpy((void *)addr, data, size);
    return 0;
}

static int unlink(proc_t proc, struct unlink_args *args, int32_t *result) {
    const char *path = (const char *)args->path;
    assert(proc == 1 && result != NULL);
    if (strstr(path, "refresh-ui2556-")) {
        rollbacks++;
    } else if (strstr(path, ".x9-bbfw-temp")) {
        return ENOENT;
    } else if (strstr(path, "refresh-capabilities-")) {
        epoch_exists = false;
    } else {
        assert(strstr(path, "com.apple.MobileGestalt.plist") && !epoch_exists);
        if (strstr(path, "/var/mobile/")) {
            if (fail_legacy) return EIO;
            refreshes++;
        }
    }
    return 0;
}

static void SHA256_Init(SHA256_CTX *hash) { memset(*hash, 0, 32); }
static void SHA256_Update(SHA256_CTX *hash, const char *data, size_t size) {
    for (size_t index = 0; index < size; index++) (*hash)[index % 32] ^= data[index];
}
static void SHA256_Final(uint8_t digest[32], SHA256_CTX *hash) { memcpy(digest, *hash, 32); }

static int x9_bbfw_equal(const char *path, const uint8_t digest[32], uint32_t size) {
    assert(strstr(path, "/private/var/x9/mg/refresh-capabilities-") == path && size);
    if (!epoch_exists) return ENOENT;
    return memcmp(epoch_digest, digest, 32) ? EEXIST : 0;
}

static int x9_bbfw_directory(proc_t proc, user_addr_t scratch, user_addr_t end, const char *path) {
    assert(proc == 1 && scratch < end && !strcmp(path, "/private/var/x9/mg"));
    return 0;
}

static int x9_bbfw_write(proc_t proc, user_addr_t scratch, user_addr_t end, const char *path,
    const uint8_t *data, uint32_t size, const uint8_t digest[32], int *created) {
    assert(proc == 1 && scratch < end && data && refreshes > publications);
    assert(x9_bbfw_equal(path, digest, size) == ENOENT);
    memcpy(epoch_digest, digest, 32);
    epoch_exists = true;
    publications++;
    *created = 1;
    return 0;
}
''' + source[start:end] + r'''

static void boot(int result, int expected_refreshes, bool expected_epoch) {
    char scratch[256];
    assert(sweet_x9_wipe_gestalt_cache(1, (user_addr_t)scratch,
        (user_addr_t)(scratch + sizeof(scratch))) == result);
    assert(refreshes == expected_refreshes && epoch_exists == expected_epoch);
}

int main(void) {
    boot(0, 1, true);
    boot(0, 1, true);
    bluetooth = false; boot(0, 2, true);
    bluetooth = true; boot(0, 3, true);
    island = bluetooth = false; boot(0, 4, true);
    island = bluetooth = true; boot(0, 5, true);
    als = false; boot(0, 6, true);
    camera = false; boot(0, 7, true);
    mode = 3; boot(EINVAL, 7, true);
    camera = true; boot(0, 8, true);
    boot(0, 8, true);
    mode = 2; boot(0, 9, true);
    mode = 0; island = false; boot(0, 9, true);
    mode = 1; boot(0, 10, false);
    assert(rollbacks == 1);
    mode = 2; writable = false; boot(EROFS, 10, false);
    writable = true; boot(0, 11, true);
    bluetooth = false; fail_legacy = true; boot(EIO, 11, false);
    fail_legacy = false; boot(0, 12, true);
    assert(publications == 11);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            fixture = pathlib.Path(directory) / 'test.c'
            fixture.write_text(harness)
            executable = fixture.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(fixture), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
