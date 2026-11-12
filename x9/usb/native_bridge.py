#!/usr/bin/env python3
"""Prepare class-local native USB dispatch adapters, not a runtime kext loader.

Pinned original input only; never inspect or modify a successful boot image.
Recover constructors/metaclasses and native vtables, then conservatively track
virtual receivers through bounded control-flow graphs. Emit symbolic expanded
vtables and compare-before-write instruction plans into a NEW private directory.
Unqualified calls, field layout and DMA/bootstrap obligations remain blockers.
"""
import argparse
from bisect import bisect_right
from collections import deque
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_native import ROOT, SHA256, MachO, analyze, object_vtable
from native_abi import nondeleting_destructor


def symbol(class_name, suffix):
    return '__ZN' + str(len(class_name)) + class_name + suffix


def decode(cs, cache, address, size):
    rows = list(cs.disasm(cache.read(address, size), address))
    if len(rows)*4 != size:
        raise ValueError(f'incomplete arm64 decode at {address:#x}')
    return rows


def code_address(cache, address):
    return any(s['initprot'] & 4 and s['va'] <= address < s['va']+s['filesize']
               for s in cache.segments)


def stub_target(cs, cache, address):
    rows = decode(cs, cache, address, 12)
    a, b, z = rows
    if (a.mnemonic != 'adrp' or not a.op_str.startswith('x16, #')
            or b.mnemonic != 'ldr'
            or not re.fullmatch(r'x16, \[x16(?:, #0x[0-9a-f]+)?\]', b.op_str)
            or (z.mnemonic, z.op_str) != ('br', 'x16')):
        raise ValueError('unqualified external import stub')
    page = int(a.op_str.split('#')[1], 0)
    offset = int(b.op_str.split('#')[1][:-1], 0) if '#' in b.op_str else 0
    return struct.unpack('<Q', cache.read(page+offset, 8))[0]


def pointers(cache, vptr, limit=512):
    if struct.unpack('<2Q', cache.read(vptr-16, 16)) != (0, 0):
        raise ValueError('unqualified vtable prefix/single-base geometry')
    result = []
    for slot in range(limit):
        a, = struct.unpack('<Q', cache.read(vptr+8*slot, 8))
        if not code_address(cache, a):
            return result
        result.append(a)
    raise ValueError('vtable boundary not recovered')


def init_arguments(cs, cache, address):
    """Only the exact straight-line OSMetaClass initializer shape is allowed."""
    rows = decode(cs, cache, address, 160)
    # Common also has one ordinary C++ global initializer, not a metaclass.
    # Retain that bootstrap obligation; do not invent an allocation for it.
    if (rows[0].mnemonic == 'adrp' and rows[0].op_str.startswith('x0, #')
            and rows[1].mnemonic == 'add' and rows[1].op_str.startswith('x0, x0, #')
            and rows[2].mnemonic == 'b'):
        return [], dict(init_pc=address, tail_target=int(rows[2].op_str[1:], 0),
                        classification='non-metaclass global initializer; bootstrap unqualified')
    regs, args, ctor_pc, result = {}, None, None, []
    for i in rows:
        parts = i.op_str.split(', ')
        if i.mnemonic == 'adrp':
            regs[parts[0]] = int(parts[1][1:], 0)
        elif (i.mnemonic == 'add' and len(parts) == 3
              and parts[2].startswith('#') and parts[1] in regs):
            regs[parts[0]] = regs[parts[1]] + int(parts[2][1:], 0)
        elif i.mnemonic == 'mov' and len(parts) == 2 and parts[1].startswith('#'):
            regs[parts[0]] = int(parts[1][1:], 0)
        elif i.mnemonic == 'mov' and len(parts) == 2 and parts[1] in regs:
            regs[parts[0]] = regs[parts[1]]
        elif i.mnemonic == 'ldr':
            match = re.fullmatch(r'(x\d+), \[(x\d+)(?:, #(0x[0-9a-f]+))?\]', i.op_str)
            if not match or match[2] not in regs:
                raise ValueError('unqualified parent metaclass import')
            offset = int(match[3], 0) if match[3] else 0
            regs[match[1]], = struct.unpack('<Q', cache.read(regs[match[2]]+offset, 8))
        elif i.mnemonic == 'bl':
            if args is not None:
                raise ValueError('multiple calls in class initializer')
            target = stub_target(cs, cache, int(i.op_str.removeprefix('#'), 0))
            if '__ZN11OSMetaClassC2EPKcPKS_j' not in cache.exports().get(target, []):
                raise ValueError('initializer does not call the original OSMetaClass constructor')
            args, ctor_pc = dict(regs), i.address
            for r in range(1, 19):
                regs.pop('x'+str(r), None)
                regs.pop('w'+str(r), None)
        elif i.mnemonic == 'str' and i.op_str == 'x8, [x0]':
            if args is None or 'x8' not in regs or any(k not in args for k in ('x0','x1','x2','w3')):
                raise ValueError('incomplete class initializer')
            name = cache.read(args['x1'], 128).split(b'\0', 1)[0].decode('ascii')
            result.append(dict(name=name, init_pc=address, meta=args['x0'],
                               parent_meta=args['x2'], instance_size=args['w3'],
                               meta_vptr=regs['x8'], ctor_call_pc=ctor_pc))
            args = None
        elif i.mnemonic == 'ret':
            break
        elif i.mnemonic == 'add' and parts[:2] == ['x29', 'sp']:
            pass
        elif i.mnemonic not in ('stp', 'ldp', 'mov'):
            raise ValueError(f'initializer shape at {i.address:#x}: {i.mnemonic} {i.op_str}')
    if args is not None or not result:
        raise ValueError('incomplete class initializer')
    return result, None


def recover_classes(cs, cache, modules, components):
    classes, instructions, other_inits = [], {}, []
    for component in components:
        module = modules[component]
        for s in module.sections:
            if s['name'] == '__mod_init_func':
                if s['size'] % 8:
                    raise ValueError('unaligned module initializer array')
                for off in range(0, s['size'], 8):
                    address, = struct.unpack('<Q', cache.read(s['va']+off, 8))
                    rows, other = init_arguments(cs, cache, address)
                    for row in rows:
                        row['component'] = component
                        classes.append(row)
                    if other:
                        other['component'] = component
                        other_inits.append(other)
        for s in module.segments:
            if s['initprot'] & 4 and s['filesize']:
                instructions.update({i.address: i for i in decode(cs, cache, s['va'], s['filesize'])})
    if len({c['name'] for c in classes}) != len(classes):
        raise ValueError('duplicate native class')
    metas = {c['meta']: c for c in classes}
    getters = {}
    for address, a in instructions.items():
        b, z = instructions.get(address+4), instructions.get(address+8)
        if (a.mnemonic == 'adrp' and a.op_str.startswith('x0, #') and b and z
                and b.mnemonic == 'add' and b.op_str.startswith('x0, x0, #')
                and z.mnemonic == 'ret'):
            meta = int(a.op_str.split('#')[1], 0)+int(b.op_str.split('#')[1], 0)
            if meta in metas:
                getters[address] = meta
    for component in components:
        for s in modules[component].segments:
            if s['name'] != '__DATA_CONST':
                continue
            for address in range(s['va'], s['va']+s['filesize']-7, 8):
                fn, = struct.unpack('<Q', cache.read(address, 8))
                if fn not in getters:
                    continue
                c = metas[getters[fn]]
                if 'vptr' in c:
                    raise ValueError('ambiguous native instance vtable')
                c['vptr'] = address-7*8
                c['targets'] = pointers(cache, c['vptr'])
                c['stock_slots'] = len(c['targets'])
                c['get_meta_class_pc'] = fn
    exports = cache.exports()
    for c in classes:
        if 'vptr' not in c or c['stock_slots'] < 15:
            raise ValueError('missing native instance vtable')
        parents = [n for n in exports.get(c['parent_meta'], [])
                   if re.fullmatch(r'__ZN\d+\w+10gMetaClassE', n)]
        if len(parents) != 1:
            raise ValueError('native/non-kernel parent requires separate qualification')
        match = re.fullmatch(r'__ZN(\d+)(\w+)10gMetaClassE', parents[0])
        c['parent'] = match[2]
        if int(match[1]) != len(c['parent']):
            raise ValueError('invalid metaclass name')
        c['layout_qualified'] = False
        c['layout_note'] = 'size/parent/vptr recovered; field-by-field parent layout NOT proved'
    return classes, instructions, other_inits


def parent_maps(cs, cache, object_root, classes):
    exports = cache.exports()
    result = {}
    for parent in sorted({c['parent'] for c in classes}):
        path = object_root / ('libkern' if parent == 'OSObject' else 'iokit') / 'DEVELOPMENT' / (parent+'.cpo')
        vtable_symbol = '__ZTV'+str(len(parent))+parent
        va = [a for a, n in exports.items() if vtable_symbol in n]
        if len(va) != 1:
            raise ValueError('ambiguous kernel parent vtable')
        old = pointers(cache, va[0]+16)
        obj = MachO(path.read_bytes())
        current = object_vtable(path, vtable_symbol)
        forward = []
        for slot, target in enumerate(old):
            matches = [j for j, name in enumerate(current) if name in exports.get(target, [])]
            forward.append(matches[0] if len(matches) == 1 else None)
        role = None
        if parent == 'IOService':
            role = nondeleting_destructor(cache, path, parent, va[0]+16)
            if current[0] != role['current_symbol']:
                raise ValueError('IOService nondeleting role mismatch')
            forward[0] = 0
        result[parent] = dict(stock_vptr=va[0]+16, stock_targets=old,
                              current_symbols=current, forward=forward,
                              stock_instance_size=metaclass_size(cs, cache, parent),
                              current_instance_size=metaclass_size(cs, obj, parent),
                              destructor_role=role, frozen_object=str(path),
                              frozen_object_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    return result


def metaclass_size(cs, binary, class_name):
    name = symbol(class_name, '9MetaClassC2Ev')
    entries = [a for n,a,_,_ in binary.symbols() if n == name]
    if len(entries) != 1:
        raise ValueError('missing/ambiguous parent metaclass constructor')
    relocations = {}
    symbols = binary.symbols()
    if binary.kind == 1:
        for section in binary.sections:
            for j in range(section['nreloc']):
                off, flags = struct.unpack_from('<iI', binary.data, section['reloff']+8*j)
                if flags >> 27 & 1 and flags >> 28 == 2:
                    relocations[section['va']+off] = symbols[flags & 0xffffff][0]
    size = None
    for i in decode(cs, binary, entries[0], 96):
        if i.mnemonic == 'mov' and i.op_str.startswith('w3, #'):
            size = int(i.op_str.split('#')[1], 0)
        elif i.mnemonic == 'bl':
            aliases = ([relocations[i.address]] if i.address in relocations else
                       binary.exports().get(int(i.op_str[1:], 0), []))
            accepted = {'__ZN11OSMetaClassC2EPKcPKS_j',
                        '__ZN11OSMetaClassC2EPKcPKS_jPP4zoneS1_19zone_create_flags_t'}
            if not accepted.intersection(aliases) or size is None:
                raise ValueError(f'parent {class_name} metaclass at {i.address:#x}: size={size}, call={aliases}')
            return size
    raise ValueError('parent metaclass allocation size not recovered')


def class_slot(c, parents, slot):
    p = parents[c['parent']]
    if slot < len(p['forward']):
        return p['forward'][slot]
    if slot < c['stock_slots']:
        return len(p['current_symbols'])+slot-len(p['forward'])
    return None


def templates(classes, parents):
    """Both directions: defaults from CURRENT parent, native overrides mapped back."""
    result = []
    for c in classes:
        p = parents[c['parent']]
        table = [dict(current_slot=j, kernel_symbol=n, kind='current-parent-default')
                 for j, n in enumerate(p['current_symbols'])]
        unmapped = []
        for old, target in enumerate(c['targets']):
            new = class_slot(c, parents, old)
            if new is None:
                unmapped.append(old)
                continue
            # Never carry an inherited stock kernel pointer into a new kernel.
            if old < len(p['stock_targets']) and target == p['stock_targets'][old]:
                continue
            row = dict(current_slot=new, stock_slot=old, stock_target=target,
                       kind='native-override-or-tail', requires_relocation=True)
            if new < len(table):
                table[new] = row
            elif new == len(table):
                table.append(row)
            else:
                raise ValueError('noncontiguous native tail')
        if c['instance_size'] < p['stock_instance_size']:
            raise ValueError('native allocation smaller than its recovered parent')
        result.append(dict(class_name=c['name'], stock_vptr=c['vptr'],
                           stock_slots=c['stock_slots'], current_slots=len(table),
                           parent_sizes_equal=p['stock_instance_size'] == p['current_instance_size'],
                           slots=table, unmapped_stock_slots=unmapped, callable=False))
    return result


def qualify_calls(classes, parents, instructions, cache):
    """Must-dataflow only. No argument/return type guesses, no stack recovery.

    A vtable origin AND x0 must agree on every reaching path. Unsupported
    writes kill provenance, calls kill caller-saved registers, merges keep
    only equal facts. Analysis stops at other known method entries/bounds.
    """
    from capstone.arm64 import ARM64_OP_REG, ARM64_OP_MEM, ARM64_OP_IMM
    byname = {c['name']: c for c in classes}
    parent_headers = {p['stock_vptr']-16: name for name,p in parents.items()}
    expectations = {}
    seeds = {}
    for c in classes:
        for target in c['targets']:
            if target in instructions:
                seeds.setdefault(target, set()).add(c['name'])
    starts = sorted(seeds)
    qualified, uncertain = {}, set()
    for entry, names in seeds.items():
        nxt = bisect_right(starts, entry)
        high = min(entry+16384, starts[nxt] if nxt < len(starts) else entry+16384)
        origins = tuple(sorted(names))
        incoming = {entry: {'x0': ('object', origins)}}
        work = deque([entry])
        while work:
            pc = work.popleft()
            i = instructions.get(pc)
            if i is None or not entry <= pc < high:
                continue
            before = incoming[pc]
            after = dict(before)
            ops = i.operands
            _, written = i.regs_access()
            for r in written:
                n = i.reg_name(r)
                after.pop('x'+n[1:] if n.startswith('w') else n, None)
            if i.mnemonic == 'mov' and len(ops) == 2 and all(o.type == ARM64_OP_REG for o in ops):
                dst, src = (i.reg_name(o.reg) for o in ops)
                if dst.startswith('x') and src in before:
                    after[dst] = before[src]
            if i.mnemonic == 'adrp' and len(ops) == 2 and ops[1].type == ARM64_OP_IMM:
                # Capstone detail exposes high-half kernel VAs as signed
                # immediates, while .address/cache VAs are unsigned. Normalize
                # BOTH constants and CFG edges, never drop taken branches.
                after[i.reg_name(ops[0].reg)] = ('constant', ops[1].imm & ((1 << 64)-1))
            if (i.mnemonic == 'add' and len(ops) == 3 and ops[0].type == ARM64_OP_REG
                    and ops[1].type == ARM64_OP_REG and ops[2].type == ARM64_OP_IMM
                    and not ops[2].shift.value):
                source = before.get(i.reg_name(ops[1].reg))
                if source and source[0] == 'constant':
                    after[i.reg_name(ops[0].reg)] = ('constant', source[1]+ops[2].imm)
            if (i.mnemonic == 'ldr' and len(ops) == 2 and ops[0].type == ARM64_OP_REG
                    and ops[1].type == ARM64_OP_MEM and not ops[1].mem.index and not i.writeback):
                dst = i.reg_name(ops[0].reg)
                origin = before.get(i.reg_name(ops[1].mem.base))
                offset = ops[1].mem.disp
                if dst.startswith('x') and origin:
                    if origin[0] == 'object' and offset == 0:
                        after[dst] = ('vtable', origin[1])
                    elif origin[0] == 'vtable' and offset >= 0 and offset % 8 == 0:
                        after[dst] = ('function', origin[1], offset//8, pc)
                    elif origin[0] == 'constant':
                        try:
                            target, = struct.unpack('<Q', cache.read(origin[1]+offset, 8))
                        except ValueError:
                            target = None
                        if target in parent_headers:
                            after[dst] = ('parent-header', parent_headers[target])
                    elif origin[0] == 'parent-header' and offset >= 16 and offset % 8 == 0:
                        # Imported __ZTV points at TWO header words. An
                        # instance vptr does not. Treating both alike silently
                        # maps super clear/prepare to the wrong methods.
                        after[dst] = ('parent-function', origin[1], offset//8-2, pc)
            if i.mnemonic in ('blr', 'br') and ops[0].type == ARM64_OP_REG:
                fn = before.get(i.reg_name(ops[0].reg))
                receiver = before.get('x0')
                compatible = False
                header_words = 0
                if fn and fn[0] == 'function' and receiver == ('object', fn[1]):
                    slots = {class_slot(byname[n], parents, fn[2]) for n in fn[1]}
                    compatible = True
                elif (fn and fn[0] == 'parent-function' and receiver and receiver[0] == 'object'
                      and all(byname[n]['parent'] == fn[1] for n in receiver[1])):
                    mapping = parents[fn[1]]['forward']
                    slots = {mapping[fn[2]] if fn[2] < len(mapping) else None}
                    compatible, header_words = True, 2
                if compatible:
                    # The load is an unsigned, scaled 64-bit LDR, not LDUR,
                    # register-offset, or a byte/32-bit access.
                    load = instructions[fn[3]]
                    word = int.from_bytes(load.bytes, 'little')
                    if len(slots) == 1 and None not in slots and word & 0xffc00000 == 0xf9400000:
                        new = next(iter(slots))
                        if new+header_words <= 4095:
                            patched = (word & ~(0xfff << 10)) | ((new+header_words) << 10)
                            row = dict(load_pc=fn[3], call_pc=pc, method_entry=entry, method_limit=high,
                                       receiver_classes=list(receiver[1]), stock_slot=fn[2],
                                       current_slot=new, before_word=word, after_word=patched,
                                       dispatch_kind=fn[0], vtable_header_words=header_words,
                                       receiver_qualified=True, requires_expanded_vtable=True,
                                       instruction_patch_qualified=False, applied=False)
                            key = (fn[3], pc)
                            if key in qualified and qualified[key]['after_word'] != patched:
                                raise ValueError('conflicting receiver-derived slot mapping')
                            qualified[key] = row
                            expectations[key] = (fn, receiver)
                        else:
                            uncertain.add(pc)
                    else:
                        uncertain.add(pc)
                else:
                    uncertain.add(pc)
            if i.mnemonic in ('bl', 'blr'):
                for r in range(19):
                    after.pop('x'+str(r), None)
            successors = []
            if i.mnemonic in ('ret', 'br'):
                pass
            elif i.mnemonic == 'b':
                successors.append(ops[0].imm & ((1 << 64)-1))
            elif i.mnemonic.startswith('b.') or i.mnemonic in ('cbz','cbnz','tbz','tbnz'):
                if ops[-1].type != ARM64_OP_IMM:
                    raise ValueError('unqualified control-flow target')
                successors.extend((pc+4, ops[-1].imm & ((1 << 64)-1)))
            else:
                successors.append(pc+4)
            for succ in successors:
                if not entry <= succ < high or succ not in instructions:
                    continue
                if succ not in incoming:
                    incoming[succ] = after
                    work.append(succ)
                else:
                    common = {k:v for k,v in incoming[succ].items() if after.get(k) == v}
                    if common != incoming[succ]:
                        incoming[succ] = common
                        work.append(succ)
        # A later CFG merge can retract an earlier optimistic fact. Recheck
        # every emitted call against the FINAL fixed-point reaching state.
        for key, row in list(qualified.items()):
            if row['method_entry'] != entry:
                continue
            state = incoming.get(row['call_pc'], {})
            ins = instructions[row['call_pc']]
            fn = state.get(ins.reg_name(ins.operands[0].reg))
            if (fn, state.get('x0')) != expectations[key]:
                del qualified[key]
                uncertain.add(row['call_pc'])
    qualified_pcs = {r['call_pc'] for r in qualified.values()}
    return (sorted(qualified.values(), key=lambda r:r['load_pc']),
            sorted(uncertain-qualified_pcs), len(seeds))


def qualify_load_consumers(calls, instructions):
    """Audit the ENTIRE loaded value, not just one receiver-qualified BLR.

    A single changed LDR can feed aliases, a second call or a stored/returned
    function pointer. May-taint joins by UNION (unlike receiver must-facts).
    Unknown uses, stack spills and live values at unbounded exits fail closed.
    This proves only local consumer closure; it does NOT prove every possible
    producer/entry, parent fields, relocation or that an instruction is safe to
    apply. In particular instruction_patch_qualified stays false.
    """
    from capstone.arm64 import ARM64_OP_REG, ARM64_OP_IMM
    groups = {}
    for row in calls:
        groups.setdefault(row['load_pc'], []).append(row)

    def canonical(name):
        # A 32-bit read of a loaded pointer is also a consumer, not an alias.
        if name in ('fp', 'lr'):
            return {'fp': 'x29', 'lr': 'x30'}[name]
        return 'x'+name[1:] if name.startswith('w') else name

    plans = []
    for load_pc, rows in sorted(groups.items()):
        contexts = {(r['method_entry'], r['method_limit']) for r in rows}
        encodings = {(r['before_word'], r['after_word']) for r in rows}
        if len(contexts) != 1 or len(encodings) != 1:
            raise ValueError('one load has conflicting method/encoding contexts')
        entry, high = next(iter(contexts))
        expected = {r['call_pc'] for r in rows}
        seen, problems = set(), set()
        incoming = {load_pc: frozenset()}
        work = deque([load_pc])
        while work:
            pc = work.popleft()
            before = incoming[pc]
            i = instructions.get(pc)
            if i is None or not entry <= pc < high:
                if before:
                    problems.add((pc, 'live-value-outside-bounded-method'))
                continue
            read_ids, written_ids = i.regs_access()
            reads = {canonical(i.reg_name(r)) for r in read_ids}
            writes = {canonical(i.reg_name(r)) for r in written_ids}
            after = set(before)-writes
            used = set(before) & reads
            ops = i.operands
            alias = (i.mnemonic == 'mov' and len(ops) == 2 and
                     all(o.type == ARM64_OP_REG for o in ops) and
                     all(canonical(i.reg_name(o.reg)).startswith('x') and
                         not i.reg_name(o.reg).startswith('w') for o in ops))
            indirect = (i.mnemonic in ('blr', 'br') and len(ops) == 1 and
                        ops[0].type == ARM64_OP_REG)
            target = canonical(i.reg_name(ops[0].reg)) if indirect else None
            if pc == load_pc:
                # Re-execution in a loop generates the same changed value.
                # An earlier alias used as this load's address is NOT safe.
                if used:
                    problems.add((pc, 'loaded-function-reused-as-load-address'))
                after.add(canonical(i.reg_name(ops[0].reg)))
            elif alias:
                dst, src = (canonical(i.reg_name(o.reg)) for o in ops)
                if src in before:
                    after.add(dst)
            elif indirect and target in before:
                if pc not in expected:
                    problems.add((pc, 'unqualified-indirect-consumer'))
                else:
                    seen.add(pc)
                if used-{target}:
                    problems.add((pc, 'indirect-call-has-other-pointer-use'))
            elif used:
                problems.add((pc, 'unmodeled-pointer-use:'+i.mnemonic))
            if i.mnemonic in ('bl', 'blr') or indirect:
                # x0..x7 are argument registers even when ALSO the indirect
                # target (e.g. blr x1). Treat x8 as a possible hidden-result
                # escape unless it is this exact dispatch target: that common
                # load-x8/blr-x8 idiom cannot simultaneously supply a separate
                # result-storage pointer. Signatures still need their own ABI
                # proof; consumer closure is not that proof.
                if set(before) & {'x'+str(r) for r in range(8)} or (
                        'x8' in before and target != 'x8'):
                    problems.add((pc, 'function-pointer-passed-as-argument'))
                if i.mnemonic in ('bl', 'blr'):
                    after -= {'x'+str(r) for r in range(19)}
            successors = []
            if i.mnemonic == 'ret':
                # x0/x1 may return values. Callee-saved registers must have
                # been restored, not leak a changed load to an outer caller.
                if set(before) & ({'x0', 'x1', 'x8'} |
                                  {'x'+str(r) for r in range(19, 31)}):
                    problems.add((pc, 'function-pointer-at-return-boundary'))
            elif i.mnemonic == 'br':
                if before and (target not in before or pc not in expected):
                    problems.add((pc, 'live-value-at-unqualified-indirect-exit'))
                if set(before) & {'x'+str(r) for r in range(19, 31)}:
                    problems.add((pc, 'function-pointer-at-tail-preserved-boundary'))
            elif i.mnemonic == 'b':
                successors.append(ops[0].imm & ((1 << 64)-1))
            elif i.mnemonic.startswith('b.') or i.mnemonic in ('cbz','cbnz','tbz','tbnz'):
                if ops[-1].type != ARM64_OP_IMM:
                    raise ValueError('unqualified consumer CFG edge')
                successors.extend((pc+4, ops[-1].imm & ((1 << 64)-1)))
            elif i.mnemonic in ('svc','hvc','smc','eret','drps','brk'):
                if before:
                    problems.add((pc, 'unmodeled-exception-boundary'))
            else:
                successors.append(pc+4)
            for succ in successors:
                if not entry <= succ < high or succ not in instructions:
                    if after:
                        problems.add((pc, 'live-value-leaves-bounded-CFG'))
                    continue
                # The join must keep either path's possible loaded value.
                joined = incoming.get(succ, frozenset()) | frozenset(after)
                if succ not in incoming or joined != incoming[succ]:
                    incoming[succ] = joined
                    work.append(succ)
        for pc in expected-seen:
            problems.add((pc, 'qualified-call-not-reached-by-loaded-value'))
        qualified = bool(seen) and not problems
        for row in rows:
            row['load_consumers_qualified'] = qualified
        plans.append(dict(load_pc=load_pc, method_entry=entry, method_limit=high,
                          before_word=rows[0]['before_word'], after_word=rows[0]['after_word'],
                          qualified_indirect_consumers=sorted(seen),
                          consumer_issues=[dict(pc=pc, reason=reason) for pc,reason in sorted(problems)],
                          visited_instructions=len(incoming), load_consumers_qualified=qualified,
                          producer_entries_qualified=False, instruction_patch_qualified=False,
                          applied=False))
    return plans


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernelcache', type=Path, required=True)
    ap.add_argument('--object-root', type=Path, required=True,
                    help='frozen DEVELOPMENT_ARM64_SWEET object tree')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--require-loadable', action='store_true')
    a = ap.parse_args()
    out = a.out.resolve()
    if out.exists() or not any(out != p.resolve() and out.is_relative_to(p.resolve())
                               for p in (ROOT/'.local', ROOT/'x9/out')):
        ap.error('use a fresh private output directory')
    data = a.kernelcache.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        ap.error('requires the reviewed original D22/20H392 cache')
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
    cs = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    cs.detail = True
    base, cache, _, modules = analyze(data, a.object_root/'iokit/DEVELOPMENT/IOService.cpo', [])
    classes, instructions, other_inits = recover_classes(cs, cache, modules, base['components'])
    parents = parent_maps(cs, cache, a.object_root, classes)
    tables = templates(classes, parents)
    calls, uncertain, methods = qualify_calls(classes, parents, instructions, cache)
    loads = qualify_load_consumers(calls, instructions)
    result = dict(build='20H392', identity='d22ap', source_sha256=SHA256,
                  classes=classes, parents=parents, other_initializers=other_inits,
                  expanded_vtable_templates=tables,
                  receiver_qualified_calls=calls, visited_native_methods=methods,
                  load_consumer_plans=loads,
                  unqualified_visited_calls=uncertain,
                  abi_compatible=False, runtime_loadable=False, patches_applied=False,
                  blockers=['field-by-field parent layout/OSMetaClass allocation',
                            'unqualified producer entries/receivers, static helpers and escaped load consumers',
                            'metaclass vtables and unqualified non-IOService destructors',
                            'request timer/abort/descriptor prepare-complete ownership',
                            'MTU3 native endpoints/EP0 and original module relocation/bootstrap',
                            'native interface -> mux binding and wire DMA/Trust/Pair/TLS/Finder'])
    out.mkdir(parents=True)
    (out/'bridge-plan.json').write_text(json.dumps(result, indent=2)+'\n')
    print(f'Native classes: {len(classes)}; class-local vtable templates: {len(tables)}')
    print(f'Receiver-qualified calls: {len(calls)}; changed load plans: '
          f'{sum(r["before_word"] != r["after_word"] for r in calls)}')
    print(f'Consumer-closed loads: {sum(r["load_consumers_qualified"] for r in loads)}/{len(loads)}; '
          'producer/entry and runtime qualification still blocked')
    print('Plans only; no executable bytes modified, no runtime load-readiness claim')
    return 2 if a.require_loadable else 0


if __name__ == '__main__':
    raise SystemExit(main())
