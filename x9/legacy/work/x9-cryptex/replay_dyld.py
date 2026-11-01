#!/usr/bin/env python3
"""Replay original dyld argument parsing/stage gating and CacheFinder directory selection.
OS calls are mocked; this is not an iOS boot or cache mapping test.
"""
import argparse, json, struct, subprocess
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_PC

class Replay:
    def __init__(self, path):
        self.data = path.read_bytes()
        self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.u.mem_map(0, 0x100000)
        self.u.mem_map(0x1000000, 0x200000)
        self.symbols = {}
        for line in subprocess.check_output(['/opt/homebrew/opt/llvm/bin/llvm-nm', str(path)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 3:
                self.symbols[fields[2]] = int(fields[0], 16)
        off = 32
        fix = None
        for _ in range(struct.unpack_from('<I', self.data, 16)[0]):
            cmd, size = struct.unpack_from('<II', self.data, off)
            if cmd == 0x19:
                _, va, _, fo, fs = struct.unpack_from('<16sQQQQ', self.data, off + 8)
                if fs:
                    self.u.mem_write(va, self.data[fo:fo+fs])
            if cmd == 0x80000034:
                fix = struct.unpack_from('<I', self.data, off + 8)[0]
            off += size
        assert fix is not None
        starts = fix + struct.unpack_from('<I', self.data, fix+4)[0]
        for i in range(struct.unpack_from('<I', self.data, starts)[0]):
            rel = struct.unpack_from('<I', self.data, starts+4+4*i)[0]
            if not rel:
                continue
            at = starts + rel
            _, page_size, fmt, segment, _, pages = struct.unpack_from('<IHHQIH', self.data, at)
            assert fmt == 6  # DYLD_CHAINED_PTR_64_OFFSET, no imports in this dyld
            for page in range(pages):
                head = struct.unpack_from('<H', self.data, at+22+page*2)[0]
                if head == 0xffff:
                    continue
                assert head < 0x8000
                cursor = segment + page*page_size + head
                while True:
                    raw = self.q(cursor)
                    assert not raw >> 63
                    nxt = (raw >> 51) & 0xfff
                    target = (raw & 0xfffffffff) | (((raw >> 36) & 0xff) << 56)
                    self.putq(cursor, target)
                    if not nxt:
                        break
                    cursor += 4*nxt
        self.hooks = {}
        self.u.hook_add(UC_HOOK_CODE, self.dispatch)
    def q(self, a): return struct.unpack('<Q', self.u.mem_read(a, 8))[0]
    def putq(self, a, n): self.u.mem_write(a, struct.pack('<Q', n))
    def string(self, a):
        return bytes(self.u.mem_read(a, 1024)).split(b'\0')[0].decode()
    def ret(self, value=0):
        self.u.reg_write(UC_ARM64_REG_X0, value & 0xffffffffffffffff)
        self.u.reg_write(UC_ARM64_REG_PC, self.u.reg_read(UC_ARM64_REG_X30))
    def dispatch(self, u, address, size, _):
        if address in self.hooks:
            self.hooks[address]()
    def hook(self, symbol, fn): self.hooks[self.symbols[symbol]] = fn
    def call(self, symbol, *args):
        self.u.reg_write(UC_ARM64_REG_SP, 0x11ff000)
        self.u.reg_write(UC_ARM64_REG_X30, 0xf0000)
        for i, value in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0+i, value)
        self.u.emu_start(self.symbols[symbol], 0xf0000, count=200000)
        assert self.u.reg_read(UC_ARM64_REG_PC) == 0xf0000, symbol
        return self.u.reg_read(UC_ARM64_REG_X0)

def stage_replay(binary, args):
    r = Replay(binary)
    r.u.mem_write(0x1000000, args.encode()+b'\0')
    r.call('_arguments_init', 0x1000000)
    argp = r.q(0x8c1d8)
    level = r.q(argp)
    called, result = [], []
    r.hook('_getpid', lambda: r.ret(1))
    r.hook('_dlog', lambda: r.ret())
    for symbol in ['__stage_hello','__stage_preboot','__stage_cryptex1_sniff','__stage_graft','__stage_graft_select','__stage_dylib_cache','__stage_goodbye']:
        stage = r.symbols[symbol]
        name = r.string(r.q(stage))
        r.hooks[r.q(stage+0x30)] = lambda name=name: (called.append(name), r.ret())
        r.hooks[r.q(stage+0x38)] = lambda: r.ret(1)
        prior = len(called)
        status = r.call('_stage_fire', stage, 0x1010000)
        result.append({'stage': name, 'minimum_level': r.q(stage+0x28), 'called': len(called)>prior, 'status': status})
    return {'boot_args': args, 'parsed_level': level, 'parsed_force_dylib_root': r.q(argp+8), 'stages': result}

def cache_replay(binary, directory, ignition_errno, skip=False, fd=71):
    r = Replay(binary)
    finder, process, logging, syscall = 0x1000000,0x1001000,0x1002000,0x1003000
    envv, envstr = 0x1004000,0x1005000
    r.u.mem_write(envstr, ('DYLD_SHARED_CACHE_DIR='+directory).encode()+b'\0')
    r.putq(envv, envstr); r.putq(envv+8, 0)
    r.putq(process+0x68, envv)
    r.u.mem_write(process+0x80, bytes([8 if skip else 0]))
    ignite_calls=[]
    # PID1 follows the real constructor branch; only console writes are mocked.
    r.u.mem_write(process+0x38, struct.pack('<II',1,2))
    r.hook('__ZN5dyld47consoleEPKcz', lambda: r.ret())
    paths = []
    def opendir():
        path = r.string(r.u.reg_read(UC_ARM64_REG_X1)); paths.append(path)
        assert path == directory
        r.ret(fd)
    r.hook('__ZNK5dyld415SyscallDelegate4openEPKcii', opendir)
    r.hook('_ignite', lambda: (ignite_calls.append(1),r.ret(ignition_errno)))
    r.call('__ZN5dyld411CacheFinderC2ERKNS_13ProcessConfig7ProcessERKNS1_7LoggingERNS_15SyscallDelegateE', finder,process,logging,syscall)
    selected = struct.unpack('<i', r.u.mem_read(finder,4))[0]
    assert selected == fd and paths == [directory]
    assert len(ignite_calls)==(0 if skip else 1)
    return {'mocked_ignition_errno': ignition_errno,'opened_directories': paths,'selected_mock_fd':selected,'skipIgnition':skip,'ignite_calls':len(ignite_calls),'default_directory_opened':False}

def main():
    ap=argparse.ArgumentParser();ap.add_argument('dyld',type=Path);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
    old=stage_replay(a.dyld,'debug=0xc')
    new=stage_replay(a.dyld,'debug=0xc ignition_level=1')
    assert old['parsed_level']==5 and new['parsed_level']==1
    assert next(x for x in old['stages'] if x['stage']=='preboot')['called']
    assert not next(x for x in new['stages'] if x['stage']=='preboot')['called']
    assert next(x for x in new['stages'] if x['stage']=='dylib_cache')['called']
    directory='/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld'
    caches=[cache_replay(a.dyld,directory,e) for e in [0,19,89]]
    caches += [cache_replay(a.dyld,directory,19,skip=True,fd=f) for f in [0,3,71]]
    a.output.write_text(json.dumps({'scope':'original dyld machine code; stage bodies and OS calls mocked, no cache mapping or hardware emulation','before':old,'after':new,'cache_selection':caches},indent=2)+'\n')
    print('Original dyld replay: level5 -> level1 skips Preboot; skipIgnition bit3 prevents ignite call and preserves FD0/3/71 (OS calls mocked).')
if __name__=='__main__':main()
