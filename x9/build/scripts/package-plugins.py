#!/usr/bin/env python3
"""Package built Metal/PanVK bundles for the mailbox and the kernel allowlist.

    python3 -I x9/build/scripts/package-plugins.py --revision N --out x9/out/<name>
    python3 -I x9/build/scripts/package-plugins.py --from-mailbox <plugin-package>/mailbox.json

Reads X9GeneralMetal<N>.bundle and X9PanVK<N>.bundle from the user-space work
directory, copies them into <out>/plugin-package/ and writes there:
manifest.json, mailbox.json (identity, per-file offsets, payload_end),
SHA256SUMS and install-on-ios.sh. It also rewrites the kernel allowlist
x9/kernel/generated/x9_render_external_manifest.h, so the next kernel build
accepts exactly these files. Layout and identity are those of the pan171-174
packager (recovered from git history): files 4 KiB aligned from offset 4096,
mode 0755 for the two executables, identity = SHA-256 of the sorted-key
compact JSON file list. No device access; mailbox/update.py stages it.
--from-mailbox only rewrites the kernel allowlist from an existing package
(e.g. to return to an installed revision).

    python3 -I x9/build/scripts/package-plugins.py --base-package x9/out/<old>/plugin-package \
        --extra-root .local/x9/appsync/runtime-NEW/root --out x9/out/<name>

reuses an existing package byte for byte (plugin rebuilds are not reproducible,
and the installed revision must keep matching) and appends every file under
--extra-root at its absolute path (e.g. the AppSync runtime and
/Applications/X9Installer.app). Mode 0755 for Mach-O files, 0644 otherwise.
The kernel creates every listed ancestor directory (mkdir is one level) and
never replaces an existing file that differs.
"""
import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / '.local/x9/work/x9-metal-render'
MAILBOX = ROOT / 'x9/mailbox/mailbox-manifest.json'
HEADER = ROOT / 'x9/kernel/generated/x9_render_external_manifest.h'
EXTENSIONS = '/System/Library/Extensions/'


def package(revision, out, work=WORK):
    bundles = [f'X9GeneralMetal{revision}.bundle', f'X9PanVK{revision}.bundle']
    for name in bundles:
        if not (work / name).is_dir():
            sys.exit(f'missing {work / name}: build the plugins first')
    pkg = out / 'plugin-package'
    if pkg.exists():
        shutil.rmtree(pkg)
    pkg.mkdir(parents=True)
    files = []
    for name in bundles:
        shutil.copytree(work / name, pkg / name, symlinks=False)
        for p in sorted((pkg / name).rglob('*')):
            if p.is_file():
                files.append({'path': EXTENSIONS + name + '/' + str(p.relative_to(pkg / name)),
                              'source': str(p.relative_to(pkg)), 'bytes': p.stat().st_size,
                              'sha256': hashlib.sha256(p.read_bytes()).hexdigest()})
    offset = 4096
    for f in files:
        f['offset'] = offset
        f['mode'] = 0o755 if f['source'].endswith(('/X9GeneralMetal', '/X9PanVK')) else 0o644
        offset = (offset + f['bytes'] + 4095) & ~4095
    capacity = json.loads(MAILBOX.read_text())['logical_blocks'] * 4096
    if offset > capacity:
        sys.exit(f'payload {offset} bytes exceeds the mailbox ({capacity} bytes)')
    identity = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    (pkg / 'mailbox.json').write_text(json.dumps({'identity': identity, 'files': files, 'payload_end': offset}, indent=2) + '\n')
    (pkg / 'manifest.json').write_text(json.dumps({'files': files,
        'destination': 'iOS writable native HFS root; never Android F2FS plaintext view',
        'no_probe_or_test_program': True}, indent=2) + '\n')
    (pkg / 'SHA256SUMS').write_text(''.join(f"{f['sha256']}  {f['source']}\n" for f in files))
    names = ' '.join(bundles)
    installer = f'''#!/bin/sh
# Run as root on the mounted iOS root volume, from this extracted package.
# Creates only two versioned plugin directories; never changes launchd.
set -eu
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || {{ echo "root required" >&2; exit 1; }}
command -v shasum >/dev/null || {{ echo "shasum required" >&2; exit 1; }}
DEST=/System/Library/Extensions
[ -d "$DEST" ] && [ -w "$DEST" ] || {{ echo "iOS Extensions directory unavailable or not writable" >&2; exit 1; }}
for name in {names}; do
 [ ! -e "$DEST/$name" ] && [ ! -L "$DEST/$name" ] || {{ echo "existing destination preserved: $DEST/$name" >&2; exit 1; }}
done
shasum -a 256 -c SHA256SUMS
for name in {names}; do
 stage="$DEST/.$name.installing"
 [ ! -e "$stage" ] && [ ! -L "$stage" ] || {{ echo "staging conflict preserved: $stage" >&2; exit 1; }}
 mkdir "$stage"
 cp -Rp "$name/." "$stage/"
 chmod 755 "$stage"
 [ ! -e "$DEST/$name" ] && [ ! -L "$DEST/$name" ] || {{ echo "destination appeared: $DEST/$name" >&2; exit 1; }}
 mv "$stage" "$DEST/$name"
done
sync
echo "Installed versioned Metal and PanVK bundles. Restart XNU to verify exact payload and enable discovery."
'''
    (pkg / 'install-on-ios.sh').write_text(installer)
    (pkg / 'install-on-ios.sh').chmod(0o755)
    write_header(files, identity)
    return pkg, len(files), offset


def layout(pkg, files):
    offset = 4096
    for f in files:
        f['offset'] = offset
        offset = (offset + f['bytes'] + 4095) & ~4095
    capacity = json.loads(MAILBOX.read_text())['logical_blocks'] * 4096
    if offset > capacity:
        sys.exit(f'payload {offset} bytes exceeds the mailbox ({capacity} bytes)')
    identity = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    (pkg / 'mailbox.json').write_text(json.dumps({'identity': identity, 'files': files, 'payload_end': offset}, indent=2) + '\n')
    (pkg / 'manifest.json').write_text(json.dumps({'files': files,
        'destination': 'iOS writable native HFS root; never Android F2FS plaintext view',
        'no_probe_or_test_program': True}, indent=2) + '\n')
    (pkg / 'SHA256SUMS').write_text(''.join(f"{f['sha256']}  {f['source']}\n" for f in files))
    write_header(files, identity)
    return offset


def package_extra(base, extra, out):
    desc = json.loads((base / 'mailbox.json').read_text())
    pkg = out / 'plugin-package'
    if pkg.exists():
        shutil.rmtree(pkg)
    pkg.mkdir(parents=True)
    files = []
    for f in desc['files']:
        src = base / f['source']
        data = src.read_bytes()
        if len(data) != f['bytes'] or hashlib.sha256(data).hexdigest() != f['sha256']:
            sys.exit(f'{src} does not match {base}/mailbox.json')
        (pkg / f['source']).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, pkg / f['source'])
        files.append({k: f[k] for k in ('path', 'source', 'bytes', 'sha256', 'mode')})
    known = {f['path'] for f in files}
    for p in sorted(extra.rglob('*')):
        if p.is_symlink():
            sys.exit(f'symlink not supported: {p}')
        if not p.is_file():
            continue
        path = '/' + p.relative_to(extra).as_posix()
        if path in known or len(path) > 400:
            sys.exit(f'bad or duplicate path {path}')
        data = p.read_bytes()
        source = 'extra' + path
        (pkg / source).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, pkg / source)
        files.append({'path': path, 'source': source, 'bytes': len(data),
                      'sha256': hashlib.sha256(data).hexdigest(),
                      'mode': 0o755 if data[:4] == b'\xcf\xfa\xed\xfe' else 0o644})
    end = layout(pkg, files)
    return pkg, len(files), end


def write_header(files, identity):
    def digest(hexstr):
        return ','.join('0x' + hexstr[i:i + 2] for i in range(0, 64, 2))
    lines = ['/* Exact independent plugin package; no executable bytes in kernel. */', '#pragma once',
             'struct X9ExternalFile {const char *path;uint64_t bytes;unsigned char sha256[32];uint64_t offset;int mode;};',
             'static const struct X9ExternalFile x9_external_files[]={']
    lines += [f'{{{json.dumps(f["path"])},{f["bytes"]},{{{digest(f["sha256"])}}},{f["offset"]},{f["mode"]}}},' for f in files]
    dirs = sorted({str(a) for f in files for a in Path(f['path']).parents if str(a) != '/'},
                  key=lambda d: (d.count('/'), d))
    lines += ['};', f'static const unsigned char x9_update_identity[32]={{{digest(identity)}}};',
              'static const char *const x9_update_dirs[]={'] + [json.dumps(d) + ',' for d in dirs] + ['};']
    HEADER.write_text('\n'.join(lines) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--revision', type=int)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--work', type=Path, default=WORK, help='directory holding the built bundles')
    ap.add_argument('--from-mailbox', type=Path, help="only regenerate the allowlist from this package's mailbox.json")
    ap.add_argument('--base-package', type=Path, help='existing plugin-package to reuse byte for byte')
    ap.add_argument('--extra-root', type=Path, help='directory tree appended at absolute paths')
    a = ap.parse_args()
    if a.base_package or a.extra_root:
        if not (a.base_package and a.extra_root and a.out):
            ap.error('--base-package, --extra-root and --out go together')
        pkg, count, end = package_extra(a.base_package.resolve(), a.extra_root.resolve(), a.out.resolve())
        print(f'{pkg}: {count} files, payload {end} bytes; kernel allowlist {HEADER} updated')
        return
    if a.from_mailbox:
        desc = json.loads(a.from_mailbox.read_text())
        write_header(desc['files'], desc['identity'])
        print(f'{HEADER}: {len(desc["files"])} files from {a.from_mailbox}')
        return
    if a.revision is None or a.out is None:
        ap.error('--revision and --out are required unless --from-mailbox is given')
    pkg, count, end = package(a.revision, a.out.resolve(), a.work.resolve())
    print(f'{pkg}: {count} files, payload {end} bytes; kernel allowlist {HEADER} updated')


if __name__ == '__main__':
    main()
