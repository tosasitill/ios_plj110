/*
 * Private, fixed-size, single-file macFUSE adapter for native HFS staging.
 * This helper never opens a device or an image backing file. Every image
 * read/write and sync is delegated to the parent Python Unix-socket server,
 * which owns live reads, alignment, budgets, durable COW and the write ledger.
 * No HTTP backend or Apple shadow-format interpretation is involved.
 *
 * RPC v1: all integers little endian, one connection per request.
 *   request (24 bytes): <IHHQII>
 *     magic=0x58423952, version=1, op, offset, length, reserved=0
 *     READ=1; WRITE=2 (followed by length bytes); FSYNC=3 (offset/length=0).
 *   response (16 bytes): <IIII>
 *     same magic, positive errno or 0, length, reserved=0.
 *     READ success: length MUST equal requested length, followed by data.
 *     WRITE success: length MUST equal requested length, no response data.
 *     FSYNC success: length=0, no data. Any error: length=0, no data.
 *   Every READ/WRITE is <= 1 MiB. Offsets need not be 4 KiB aligned.
 *   EOF reads are trimmed here; writes past fixed EOF are rejected here.
 *
 * Build (installed macFUSE 5.3.2 / libfuse 2.9.9 headers):
 *   clang -std=c11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 \
 *     -isysroot <Xcode>/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk \
 *     -I/usr/local/include/fuse x9/bluetooth/range_fs.c \
 *     -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lfuse -pthread -o <output>
 * Run only during the parent's explicitly authorized staging session:
 *   <output> <canonical-private-unix-socket> <size-bytes> <private-mountpoint>
 * The exported image is <mountpoint>/root.hfs. The socket parent directory
 * must be private (no group/other permissions); macOS sun_path is short, so
 * use a short canonical /private/tmp/<private-directory>/rpc.sock path.
 * FUSE is foreground, single-threaded, with direct I/O and no readahead.
 * Installation of macFUSE is not proof that mounting is presently permitted.
 */

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define RPC_MAGIC UINT32_C(0x58423952)
#define RPC_READ 1u
#define RPC_WRITE 2u
#define RPC_FSYNC 3u
#define MAX_IO (1024u * 1024u)

static uint64_t image_size;
static struct sockaddr_un endpoint;
static dev_t endpoint_device;
static ino_t endpoint_inode;
static uid_t endpoint_owner;
static uid_t owner;
static gid_t group;
static int failed;

static void put16(unsigned char *p, uint16_t n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}

static void put32(unsigned char *p, uint32_t n)
{
    for (unsigned i = 0; i != 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}

static void put64(unsigned char *p, uint64_t n)
{
    for (unsigned i = 0; i != 8; ++i) p[i] = (unsigned char)(n >> (8 * i));
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int transfer(int fd, void *buffer, size_t count, int sending)
{
    unsigned char *p = buffer;
    while (count) {
        ssize_t n = sending ? send(fd, p, count, 0) : recv(fd, p, count, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -(n < 0 ? errno : EIO);
        p += n;
        count -= (size_t)n;
    }
    return 0;
}

/* A transport/protocol/server error is sticky: no subsequent image I/O may
 * silently continue with incomplete source data or an unavailable ledger. */
static int rpc(uint16_t op, uint64_t offset, uint32_t length, void *data)
{
    unsigned char request[24] = {0}, reply[16];
    struct stat identity;
    struct timeval timeout = {120, 0};
    uid_t peer_uid;
    gid_t peer_gid;
    int fd = -1, error = -EIO, no_sigpipe = 1;
    if (failed) return -EIO;
    if (length > MAX_IO || (op == RPC_FSYNC && (offset || length))) return -EINVAL;
    if (lstat(endpoint.sun_path, &identity) || !S_ISSOCK(identity.st_mode) ||
        identity.st_dev != endpoint_device || identity.st_ino != endpoint_inode ||
        identity.st_uid != endpoint_owner) goto done;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) goto done;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) goto done;
    if (connect(fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) ||
        getpeereid(fd, &peer_uid, &peer_gid) || peer_uid != endpoint_owner) goto done;
    put32(request, RPC_MAGIC);
    put16(request + 4, 1);
    put16(request + 6, op);
    put64(request + 8, offset);
    put32(request + 16, length);
    if ((error = transfer(fd, request, sizeof(request), 1))) goto done;
    if (op == RPC_WRITE && (error = transfer(fd, data, length, 1))) goto done;
    if ((error = transfer(fd, reply, sizeof(reply), 0))) goto done;
    error = -EIO;
    if (get32(reply) != RPC_MAGIC || get32(reply + 12) ||
        get32(reply + 4) > INT_MAX) goto done;
    if (get32(reply + 4)) {
        if (!get32(reply + 8)) error = -(int)get32(reply + 4);
        goto done;
    }
    if (get32(reply + 8) != length) goto done;
    if (op == RPC_READ && (error = transfer(fd, data, length, 0))) goto done;
    error = 0;
done:
    if (fd >= 0) close(fd);
    if (error) failed = 1;
    return error;
}

static int is_image(const char *path)
{
    return path && !strcmp(path, "/root.hfs");
}

static int permitted(void)
{
    const struct fuse_context *context = fuse_get_context();
    return context && (context->uid == owner || context->uid == 0);
}

static int image_getattr(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_uid = owner;
    st->st_gid = group;
    st->st_blksize = 4096;
    if (path && !strcmp(path, "/")) {
        st->st_mode = S_IFDIR | 0700;
        st->st_nlink = 2;
        st->st_ino = 1;
    } else if (is_image(path)) {
        st->st_mode = S_IFREG | 0600;
        st->st_nlink = 1;
        st->st_ino = 2;
        st->st_size = (off_t)image_size;
        st->st_blocks = (blkcnt_t)(image_size / 512);
    } else {
        return -ENOENT;
    }
    return 0;
}

static int image_open(const char *path, struct fuse_file_info *fi)
{
    if (!is_image(path)) return -ENOENT;
    if (!permitted()) return -EACCES;
    if (failed) return -EIO;
    if ((fi->flags & (O_TRUNC | O_APPEND | O_CREAT)) ||
        (fi->flags & O_ACCMODE) == O_ACCMODE) return -EPERM;
    fi->direct_io = 1;
    fi->keep_cache = 0;
    fi->fh = 2;
    return 0;
}

static int image_read(const char *path, char *buffer, size_t length, off_t offset,
                      struct fuse_file_info *fi)
{
    (void)fi;
    if (!is_image(path)) return -ENOENT;
    if (!permitted()) return -EACCES;
    if (failed) return -EIO;
    if (offset < 0 || length > MAX_IO) return -EINVAL;
    if ((uint64_t)offset >= image_size || !length) return 0;
    if (length > image_size - (uint64_t)offset)
        length = (size_t)(image_size - (uint64_t)offset);
    int result = rpc(RPC_READ, (uint64_t)offset, (uint32_t)length, buffer);
    return result ? result : (int)length;
}

static int image_write(const char *path, const char *buffer, size_t length, off_t offset,
                       struct fuse_file_info *fi)
{
    (void)fi;
    if (!is_image(path)) return -ENOENT;
    if (!permitted()) return -EACCES;
    if (failed) return -EIO;
    if (offset < 0 || length > MAX_IO) return -EINVAL;
    if ((uint64_t)offset > image_size || length > image_size - (uint64_t)offset)
        return -ENOSPC;
    if (!length) return 0;
    int result = rpc(RPC_WRITE, (uint64_t)offset, (uint32_t)length, (void *)buffer);
    return result ? result : (int)length;
}

static int image_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)datasync;
    (void)fi;
    if (!is_image(path)) return -ENOENT;
    if (!permitted()) return -EACCES;
    return rpc(RPC_FSYNC, 0, 0, NULL);
}

static int image_flush(const char *path, struct fuse_file_info *fi)
{
    /* flush is not intrinsically fsync (per installed fuse.h); explicitly
     * request the parent's durable barrier instead of assuming close syncs. */
    return image_fsync(path, 0, fi);
}

static int image_truncate(const char *path, off_t size)
{
    if (!is_image(path)) return -ENOENT;
    if (!permitted()) return -EACCES;
    if (failed) return -EIO;
    return size >= 0 && (uint64_t)size == image_size ? 0 : -EPERM;
}

static int image_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi;
    return image_truncate(path, size);
}

static int image_readdir(const char *path, void *buffer, fuse_fill_dir_t fill,
                         off_t offset, struct fuse_file_info *fi)
{
    (void)offset;
    (void)fi;
    if (!path || strcmp(path, "/")) return -ENOENT;
    if (!permitted()) return -EACCES;
    if (fill(buffer, ".", NULL, 0) || fill(buffer, "..", NULL, 0)) return 0;
    (void)fill(buffer, "root.hfs", NULL, 0);
    return 0;
}

static int image_statfs(const char *path, struct statvfs *st)
{
    if (!path || (strcmp(path, "/") && !is_image(path))) return -ENOENT;
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = 4096;
    st->f_blocks = image_size / 4096;
    /* No namespace or size allocation is permitted in the adapter. */
    st->f_files = 2;
    st->f_namemax = 255;
    return 0;
}

static void *image_init(struct fuse_conn_info *conn)
{
    conn->async_read = 0;
    conn->max_readahead = 0;
    if (conn->max_write > MAX_IO) conn->max_write = MAX_IO;
    return NULL;
}

static const struct fuse_operations operations = {
    .getattr = image_getattr,
    .open = image_open,
    .read = image_read,
    .write = image_write,
    .flush = image_flush,
    .fsync = image_fsync,
    .truncate = image_truncate,
    .ftruncate = image_ftruncate,
    .readdir = image_readdir,
    .statfs = image_statfs,
    .init = image_init,
    /* All other callbacks are absent: ENOSYS, never a fallback backing file. */
};

static int private_directory(const char *path, uid_t *uid)
{
    char resolved[PATH_MAX];
    struct stat st;
    if (!path || path[0] != '/' || !realpath(path, resolved) || strcmp(path, resolved) ||
        lstat(path, &st) || !S_ISDIR(st.st_mode) || (st.st_mode & 077)) return 0;
    if (st.st_uid != getuid() && geteuid() != 0) return 0;
    if (uid) *uid = st.st_uid;
    return 1;
}

int main(int argc, char **argv)
{
    char directory[PATH_MAX];
    struct stat socket_stat;
    uid_t directory_owner;
    char *end;
    if (argc != 4) {
        fputs("usage: range_fs <private-unix-socket> <size-bytes> <private-mountpoint>\n", stderr);
        return 2;
    }
    if (argv[1][0] != '/' || strlen(argv[1]) >= sizeof(endpoint.sun_path) ||
        strlen(argv[1]) >= sizeof(directory)) goto invalid;
    strcpy(directory, argv[1]);
    char *slash = strrchr(directory, '/');
    if (!slash || slash == directory || !slash[1]) goto invalid;
    *slash = 0;
    if (!private_directory(directory, &directory_owner) ||
        lstat(argv[1], &socket_stat) || !S_ISSOCK(socket_stat.st_mode) ||
        socket_stat.st_uid != directory_owner || !private_directory(argv[3], NULL)) goto invalid;
    if (!argv[2][0]) goto invalid;
    for (const char *p = argv[2]; *p; ++p) if (*p < '0' || *p > '9') goto invalid;
    errno = 0;
    image_size = strtoull(argv[2], &end, 10);
    if (errno || *end || image_size < 4096 || image_size > INT64_MAX || image_size % 4096)
        goto invalid;
    endpoint.sun_family = AF_UNIX;
    endpoint.sun_len = sizeof(endpoint);
    strcpy(endpoint.sun_path, argv[1]);
    endpoint_device = socket_stat.st_dev;
    endpoint_inode = socket_stat.st_ino;
    endpoint_owner = socket_stat.st_uid;
    owner = getuid();
    group = getgid();
    char *fuse_argv[] = {argv[0], "-s", "-f", "-o",
        "allow_root,max_read=1048576,max_readahead=0", argv[3], NULL};
    int result = fuse_main(6, fuse_argv, &operations, NULL);
    return result ? result : (failed ? 1 : 0);
invalid:
    fputs("range_fs: invalid size, private socket, or private mountpoint\n", stderr);
    return 2;
}
