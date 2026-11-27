'''Compile the actual sweet_x9_prepare_local_storage helper from kern_exec.c
with mocked PID1 syscalls and an in-memory fake filesystem.

Covers: container discovery by MCM metadata identifier (including a target
that only appears in a later getdirentries chunk), creation of
"File Provider Storage" and "Downloads" with mobile ownership, the
already-present case, missing container, the x9_local_storage=0 opt-out,
and a non-directory collision.
'''
import subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'x9/kernel/src/bsd/kern/kern_exec.c'

PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
typedef uint64_t user_addr_t;
typedef int64_t user_ssize_t;
typedef uint64_t user_size_t;
typedef void *proc_t;
typedef void *vnode_t;
typedef int boolean_t;
#define NULLVP ((vnode_t)0)
#define VNODE_LOOKUP_NOFOLLOW 1
struct open_args {user_addr_t path; int flags; int mode;};
struct close_args {int fd;};
struct read_args {int fd; user_addr_t cbuf; user_size_t nbyte;};
struct mkdir_args {user_addr_t path; int mode;};
struct fchown_args {int fd; int uid; int gid;};
struct getdirentries64_args {int fd; user_addr_t buf; user_size_t bufsize; user_addr_t position;};

static unsigned char page[16384];
#define SCRATCH 256

struct fentry { char path[256]; int isdir; const char *content; int uid; int gid; };
static struct fentry fs[256];
static int fs_count;
static void fs_add(const char *path, int isdir, const char *content) {
    assert(fs_count < 256);
    struct fentry *e = &fs[fs_count++];
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->isdir = isdir; e->content = content; e->uid = 0; e->gid = 0;
}
static struct fentry *fs_find(const char *path) {
    for (int i = 0; i < fs_count; i++) if (!strcmp(fs[i].path, path)) return &fs[i];
    return 0;
}
static const char *dir_names[256];
static int dir_count, dir_pos[64];
static int mkdir_calls, mkdir_modes[8], fchown_calls, fchown_uid, fchown_gid, open_calls, put_count;
static char mkdir_paths[8][256], fchown_paths[8][256];
struct fdent { struct fentry *e; int isdirfd; };
static struct fdent fds[64];

static int arg_present = 1, arg_value = 1, writable = 1;
int sweet_x9_arg(const char *name, int def) {
    assert(!strcmp(name, "x9_local_storage"));
    return arg_present ? arg_value : def;
}
boolean_t sweet_x9_ufs_root_writable(void) { return writable; }
static int proc_is64bit(proc_t p) { (void)p; return 1; }
static int copyout(const void *s, user_addr_t d, size_t n) {
    assert(d + n <= sizeof(page)); memcpy(page + d, s, n); return 0;
}
static int copyin(user_addr_t s, void *d, size_t n) {
    assert(s + n <= sizeof(page)); memcpy(d, page + s, n); return 0;
}
static void *vfs_context_kernel(void) { return 0; }
static int vnode_lookup(const char *path, int flags, vnode_t *vp, void *ctx) {
    (void)flags; (void)ctx;
    struct fentry *e = fs_find(path);
    if (!e) return ENOENT;
    *vp = (vnode_t)e; return 0;
}
static int vnode_isdir(vnode_t vp) { return ((struct fentry *)vp)->isdir; }
static void vnode_put(vnode_t vp) { (void)vp; put_count++; }

static int open(proc_t p, struct open_args *a, int32_t *rv) {
    (void)p; open_calls++;
    const char *path = (const char *)page + a->path;
    struct fentry *e = fs_find(path);
    if (!e) return ENOENT;
    for (int i = 3; i < 64; i++) if (!fds[i].e) {
        fds[i].e = e; fds[i].isdirfd = e->isdir && !strcmp(path, "/private/var/mobile/Containers/Shared/AppGroup");
        *rv = i; return 0;
    }
    return EMFILE;
}
static int sys_close(proc_t p, struct close_args *a, int32_t *rv) {
    (void)p; (void)rv;
    if (a->fd >= 3 && a->fd < 64) fds[a->fd].e = 0;
    return 0;
}
static int read(proc_t p, struct read_args *a, user_ssize_t *rv) {
    (void)p;
    struct fentry *e = fds[a->fd].e;
    if (!e || !e->content) { *rv = 0; return 0; }
    size_t len = strlen(e->content);
    if (len > a->nbyte) len = a->nbyte;
    memcpy(page + a->cbuf, e->content, len);
    *rv = (user_ssize_t)len; return 0;
}
static int mkdir(proc_t p, struct mkdir_args *a, int32_t *rv) {
    (void)p; (void)rv;
    const char *path = (const char *)page + a->path;
    assert(mkdir_calls < 8);
    snprintf(mkdir_paths[mkdir_calls], 256, "%s", path);
    mkdir_modes[mkdir_calls] = a->mode;
    mkdir_calls++;
    if (fs_find(path)) return EEXIST;
    fs_add(path, 1, 0);
    return 0;
}
static int fchown(proc_t p, struct fchown_args *a, int32_t *rv) {
    (void)p; (void)rv;
    struct fentry *e = fds[a->fd].e;
    assert(e && fchown_calls < 8);
    snprintf(fchown_paths[fchown_calls], 256, "%s", e->path);
    fchown_calls++; fchown_uid = a->uid; fchown_gid = a->gid;
    e->uid = a->uid; e->gid = a->gid;
    return 0;
}
static int getdirentries64(proc_t p, struct getdirentries64_args *a, user_ssize_t *rv) {
    (void)p;
    assert(fds[a->fd].isdirfd);
    int pos = dir_pos[a->fd];
    unsigned char *out = page + a->buf;
    size_t used = 0;
    while (pos < dir_count) {
        const char *name = dir_names[pos];
        uint16_t namlen = (uint16_t)strlen(name);
        uint16_t reclen = (uint16_t)((21 + namlen + 1 + 7) & ~7);
        if (used + reclen > a->bufsize) break;
        memset(out + used, 0, reclen);
        uint64_t ino = 1000 + pos, seek = 0;
        memcpy(out + used, &ino, 8); memcpy(out + used + 8, &seek, 8);
        memcpy(out + used + 16, &reclen, 2); memcpy(out + used + 18, &namlen, 2);
        out[used + 20] = 4;
        memcpy(out + used + 21, name, namlen);
        used += reclen; pos++;
    }
    dir_pos[a->fd] = pos;
    int64_t newpos = pos;
    memcpy(page + a->position, &newpos, 8);
    *rv = (user_ssize_t)used;
    return 0;
}
'''

BODY = r'''
static const char *GROUPS = "/private/var/mobile/Containers/Shared/AppGroup";
static const char *LS_UUID = "3023E235-31FD-45FF-BEDD-AFAFEE26F864";
static char pathbuf[512];

static void reset(void) {
    fs_count = 0; dir_count = 0; mkdir_calls = 0; fchown_calls = 0; open_calls = 0; put_count = 0;
    memset(fds, 0, sizeof(fds)); memset(dir_pos, 0, sizeof(dir_pos));
    arg_present = 1; arg_value = 1; writable = 1;
    fs_add(GROUPS, 1, 0);
}
static void add_container(const char *uuid, const char *metadata) {
    dir_names[dir_count++] = uuid;
    if (metadata) {
        snprintf(pathbuf, sizeof(pathbuf), "%s/%s/.com.apple.mobile_container_manager.metadata.plist", GROUPS, uuid);
        fs_add(strdup(pathbuf), 0, metadata);
    }
}
static void add_dir_entry(const char *name) { dir_names[dir_count++] = name; }

int main(void) {
    reset();
    add_dir_entry("."); add_dir_entry("..");
    add_container("0321D182-7A25-43E8-A7C1-103072C38D06", "xx group.com.apple.feedback yy");
    add_container(LS_UUID, "bplist group.com.apple.FileProvider.LocalStorage tail");
    add_container("0FCFA7BB-7626-40C3-8D75-4986752AB58F", 0);
    int r = sweet_x9_prepare_local_storage(0, SCRATCH, sizeof(page));
    assert(r == 0);
    assert(mkdir_calls == 2);
    snprintf(pathbuf, sizeof(pathbuf), "%s/%s/File Provider Storage", GROUPS, LS_UUID);
    assert(!strcmp(mkdir_paths[0], pathbuf) && mkdir_modes[0] == 0755);
    snprintf(pathbuf, sizeof(pathbuf), "%s/%s/File Provider Storage/Downloads", GROUPS, LS_UUID);
    assert(!strcmp(mkdir_paths[1], pathbuf));
    assert(fchown_calls == 2 && fchown_uid == 501 && fchown_gid == 501);
    assert(!strcmp(fchown_paths[0], mkdir_paths[0]));

    reset();
    add_container(LS_UUID, "group.com.apple.FileProvider.LocalStorage");
    snprintf(pathbuf, sizeof(pathbuf), "%s/%s/File Provider Storage", GROUPS, LS_UUID);
    fs_add(strdup(pathbuf), 1, 0);
    snprintf(pathbuf, sizeof(pathbuf), "%s/%s/File Provider Storage/Downloads", GROUPS, LS_UUID);
    fs_add(strdup(pathbuf), 1, 0);
    r = sweet_x9_prepare_local_storage(0, SCRATCH, sizeof(page));
    assert(r == 0 && mkdir_calls == 2 && fchown_calls == 2);

    reset();
    add_container("0321D182-7A25-43E8-A7C1-103072C38D06", "group.com.apple.feedback");
    r = sweet_x9_prepare_local_storage(0, SCRATCH, sizeof(page));
    assert(r == ENOENT && mkdir_calls == 0 && fchown_calls == 0);

    reset(); arg_value = 0;
    add_container(LS_UUID, "group.com.apple.FileProvider.LocalStorage");
    r = sweet_x9_prepare_local_storage(0, SCRATCH, sizeof(page));
    assert(r == 0 && open_calls == 0 && mkdir_calls == 0);
    reset(); arg_present = 0;
    add_container(LS_UUID, "group.com.apple.FileProvider.LocalStorage");
    r = sweet_x9_prepare_local_storage(0, SCRATCH, sizeof(page));
    assert(r == 0 && mkdir_calls == 2);

    reset(); writable = 0;
    add_container(LS_UUID, "group.com.apple.FileProvider.LocalStorage");
    r = sweet_x9_prepare_local_storage(0, SCRATCH, sizeof(page));
    assert(r == EROFS && open_calls == 0);

    reset();
    for (int i = 0; i < 40; i++) {
        char *u = malloc(64);
        snprintf(u, 64, "FILLER%04d-0000-0000-0000-000000000000", i);
        add_container(u, "group.com.apple.filler");
    }
    add_container(LS_UUID, "group.com.apple.FileProvider.LocalStorage");
    r = sweet_x9_prepare_local_storage(0, SCRATCH, sizeof(page));
    assert(r == 0 && mkdir_calls == 2);

    reset();
    add_container(LS_UUID, "group.com.apple.FileProvider.LocalStorage");
    snprintf(pathbuf, sizeof(pathbuf), "%s/%s/File Provider Storage", GROUPS, LS_UUID);
    fs_add(strdup(pathbuf), 0, "not a dir");
    r = sweet_x9_prepare_local_storage(0, SCRATCH, sizeof(page));
    assert(r == ENOTDIR);
    for (int i = 0; i < fchown_calls; i++) assert(strcmp(fchown_paths[i], pathbuf));

    printf("local-storage bootstrap: all scenarios passed\n");
    return 0;
}
'''


class LocalStorageBootstrapTests(unittest.TestCase):
    def test_helper_compiles_and_behaves(self):
        s = SOURCE.read_text()
        a = s.index('static int\nx9_local_storage_contains(')
        b = s.index('/* SystemOS is already present', a)
        helper = s[a:b]
        code = PRELUDE + helper + BODY
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / 't.c'
            src.write_text(code)
            exe = Path(td) / 't'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', str(src), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('all scenarios passed', out.stdout)


if __name__ == '__main__':
    unittest.main()
