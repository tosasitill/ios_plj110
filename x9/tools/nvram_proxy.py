"""Build an empty software NVRAM proxy for XNU8796's CHRP handler.
This is volatile bootstrap state, not a captured bank or physical backend.
No Android NVRAM, flash addresses, variables or credentials are imported.
"""
import struct
import zlib

BANK_SIZE=8192
BANK_COUNT=1
CURRENT_BANK=0

def chrp_header(name,size):
    if not name or len(name)>12 or size<16 or size%16 or size//16>65535:
        raise ValueError('invalid CHRP header')
    raw=bytearray(struct.pack('<BBH12s',0x70,0,size//16,name))
    total=raw[0]+sum(raw[2:])
    while total>255:total=(total&255)+(total>>8)
    raw[1]=total
    return bytes(raw)

def image():
    data=bytearray(BANK_SIZE)
    # Apple header is32 bytes; partition scanner advances by its CHRP length.
    data[:16]=chrp_header(b'nvram',32)
    data[32:48]=chrp_header(b'common',16+2048)
    free_at=32+16+2048
    data[free_at:free_at+16]=chrp_header(b'free',BANK_SIZE-free_at)
    # XNU v1/v2 Adler starts at apple_nvram_header.generation, offset20.
    struct.pack_into('<I',data,16,zlib.adler32(data[20:]))
    return bytes(data)

def chosen_properties():
    return [('nvram-bank-size',struct.pack('<I',BANK_SIZE)),
            ('nvram-bank-count',struct.pack('<I',BANK_COUNT)),
            ('nvram-current-bank',struct.pack('<I',CURRENT_BANK)),
            ('nvram-proxy-data',image())]
