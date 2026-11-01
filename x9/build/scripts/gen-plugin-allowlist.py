#!/usr/bin/env python3
"""Generate the kernel's Metal/PanVK plugin allowlist from a plugin package.

    python3 -I x9/build/scripts/gen-plugin-allowlist.py <plugin-package>/mailbox.json

Writes x9/kernel/generated/x9_render_external_manifest.h: the file allowlist
(at boot the kernel checks every listed file's size and SHA-256 on the iOS
root and enables Metal discovery only if all match) and the mailbox update
identity and directories (x9_update_install.inc). Regenerate it whenever new
plugin bundles are packaged (e.g. X9GeneralMetal207 -> 208), then rebuild.
"""
import json
import re
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / 'kernel/generated/x9_render_external_manifest.h'


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    manifest = json.loads(Path(sys.argv[1]).read_text())
    files = manifest['files']
    identity = manifest.get('identity', '')
    if not re.fullmatch(r'[0-9a-f]{64}', identity):
        sys.exit('mailbox.json needs a 64-hex "identity" (use the package\'s mailbox.json, not manifest.json)')
    bundles = set()
    lines = []
    for f in files:
        path, sha = f['path'], f['sha256']
        if not re.fullmatch(r'[0-9a-f]{64}', sha) or not path.startswith('/') or '"' in path or '\\' in path:
            sys.exit(f'bad manifest entry: {f}')
        m = re.match(r'/System/Library/Extensions/(X9GeneralMetal\d+)\.bundle/', path)
        if m:
            bundles.add(m.group(1))
        digest = ','.join(f'0x{sha[i:i + 2]}' for i in range(0, 64, 2))
        lines.append(f'{{"{path}",{int(f["bytes"])},{{{digest}}},{int(f["offset"])},{int(f["mode"])}}},')
    # The kernel derives the Metal plugin name from exactly one versioned
    # X9GeneralMetal<N>.bundle/Info.plist in this list (patch 0254).
    if len(bundles) != 1:
        sys.exit(f'expected exactly one X9GeneralMetal<N> bundle, found {sorted(bundles)}')
    # Parent directories of the files: bundles first, then deeper ones.
    dirs = sorted({str(Path(f['path']).parent) for f in files}, key=lambda d: (d.count('/'), d))
    ident = ','.join(f'0x{identity[i:i + 2]}' for i in range(0, 64, 2))
    OUT.write_text('/* Exact independent plugin package; no executable bytes in kernel. */\n'
                   '#pragma once\n'
                   'struct X9ExternalFile {const char *path;uint64_t bytes;unsigned char sha256[32];uint64_t offset;int mode;};\n'
                   'static const struct X9ExternalFile x9_external_files[]={\n' + '\n'.join(lines) + '\n};\n'
                   f'static const unsigned char x9_update_identity[32]={{{ident}}};\n'
                   'static const char *const x9_update_dirs[]={\n' + '\n'.join(f'"{d}",' for d in dirs) + '\n};\n')
    print(f'{OUT}: {len(files)} files, plugin {bundles.pop()}')


if __name__ == '__main__':
    main()
