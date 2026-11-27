#!/usr/bin/env python3
"""Read-only RV32 slice disassembly with independent instruction framing.

Some rv_v2 vendor instructions are unsupported by r2. Its invalid-op fallback
can advance TWO bytes into a FOUR-byte encoding and invent subsequent calls.
Frame from RISC-V instruction-length bits first, invoke r2 at each exact PC,
and preserve unknown/reserved encodings rather than adopting those fake ops.
This does not decode the vendor ISA, prove a function entry, or build a CFG.
Engine acceptance only means the decoder supplied a matching-length mnemonic;
vendor instructions can reuse standard encodings, so it is not an ISA proof.
"""
import argparse
import json
import os
from pathlib import Path
import stat
import subprocess
import sys


MAX_SLICE_BYTES = 32768
MAX_INSTRUCTIONS = 4096


def encoding_bytes(halfword):
    if (halfword & 3) != 3:
        return 2
    if (halfword & 0x1f) != 0x1f:
        return 4
    if (halfword & 0x3f) == 0x1f:
        return 6
    if (halfword & 0x7f) == 0x3f:
        return 8
    extension = (halfword >> 12) & 7
    if extension == 7:
        raise ValueError('reserved >=192-bit instruction length is not framed')
    return 10 + 2 * extension


def rv32_reserved(raw):
    h = int.from_bytes(raw[:2], 'little')
    if len(raw) == 2:
        quadrant, funct3 = h & 3, h >> 13
        # These are RV64 shift encodings, NOT RV32C shifts by 32..63. Vendor
        # push/pop encodings may reuse them; their semantics stay unknown.
        if (h & 0x1000) and ((quadrant == 1 and funct3 == 4 and
                           ((h >> 10) & 3) in (0, 1)) or
                          (quadrant == 2 and funct3 == 0)):
            return 'RV32C reserved shift encoding (possible vendor instruction)'
    if len(raw) == 4:
        w = int.from_bytes(raw, 'little')
        if (w & 0x7f) == 0x67 and ((w >> 12) & 7):
            return 'nonstandard JALR opcode/funct3 (vendor semantics unresolved)'
    if len(raw) > 4:
        return 'long encoding semantics unresolved'
    return None


def disassemble(binary, base, start, size, r2):
    if (base < 0 or start < base or (base & 1) or (start & 1) or
            not 0 < size <= MAX_SLICE_BYTES or start + size > (1 << 32)):
        raise ValueError('need a 2-byte aligned RV32 slice of 1..32768 bytes inside the input')
    binary = binary.resolve(strict=True)
    # Read only the requested slice, not an unbounded input or unrelated data.
    fd = os.open(binary, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as f:
        info = os.fstat(f.fileno())
        if (not stat.S_ISREG(info.st_mode) or
                start - base + size > info.st_size):
            raise ValueError('slice input must be a regular file containing the requested bytes')
        f.seek(start - base)
        data = f.read(size)
    if len(data) != size:
        raise ValueError('slice exceeds input')
    frames = []
    offset = 0
    while offset < size:
        if size - offset < 2:
            raise ValueError('partial final halfword; choose an instruction boundary')
        n = encoding_bytes(int.from_bytes(data[offset:offset+2], 'little'))
        if n > size - offset:
            raise ValueError('partial final instruction; choose an instruction boundary')
        if len(frames) >= MAX_INSTRUCTIONS:
            raise ValueError('slice exceeds 4096 instructions; choose a smaller window')
        frames.append((start + offset, data[offset:offset+n]))
        offset += n
    commands = ';'.join(f'pdj 1 @ {pc:#x}' for pc, _ in frames)
    # Raw mapping, no automatic executable-loader interpretation or analysis.
    # The instruction ceiling also bounds argv length on supported CLI hosts.
    result = subprocess.run([r2, '-n', '-a', 'riscv', '-b', '32', '-m', hex(base),
                             '-e', 'scr.color=0', '-qc', commands, str(binary)],
                            text=True, capture_output=True, check=True, timeout=60)
    after = binary.stat()
    if ((info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns) !=
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)):
        raise ValueError('input identity or metadata changed during disassembly')
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if len(lines) != len(frames):
        raise ValueError('r2 returned a different number of PC records')
    records = []
    for (pc, raw), line in zip(frames, lines):
        parsed = json.loads(line)
        if (not isinstance(parsed, list) or len(parsed) != 1 or
                not isinstance(parsed[0], dict) or parsed[0].get('addr') != pc):
            raise ValueError(f'r2 did not return the requested PC {pc:#x}')
        op = parsed[0]
        if op.get('bytes') != raw.hex() and op.get('size') == len(raw):
            raise ValueError(f'r2 bytes differ from the input at PC {pc:#x}')
        reason = rv32_reserved(raw)
        if op.get('size') != len(raw):
            reason = reason or 'engine instruction length differs from encoded framing'
        mnemonic = op.get('disasm', '')
        if not mnemonic or op.get('type') == 'ill' or 'invalid' in mnemonic or 'unknown' in mnemonic:
            reason = reason or 'unsupported encoding'
        # Raw bytes come from the input, NOT from a decoder's partial fetch.
        records.append(dict(address=pc, raw_hex=raw.hex(), encoded_bytes=len(raw),
                            engine_bytes=op.get('size'), engine_disasm=mnemonic,
                            engine_accepted=reason is None, unresolved_reason=reason))
    return dict(input=str(binary), base=base, start=start, bytes=size,
                instruction_framing='RISC-V encoded length bits, not r2 fallback size',
                engine_acceptance_is_semantics_proof=False,
                vendor_isa_decoded=False, entry_or_complete_cfg_proven=False,
                engine_stderr=result.stderr, instructions=records)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary', type=Path)
    p.add_argument('--base', type=lambda s: int(s, 0), required=True)
    p.add_argument('--start', type=lambda s: int(s, 0), required=True)
    p.add_argument('--bytes', type=lambda s: int(s, 0), required=True)
    p.add_argument('--r2', default='r2')
    p.add_argument('--out', type=Path, required=True,
                   help='new JSON path; also writes a new same-stem .asm file')
    a = p.parse_args()
    assembly = a.out.with_suffix('.asm')
    if a.out == assembly or a.out.exists() or assembly.exists():
        p.error('outputs must be distinct new paths; original evidence is never overwritten')
    try:
        evidence = disassemble(a.binary, a.base, a.start, a.bytes, a.r2)
        rendered = []
        unknown = 0
        for op in evidence['instructions']:
            text = op['engine_disasm']
            if not op['engine_accepted']:
                unknown += 1
                text = f"<unresolved: {op['unresolved_reason']}> ; r2: {text}"
            rendered.append(f"{op['address']:#010x}  {op['raw_hex']:<16s} {text}")
        a.out.parent.mkdir(parents=True, exist_ok=True)
        with a.out.open('x') as f:
            f.write(json.dumps(evidence, indent=2) + '\n')
        with assembly.open('x') as f:
            f.write('\n'.join(rendered) + '\n')
        print(f"{a.out}: {len(rendered)} independently framed instructions; {unknown} unresolved")
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        print(f'error: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
