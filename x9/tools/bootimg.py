"""Deterministic X9 LK boot.img v4 + legacy LZ4 Image packing and verification."""
import hashlib
import struct

PAGE = 4096
BOOT_PARTITION_SIZE = 0x4000000
# The kernel still has to fit between LK's load window and the DTB/tags area it
# writes above it; v4 does not carry these addresses, but the constraint stands.
IMAGE_LIMIT = 0x7c80000
LZ4_MAGIC = 0x184c2102
LZ4_BLOCK_SIZE = 8 << 20
# Stock boot_a.img: header_version 4, header_size 1584, no fixed load addresses
# in the header (v3+ moved them into the vendor_boot/bootconfig path).
HEADER_V4_VERSION = 4
HEADER_V4_SIZE = 1584
HEADER_V4_CMDLINE = 1536


def align(n, page=PAGE):
    return (n + page - 1) & ~(page - 1)


def pad(b):
    return b + bytes(align(len(b)) - len(b))


def empty_cpio():
    name = b'TRAILER!!!\0'
    fields = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0]
    record = b'070701' + b''.join(b'%08x' % n for n in fields) + name
    return record + bytes(-len(record) % 4)


def lz4_legacy(image):
    import lz4.block
    out = bytearray(struct.pack('<I', LZ4_MAGIC))
    for off in range(0, len(image), LZ4_BLOCK_SIZE):
        compressed = lz4.block.compress(image[off:off + LZ4_BLOCK_SIZE], store_size=False)
        out += struct.pack('<I', len(compressed)) + compressed
    return bytes(out)


def unlz4_legacy(blob):
    import lz4.block
    if len(blob) < 4 or struct.unpack_from('<I', blob)[0] != LZ4_MAGIC:
        raise ValueError('not an LZ4 legacy frame')
    off, image = 4, bytearray()
    while off < len(blob):
        if len(blob) - off < 4:
            raise ValueError('truncated LZ4 block size')
        size, = struct.unpack_from('<I', blob, off)
        off += 4
        if not size or size > len(blob) - off:
            raise ValueError('invalid LZ4 block size')
        block = lz4.block.decompress(blob[off:off + size], uncompressed_size=LZ4_BLOCK_SIZE)
        if len(image) + len(block) > IMAGE_LIMIT:
            raise ValueError('expanded Image overlaps the LK DTB target')
        image += block
        off += size
    return bytes(image)


def check_image(image):
    if len(image) < PAGE or len(image) > IMAGE_LIMIT:
        raise ValueError('Image size is outside the LK kernel load window')
    text_offset, image_size, flags = struct.unpack_from('<QQQ', image, 8)
    if image[:4] != b'MZ@\xfa' or image[56:60] != b'ARM\x64' or text_offset or flags != 0xa:
        raise ValueError('bad direct ARM64 Image header')
    if image_size != len(image) or image_size % PAGE:
        raise ValueError('Image header does not include zero-filled stacks and payload')
    pe, = struct.unpack_from('<I', image, 60)
    if pe > len(image) - 44 or image[pe:pe + 4] != b'PE\0\0':
        raise ValueError('missing PE header')
    instruction, = struct.unpack_from('<I', image, 4)
    if instruction >> 26 != 5:
        raise ValueError('ARM64 Image does not branch directly from +4')
    imm = instruction & 0x3ffffff
    if imm & (1 << 25):
        imm -= 1 << 26
    entry = 4 + imm * 4
    pe_entry, = struct.unpack_from('<I', image, pe + 24 + 16)
    if entry != pe_entry or entry < PAGE or entry >= image_size:
        raise ValueError('direct and PE entries must both target the actual shim')
    return entry


def pack(image, ramdisk=None, cmdline='console=null'):
    check_image(image)
    kernel = lz4_legacy(image)
    ramdisk = empty_cpio() if ramdisk is None else ramdisk
    if not ramdisk:
        raise ValueError('X9 LK requires a nonempty ramdisk')
    command = cmdline.encode()
    if len(command) >= HEADER_V4_CMDLINE or b'\0' in command:
        raise ValueError('invalid Android boot command line')
    # v4: v3 header layout plus a 4-byte signature_size field at 1580.
    # No AVB footer is emitted; see the README for what that does and does
    # not establish about LK's verification.
    header = struct.pack('<8sIIII16sI1536sI',
        b'ANDROID!', len(kernel), len(ramdisk), 0, HEADER_V4_SIZE,
        bytes(16), HEADER_V4_VERSION, command.ljust(HEADER_V4_CMDLINE, b'\0'),
        0)
    if len(header) != HEADER_V4_SIZE:
        raise ValueError('internal error: v4 header size')
    boot = pad(header) + pad(kernel) + pad(ramdisk)
    if len(boot) > BOOT_PARTITION_SIZE:
        raise ValueError('boot.img exceeds the captured 64 MiB boot partition')
    return boot


def unpack(boot):
    if len(boot) < PAGE or len(boot) > BOOT_PARTITION_SIZE:
        raise ValueError('bad boot image size')
    magic, ks, rs, osv, hs = struct.unpack_from('<8sIIII', boot, 0)
    if magic != b'ANDROID!':
        raise ValueError('not an Android boot image')
    if hs != HEADER_V4_SIZE or boot[24:40] != bytes(16):
        raise ValueError('X9 requires the stock v4 header size and zero reserved fields')
    hv, = struct.unpack_from('<I', boot, 40)
    if hv != HEADER_V4_VERSION:
        raise ValueError(f'X9 requires boot header v{HEADER_V4_VERSION}, got v{hv}')
    if not ks or not rs:
        raise ValueError('zero-size kernel or ramdisk')
    command = boot[44:44 + HEADER_V4_CMDLINE]
    if command[:len(command) - len(command.lstrip(b'\0'))]:
        raise ValueError('boot command line must start at offset 44')
    rd = PAGE + align(ks)
    if rd + align(rs) != len(boot):
        raise ValueError('boot image sizes do not match the file')
    kernel, ramdisk = boot[PAGE:PAGE + ks], boot[rd:rd + rs]
    image = unlz4_legacy(kernel)
    entry = check_image(image)
    return image, ramdisk, {
        'header_version': hv, 'header_size': hs,
        'kernel_address': 'LK default', 'ramdisk_address': 'LK default',
        'entry_offset': hex(entry),
        'image_bytes': len(image), 'boot_bytes': len(boot), 'ramdisk_bytes': rs,
        'cmdline': command.split(b'\0')[0].decode('ascii', 'replace'),
        'kernel_compression': 'lz4-legacy',
        'image_sha256': hashlib.sha256(image).hexdigest(),
        'boot_sha256': hashlib.sha256(boot).hexdigest(),
        'hardware_tested': False,
    }
