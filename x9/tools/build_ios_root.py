#!/usr/bin/env python3
"""Build a minimal genuine iOS launchd root from one IPSW's mounted images.
Copy original executable bytes unchanged; no probe, signing or binary patches.
System launchd/dyld/xpcproxy plus standalone RestoreRamDisk dependency closure
fit the captured 512MiB X9 kernel RAM, unlike the multi-GiB full shared cache.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import posixpath
import plistlib
import shutil
import struct
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_rootprobe import raw_hfs_volume
from hfs_owners import set_catalog_owners

DEPENDENCIES = {0xc, 0x80000018, 0x8000001f, 0x80000023, 0xe}


def dependencies(blob):
    """Read dependency commands from the plain arm64 slice, without editing it."""
    start, end = 0, len(blob)
    if blob[:4] == b'\xca\xfe\xba\xbe':
        if len(blob) < 8:
            raise ValueError('truncated fat header')
        count = struct.unpack_from('>I', blob, 4)[0]
        if count > (len(blob)-8)//20:
            raise ValueError('truncated fat architecture table')
        slices = [struct.unpack_from('>5I', blob, 8+i*20) for i in range(count)]
        candidates = [x for x in slices if x[0] == 0x100000c and (x[1] & 0xffffff) == 0]
        if not candidates:
            raise ValueError('no plain arm64 slice')
        _, _, start, size, _ = candidates[0]
        end = start + size
        if end > len(blob):
            raise ValueError('fat slice exceeds file')
    if end-start < 32 or blob[start:start+4] != b'\xcf\xfa\xed\xfe':
        raise ValueError('expected arm64 Mach-O')
    _, cpu, subtype, _, count, commands, _, _ = struct.unpack_from('<8I', blob, start)
    if cpu != 0x100000c or (subtype & 0xffffff) != 0 or start+32+commands > end:
        raise ValueError('invalid arm64 Mach-O header')
    result, off = [], start+32
    for _ in range(count):
        if off+8 > start+32+commands:
            raise ValueError('truncated load command')
        cmd, size = struct.unpack_from('<II', blob, off)
        if size < 8 or off+size > start+32+commands:
            raise ValueError('invalid load command size')
        if cmd in DEPENDENCIES:
            if size < 12:
                raise ValueError('truncated dependency command')
            name = struct.unpack_from('<I', blob, off+8)[0]
            if name < 12 or name >= size:
                raise ValueError('invalid dependency string offset')
            terminator = blob.find(b'\0', off+name, off+size)
            if terminator < 0:
                raise ValueError('unterminated dependency path')
            path = blob[off+name:terminator].decode()
            if not path.startswith('/'):
                raise ValueError('nonabsolute dependency requires explicit resolution: '+path)
            result.append((path, cmd == 0x80000018))
        off += size
    if off != start+32+commands:
        raise ValueError('inconsistent load command size')
    return result


def safe_path(path):
    if not path.startswith('/') or '..' in PurePosixPath(path).parts:
        raise ValueError('unsafe root path: '+path)
    return path.lstrip('/')


SERVICES = {
    "notifyd": "/usr/sbin/notifyd",
    "cfprefsd": "/usr/sbin/cfprefsd",
    "configd": "/usr/libexec/configd",
    "syslogd": "/usr/sbin/syslogd",
}


SERVICE_JOBS = {'cfprefsd': 'com.apple.cfprefsd.xpc.daemon.plist'}


def stage_root(system, restore, target, services=()):
    target.mkdir(parents=True, exist_ok=False)
    pending = [(x, False) for x in ['/sbin/launchd', '/usr/lib/dyld', '/usr/libexec/xpcproxy']]
    pending.extend((SERVICES[name], False) for name in services)
    copied, missing_weak = {}, set()
    while pending:
        path, weak = pending.pop(0)
        rel = safe_path(path)
        if path in copied:
            continue
        src = next((root/rel for root in [system, restore] if (root/rel).exists() or (root/rel).is_symlink()), None)
        if src is None:
            if weak:
                missing_weak.add(path)
                continue
            raise FileNotFoundError('missing required iOS library: '+path)
        dst = target/rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.is_symlink():
            link = src.readlink().as_posix()
            resolved = posixpath.normpath(link if link.startswith('/') else posixpath.join(posixpath.dirname(path), link))
            safe_path(resolved)
            dst.symlink_to(link)
            copied[path] = {'source': str(src), 'symlink': link}
            pending.append((resolved, weak))
            continue
        blob = src.read_bytes()
        deps = dependencies(blob)
        shutil.copy2(src, dst)
        # Copy-time input integrity: never modify or re-sign original userland.
        digest = hashlib.sha256(blob).hexdigest()
        if hashlib.sha256(dst.read_bytes()).hexdigest() != digest:
            raise ValueError('copy changed source bytes: '+path)
        copied[path] = {'source': str(src), 'bytes': len(blob), 'sha256': digest,
                        'dependencies': [{'path': p, 'weak': w} for p,w in deps]}
        # Weak dependencies are optional loader contracts. Omit their extra
        # cryptex/framework stacks in this bounded, standalone bootstrap root.
        missing_weak.update(p for p,w in deps if w)
        pending.extend((p,w) for p,w in deps if not w)
    for rel in ['dev', 'System/Library/LaunchDaemons', 'Library/LaunchDaemons',
                'private/etc', 'private/tmp', 'private/var/tmp', 'private/var/run',
                'private/var/db', 'private/var/log', 'private/var/mobile',
                'private/var/root', 'private/var/containers', 'private/preboot',
                'System/Library/NanoLaunchDaemons', 'System/Library/NanoLaunchDaemonsAltAccount',
                'System/Library/LaunchAngels', 'System/Library/xpc',
                'private/var/preferences', 'private/var/mobile/Library/Preferences',
                'private/var/root/Library/Preferences', 'Library/Preferences']:
        (target/rel).mkdir(parents=True, exist_ok=True)
    for rel in ['private/tmp','private/var/tmp']:
        (target/rel).chmod(0o1777)
    for link, dest in [('etc','private/etc'),('var','private/var'),('tmp','private/tmp')]:
        (target/link).symlink_to(dest)
    for path in ['/private/etc/passwd','/private/etc/group','/private/etc/master.passwd',
                 '/private/etc/hosts','/private/etc/ttys',
                 '/System/Library/CoreServices/SystemVersion.plist']:
        rel = safe_path(path)
        src, dst = system/rel, target/rel
        if not src.is_file():
            raise FileNotFoundError('missing system configuration: '+path)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        copied[path] = {'source': str(src), 'bytes': src.stat().st_size,
                        'sha256': hashlib.sha256(src.read_bytes()).hexdigest()}
    if services:
        cache_path = system/'System/Library/xpc/launchd.plist'
        source_cache = plistlib.loads(cache_path.read_bytes())
        jobs = {}
        for name in services:
            path = '/System/Library/LaunchDaemons/'+SERVICE_JOBS.get(name, 'com.apple.'+name+'.plist')
            src, dst = system/safe_path(path), target/safe_path(path)
            blob = src.read_bytes()
            job = plistlib.loads(blob)
            program = job.get('Program', job.get('ProgramArguments', [''])[0])
            if program != SERVICES[name]:
                raise ValueError('unexpected official service executable: '+path)
            shutil.copy2(src, dst)
            copied[path] = {'source': str(src), 'bytes': len(blob),
                            'sha256': hashlib.sha256(blob).hexdigest()}
            # Keep Apple's original cached job entries and schema, but select
            # only jobs whose complete mandatory dependency closure we supply.
            jobs[path] = source_cache['LaunchDaemons'][path]
        selected = {'VersionNumber': source_cache['VersionNumber'], 'LaunchDaemons': jobs,
                    'AppExtensions': {}, 'AppRemovalServices': {}, 'SystemLibraryTreeState': {},
                    'Symlinks': {k:v for k,v in source_cache.get('Symlinks',{}).items()
                                 if (target/safe_path(k)).is_symlink()}}
        blob = plistlib.dumps(selected, fmt=plistlib.FMT_BINARY, sort_keys=False)
        path = '/System/Library/xpc/launchd.plist'
        (target/safe_path(path)).write_bytes(blob)
        copied[path] = {'derived_from': str(cache_path), 'selected_services': list(services),
                        'bytes': len(blob), 'sha256': hashlib.sha256(blob).hexdigest()}
    # Canonical system-file ownership, independent of the host mount's
    # noowners UID mapping. Mobile data directories separately stay 501:501.
    owners = {}
    for path,item in copied.items():
        if 'source' in item:
            owners[path] = [0, 0]
    # Do not import Apple's restore/firmware-flashing jobs into a normal boot.
    return {'files': copied, 'owners': owners, 'services': list(services), 'optional_weak_dependencies_not_included': sorted(missing_weak-set(copied)),
            'scope': 'Original iOS launchd/dyld/xpcproxy and complete required standalone dependency closure. '
                     'Selected original system services when requested; full system/apps/shared cache are not included.',
            'userland_modified': False, 'probe_included': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system-root', required=True, type=Path)
    parser.add_argument('--restore-root', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--size-mib', type=int, default=64, choices=range(16, 513), metavar='16..512')
    parser.add_argument('--service', action='append', default=[], choices=SERVICES)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out/'ios-root.hfs').exists():
        raise ValueError('refusing to overwrite existing root image')
    inventory = stage_root(args.system_root.resolve(), args.restore_root.resolve(), out/'root-tree', args.service)
    (out/'source-files.json').write_text(json.dumps(inventory, indent=2)+'\n')
    dmg = out/'ios-root-container.dmg'
    subprocess.run(['/usr/bin/hdiutil','create','-size',str(args.size_mib)+'m','-srcfolder',str(out/'root-tree'),
                    '-fs','Case-sensitive HFS+','-layout','NONE','-format','UDRW',
                    '-volname','X9iOS16Bootstrap','-nospotlight',str(dmg)],check=True)
    raw = raw_hfs_volume(dmg.read_bytes(), max_size=512*1024*1024)
    raw, owners = set_catalog_owners(raw, inventory['owners'])
    (out/'ios-root.hfs').write_bytes(raw)
    (out/'catalog-owners.json').write_text(json.dumps(owners, indent=2)+'\n')
    print('Built genuine iOS bootstrap root:',len(inventory['files']),'files;',
          sum(x.get('bytes',0) for x in inventory['files'].values()),'source bytes;',
          'raw HFS bytes',(out/'ios-root.hfs').stat().st_size,flush=True)


if __name__ == '__main__':
    main()
