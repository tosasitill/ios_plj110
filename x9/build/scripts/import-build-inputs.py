#!/usr/bin/env python3
"""Turn an extracted collect-build-inputs.py archive into a kernel/ tree.

    python3 -I import-build-inputs.py <extracted-archive-dir> <out-dir>

Writes <out-dir>/{src,generated,prebuilt,recipe.json,link-order.txt}. An
existing <out-dir> is replaced only if it holds a previous import
(recipe.json); import into a scratch directory and merge into x9/kernel by
hand when the repo tree carries its own edits.

Sources: each object's compiler depfile, else its debug info, else its
recorded command. Files named by a depfile or debug info must have one
content across all objects (a conflict stops the import). Objects without a
depfile also get the headers next to their source and in their -I
directories; those are only added when no other copy exists, and a differing
copy is reported, not used.
"""
import collections, json, re, shutil, sys
from pathlib import Path

if len(sys.argv) != 3:
    sys.exit(__doc__)
D, OUT = Path(sys.argv[1]), Path(sys.argv[2])
m = json.loads((D / 'manifest.json').read_text())
WS = m['workspace']
DXB = re.sub(r'/build/xnu\.obj/.*', '', m['xnu']['object_root'])
DEV = next(re.sub(r'/Toolchains/.*', '', t) for t in m['host']['tools'] if '/Toolchains/' in t)
if OUT.exists() and not (OUT / 'recipe.json').is_file():
    sys.exit(f'refusing to replace {OUT}: it is not a previous import (no recipe.json)')
if OUT.resolve() == D.resolve() or D.resolve() in OUT.resolve().parents:
    sys.exit('output must not be inside the input archive')

SRC_MAP = [
    (re.escape(WS) + r'/x9/out/[^/]+/kernel-source', '{SRC}'),
    (re.escape(WS) + r'/x9/out/[^/]+/reconstructed-inputs', '{SRC}'),
    (re.escape(WS) + r'/work/x9-usb-live/recover-build', '{SRC}'),
    (re.escape(WS) + r'/MaliMetal/X9/General/kernel', '{SRC}/iokit/Kernel/arm'),
    (re.escape(WS) + r'/work/x9-battery/source', '{SRC}/iokit/Kernel/arm'),
    (re.escape(WS) + r'/x9/out/[^/]+/generated', '{GEN}'),
    # ccache-bin/clang{,++} only caches the same Xcode compiler.
    (re.escape(DXB) + r'/ccache-bin/clang', '{DEVELOPER_DIR}/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'),
    (re.escape(DXB), '{DXB}'),
    (re.escape(DEV), '{DEVELOPER_DIR}'),
]


def norm(s):
    for pat, rep in SRC_MAP:
        s = re.sub(pat, rep, s)
    return s


def local(p):
    return D / 'files' / p.lstrip('/')


# ---- choose the real source of every overlay object -------------------------
objects, files, speculative = [], {}, []
names = collections.Counter(Path(o['object']).name for o in m['objects'])
assert max(names.values()) == 1, names.most_common(3)
for o in m['objects']:
    name = Path(o['object']).name
    dwarf_src = [p for p in o.get('dwarf_sources', []) if re.search(r'\.(c|cpp|s|S)$', p)]
    cmd_src = o['sources'][-1:] if o['sources'] else []
    if o.get('depfile_source') and o['depfile_source'].startswith(WS):
        src = o['depfile_source']
    elif dwarf_src:
        assert len(dwarf_src) == 1, (name, dwarf_src)
        src = dwarf_src[0]
    elif cmd_src:
        src = cmd_src[0]
    else:
        sys.exit(f'no source for {name}')
    deps = set(o['workspace_deps']) | set(o.get('dwarf_sources', [])) | {src}
    for p in deps:
        rel = norm(p)
        assert rel.startswith(('{SRC}/', '{GEN}/')), p
        data = local(p).read_bytes()
        if rel in files and files[rel][1] != data:
            sys.exit(f'conflicting contents for {rel}: {files[rel][0]} vs {p}')
        files[rel] = (p, data)
    for d in o.get('header_dirs', []):
        ld = local(d)
        for h in sorted(ld.glob('*')) if ld.is_dir() else []:
            if h.suffix in ('.h', '.hpp', '.inc') and h.is_file():
                speculative.append((d + '/' + h.name, h.read_bytes()))
    objects.append({'name': name, 'src': norm(src), 'cmd': o['command'], 'orig': o['object'],
                    'sha256': o.get('object_sha256')})

for p, data in speculative:
    rel = norm(p)
    if not rel.startswith(('{SRC}/', '{GEN}/')):
        continue
    if rel not in files:
        files[rel] = (p, data)
    elif files[rel][1] != data:
        print(f'note: directory header {p} differs from {files[rel][0]}; kept the latter')

# ---- compile templates -------------------------------------------------------
def normalized_args(o):
    a, cwd = o['cmd']['args'], o['cmd']['cwd']
    out, i = [], 1
    while i < len(a):
        x = a[i]
        if x in ('-o', '-MF', '-MT'):
            i += 2
            continue
        if x in ('-c',) or (re.search(r'\.(c|cpp|s|S)$', x) and not x.startswith('-')):
            i += 1
            continue
        if x == '-MD' or x == '-MP':
            i += 1
            continue
        out.append(norm(x))
        i += 1
    sub = re.search(r'DEVELOPMENT_ARM64_SWEET/([^/]+)', cwd).group(1)
    return a[0], norm(cwd), sub, out


groups = collections.defaultdict(list)
for o in objects:
    if not o['cmd']:
        continue
    exe, cwd, sub, args = normalized_args(o)
    lang = 'cxx' if exe.endswith('++') else 'c'
    o.update(exe=norm(exe), cwd=cwd, sub=sub, lang=lang, args=args)
    groups[(sub, lang)].append(o)

# Exact argument lists are kept (include order matters); identical lists
# share one template, named <subsystem>-<lang>-<n>.
templates, seen = {}, {}
for (sub, lang), objs in sorted(groups.items()):
    for o in objs:
        key = (o['exe'], o['cwd'], tuple(o['args']))
        if key not in seen:
            n = sum(1 for k in seen if k[1] == o['cwd'] and k[0] == o['exe']) + 1
            seen[key] = f'{sub}-{lang}-{n}'
            templates[seen[key]] = {'compiler': o['exe'], 'cwd': o['cwd'], 'args': list(o['args'])}
        o['entry'] = {'template': seen[key]}

# ---- write --------------------------------------------------------------------
if OUT.exists():
    shutil.rmtree(OUT)
for rel, (orig, data) in files.items():
    dest = OUT / ('src' + rel[5:] if rel.startswith('{SRC}') else 'generated' + rel[5:])
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)

prebuilt = OUT / 'prebuilt'
entries = []
for o in objects:
    e = {'name': o['name'], 'source': o['src'].replace('{SRC}/', '')}
    if o.get('entry'):
        e.update(o['entry'])
    else:
        # Source known (DWARF/depfile) but no recorded command: link the
        # pan220 object as-is until a command is reconstructed.
        prebuilt.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(local(o['orig']), prebuilt / o['name'])
        e['prebuilt'] = f"prebuilt/{o['name']}"
        e['prebuilt_sha256'] = o['sha256']
    entries.append(e)

link_records = json.loads((D / 'candidate/commands.json').read_text())
link = next(r for r in link_records if '-filelist' in r['args'])
strip = link_records[-1]
filelist = (D / 'candidate/kernel-link.filelist').read_text().split()
obj_root = re.sub(r'(/xnu\.obj/[^/]+)/.*', r'\1', next(p for p in filelist if '/xnu.obj/' in p))
order = []
for p in filelist:
    if '/xnu.obj/' in p:
        order.append('xnu:' + p[len(obj_root) + 1:])
    else:
        order.append('x9:' + Path(p).name)
largs = [norm(x) for x in link['args']]
fi = largs.index('-filelist'); largs[fi + 1] = '{FILELIST}'
oi = largs.index('-o'); largs[oi + 1] = '{UNSTRIPPED}'
recipe = {
    'schema': 1,
    'imported_from': f"{m['candidate']} (collect-build-inputs.py, {m['host']['date'][:10]})",
    'xnu_object_tree': '{DXB}/build/xnu.obj/DEVELOPMENT_ARM64_SWEET',
    'templates': templates,
    'objects': entries,
    'link': {'compiler': largs[0], 'cwd': norm(link['cwd']), 'args': largs[1:]},
    'strip': {'tool': norm(strip['args'][0]), 'args': ['-S']},
}
(OUT / 'recipe.json').write_text(json.dumps(recipe, indent=1) + '\n')
(OUT / 'link-order.txt').write_text('\n'.join(order) + '\n')
print(f'{len(files)} source files, {len(entries)} objects, {sum(1 for e in entries if "prebuilt" in e)} prebuilt, '
      f'templates {sorted(templates)}, link order {len(order)}')
for t in sorted(templates):
    print(t, [e['name'] for e in entries if e.get('template') == t])
