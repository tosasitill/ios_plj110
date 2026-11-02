#!/usr/bin/env python3
"""Fail when a layer reaches past the native backend boundary.

Task 4 makes the PanVK <-> XNU boundary explicit:

  * PanVK (Mesa) submits only through the X9 backend (`x9_pan_kmod_*`); it does
    not include IOKit, IOSurface, display services, XNU VM or libkern headers.
  * The X9 integration sources under x9/metal/General/panvk are the only ones
    that may name the native transport.
  * The Metal API layer (factory, frontend, .inc) names neither IOKit nor the
    transport; bridge/NativeBackendProbe.mm is the single discovery seam.

This check reads sources only. It does not build, load or execute anything, and
it does not scan transitively included headers; a direct include is the thing
that lets a later edit start using the facility.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PANVK_MESA = ROOT / '.local/x9/work/x9-metal-render/mesa-render-source/src/panfrost/vulkan'
PANVK_X9 = ROOT / 'x9/metal/General/panvk'
METAL = ROOT / 'x9/metal/General'
TRANSPORT = METAL / 'X9RenderTransport.c'
PROBE = METAL / 'bridge/NativeBackendProbe.mm'

INCLUDE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^>"]+)[>"]', re.M)
SYMBOL = re.compile(r'\bx9_render_[a-z0-9_]+\b')
CLASSES = {
    'iokit': re.compile(r'(^|/)IOKit/'),
    'iosurface': re.compile(r'(^|/)IOSurface/'),
    'display': re.compile(r'(^|/)(IOMobileFramebuffer|IOGPU|IOGraphics)/'),
    'xnu_vm': re.compile(r'(^|/)mach/(mach_vm|vm_map)\.h$|(^|/)libkern/'),
    'native_transport': re.compile(r'(^|/)X9RenderTransport\.h$'),
}
SOURCES = ('.c', '.h', '.inc', '.m', '.mm', '.cpp', '.rs')

# None means "anywhere in this mode"; a tuple lists the files that may use it.
POLICY = {
    'panvk': {
        'roots': (PANVK_MESA, PANVK_X9),
        'exclude': (),
        'include_allow': {
            'iokit': (),
            'iosurface': (),
            'display': (),
            'xnu_vm': (),
            'native_transport': (PANVK_X9,),
        },
        'symbol_allow': (PANVK_X9,),
    },
    'metal': {
        'roots': (METAL,),
        # probed tool and the uncompiled legacy resource helper are not part of
        # X9GeneralMetal<N>.bundle and may keep using the transport directly.
        'exclude': (PANVK_X9, METAL / 'probe',
                    METAL / 'X9RenderResources.c', METAL / 'X9RenderResources.h'),
        'include_allow': {
            'iokit': (TRANSPORT, PROBE),
            'iosurface': None,
            'display': (),
            'xnu_vm': (),
            'native_transport': (TRANSPORT, PROBE),
        },
        'symbol_allow': (TRANSPORT, PROBE, METAL / 'X9RenderTransport.h',
                         METAL / 'bridge/NativeMetalBridge.rs',
                         METAL / 'bridge/NativeMetalObjects.rs'),
    },
}


def inside(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def allowed(path, allow):
    if allow is None:
        return True
    return any(inside(path, entry) if entry.is_dir() else path == entry for entry in allow)


def sources(mode):
    policy = POLICY[mode]
    seen, files = set(), []
    for root in policy['roots']:
        if not root.is_dir():
            print(f'note: {root.relative_to(ROOT)} is absent; nothing to scan', flush=True)
            continue
        for path in sorted(root.rglob('*')):
            if not path.is_file() or path.suffix not in SOURCES or path in seen:
                continue
            if any(inside(path, skip) for skip in policy['exclude']):
                continue
            seen.add(path)
            files.append(path)
    return files


def scan(mode):
    policy = POLICY[mode]
    problems = []
    for path in sources(mode):
        shown = path.relative_to(ROOT)
        text = path.read_text(errors='replace')
        for line_no, line in enumerate(text.splitlines(), 1):
            match = INCLUDE.match(line)
            if match:
                header = match.group(1)
                for name, pattern in CLASSES.items():
                    if pattern.search(header) and not allowed(path, policy['include_allow'][name]):
                        problems.append(f'{shown}:{line_no}: includes {header} ({name})')
            if not allowed(path, policy['symbol_allow']) and SYMBOL.search(line):
                problems.append(f'{shown}:{line_no}: names a native transport symbol')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument('--panvk', action='store_true', help='PanVK core + X9 integration')
    group.add_argument('--metal', action='store_true', help='Metal API layer + bridge')
    mode = 'panvk' if ap.parse_args().panvk else 'metal'
    problems = scan(mode)
    if problems:
        print(f'native boundary violated ({mode}):', file=sys.stderr)
        for problem in problems:
            print('  ' + problem, file=sys.stderr)
        return 1
    print(f'native boundary clean ({mode}): {len(sources(mode))} sources', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
