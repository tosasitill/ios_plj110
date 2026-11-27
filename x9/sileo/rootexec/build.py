#!/usr/bin/env python3
"""Build x9-rootexec + giveMeRoot for arm64-apple-ios16.0.

x9-rootexec's caller table comes from callers.json (the Sileo app; its
hash stays unpinned/null until the Sileo packaging step supplies the real
one -- fail closed until then). giveMeRoot is an unprivileged translator.
Also compiles and runs the host policy unit tests. Never touches a device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
TARGET = 'arm64-apple-ios16.0'


def run(cmd, **kw):
    print('+', ' '.join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--developer-dir', type=Path, required=True)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    dev = a.developer_dir
    sdk = dev / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
    clang = dev / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    cross = [str(clang), '-target', TARGET, '-isysroot', str(sdk),
             '-O2', '-Wall', '-Wextra']

    host_sdk = '/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk'
    run(['/usr/bin/clang', '-isysroot', host_sdk, '-Wall', '-Wextra',
         HERE / 'test_policy.c', '-o', out / 'test_policy'])
    run([out / 'test_policy'])

    callers = json.loads((HERE / 'callers.json').read_text())

    def table(name, entries):
        rows = ',\n    '.join(
            '{ "%s", %s }' % (e['path'],
                              '"%s"' % e['sha256'] if e['sha256'] else 'NULL')
            for e in entries)
        return f'static const x9_caller_entry {name}[] = {{\n    {rows}\n}};\n'

    gen = out / 'rootexec_callers.h'
    gen.write_text(table('X9_ROOTEXEC_CALLERS', callers['rootexec_callers']))
    run(cross + [HERE / 'giveMeRoot.c', '-o', out / 'giveMeRoot'])
    run(['codesign', '-s', '-', '-f', out / 'giveMeRoot'])
    run(cross + [HERE / 'x9-rootexec.c', '-I', out, '-o', out / 'x9-rootexec'])
    run(['codesign', '-s', '-', '-f', out / 'x9-rootexec'])

    files = {}
    for name, mode in (('giveMeRoot', '0755'), ('x9-rootexec', '4755')):
        data = (out / name).read_bytes()
        files[('/var/storm/Applications/Sileo.app/' + name) if name == 'giveMeRoot' else ('/usr/bin/' + name)] = {
            'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'mode': mode}
    manifest = {'target': TARGET, 'files': files,
                'notes': ['x9-rootexec must be installed mode 4755 root:wheel; '
                          'whether the setuid bit is effective on the root '
                          'volume is on-device discriminator 3 (README).',
                          'Sileo app caller is unpinned until the Sileo '
                          'packaging step pins its SHA256.']}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('rootexec manifest:', json.dumps(files, indent=1)[:200])


if __name__ == '__main__':
    main()
