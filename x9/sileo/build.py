#!/usr/bin/env python3
"""Build the X9 storm bootstrap subset; never mount, stage or modify a device.

Downloads the pinned sources in sources.json (SHA256 verified, content-
addressed cache under .local/x9/sileo-cache), cross-compiles for
arm64-apple-ios16.0 with the public prefix /var/storm, assembles a
prefix-relative bootstrap tree and emits a runtime.json-style manifest.

Built here: libmd, liblzma (xz), dash (as sh), dpkg, uicache.
Pinned but NOT built (dependency trees pending, see sources.json):
apt, coreutils. This script refuses to emit a manifest claiming them.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PREFIX = '/var/storm'
TARGET = 'arm64-apple-ios16.0'
SOURCES = json.loads((HERE / 'sources.json').read_text())
CACHE = ROOT / '.local/x9/sileo-cache'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def fetch(key):
    spec = SOURCES[key]
    url, want = spec['url'], spec['sha256']
    name = url.rsplit('/', 1)[-1]
    slot = CACHE / want
    slot.mkdir(parents=True, exist_ok=True)
    path = slot / name
    if not path.is_file() or sha256(path) != want:
        print(f'+ download {url}')
        req = urllib.request.Request(url, headers={'User-Agent': 'x9-sileo-build'})
        with urllib.request.urlopen(req, timeout=300) as r, open(path, 'wb') as f:
            shutil.copyfileobj(r, f)
        if sha256(path) != want:
            raise ValueError(f'{key}: sha256 mismatch after download')
    return path


def extract(key, work):
    tar = fetch(key)
    dest = work / key
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with tarfile.open(tar) as t:
        # sources are SHA256-pinned in sources.json, so links are trusted
        t.extractall(dest, filter='fully_trusted')
    kids = [p for p in dest.iterdir() if p.is_dir()]
    return kids[0] if len(kids) == 1 else dest


class Builder:
    def __init__(self, out, developer):
        self.out = out
        self.work = out / 'work'
        self.sysroot = self.work / 'sysroot'          # DESTDIR installs, prefix /var/storm
        self.sdk = developer / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
        self.clang = developer / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
        self.host_clang = '/usr/bin/clang'
        inc = self.sysroot / PREFIX.strip('/') / 'usr/include'
        lib = self.sysroot / PREFIX.strip('/') / 'usr/lib'
        self.env = dict(os.environ)
        self.env.update({
            'CC': f'{self.clang} -target {TARGET} -isysroot {self.sdk}',
            'CXX': f'{self.clang}++ -target {TARGET} -isysroot {self.sdk}',
            'CC_FOR_BUILD': self.host_clang,
            'CFLAGS': f'-O2 -I{inc}',
            'CXXFLAGS': f'-O2 -I{inc}',
            'LDFLAGS': f'-L{lib}',
            'MACOSX_DEPLOYMENT_TARGET': '',
            'ZERO_AR_DATE': '1',
        })

    def run(self, command, cwd=None, env=None):
        print('+', ' '.join(map(str, command)), flush=True)
        subprocess.run(list(map(str, command)), cwd=cwd,
                       env=env or self.env, check=True)

    def configure_make(self, src, args, make_targets=('all',), jobs=8,
                       host='arm64-apple-darwin'):
        self.run([src / 'configure', '--build=x86_64-apple-darwin',
                  f'--host={host}', f'--prefix={PREFIX}/usr'] + args,
                 cwd=src)
        for t in make_targets:
            self.run(['make', f'-j{jobs}', t], cwd=src)
        self.run(['make', 'install', f'DESTDIR={self.sysroot}'], cwd=src)

    def build_libmd(self):
        src = extract('libmd', self.work)
        self.configure_make(src, ['--disable-silent-rules'])

    def build_liblzma(self):
        src = extract('xz', self.work)
        self.configure_make(src, ['--disable-silent-rules', '--disable-xz',
                                  '--disable-xzdec', '--disable-lzmadec',
                                  '--disable-lzmainfo', '--disable-scripts',
                                  '--enable-shared', '--disable-static'])

    def build_dash(self):
        src = extract('dash', self.work)
        self.configure_make(src, ['--disable-silent-rules'])
        bindir = self.sysroot / PREFIX.strip('/') / 'usr/bin'
        (bindir / 'sh').unlink(missing_ok=True)
        shutil.copy2(bindir / 'dash', bindir / 'sh')

    def build_dpkg(self):
        src = extract('dpkg', self.work)
        recipes = extract('procursus_recipes', self.work)
        for group in ('dpkg', 'dpkg-ios'):
            pdir = recipes / 'build_patch' / group
            for patch in sorted(pdir.iterdir()):
                if patch.is_file():
                    print(f'+ patch {group}/{patch.name}')
                    with open(patch, 'rb') as f:
                        subprocess.run(['patch', '-p1', '-f'], cwd=src,
                                       stdin=f, check=True)
        # Same iOS SDK system() problem as GNU patch, one call site:
        # dpkg's --preinvoke/--postinvoke hook runner (src/main/main.c).
        main_c = src / 'src/main/main.c'
        text = main_c.read_text()
        needle = '    status = system(hook->command);'
        assert text.count(needle) == 1
        helper = '''    /* x9: system() is unavailable in the iOS SDK; equivalent via sh. */
    {
      pid_t pid = fork();
      if (pid == 0)
        {
          execl ("/var/storm/usr/bin/sh", "sh", "-c", hook->command,
                 (char *) 0);
          _exit (127);
        }
      if (pid < 0) status = -1;
      else
        {
          int st = -1;
          while (waitpid (pid, &st, 0) < 0 && errno == EINTR)
            ;
          status = st;
        }
    }'''
        main_c.write_text(text.replace(needle, helper))
        inc = self.sysroot / PREFIX.strip('/') / 'usr/include'
        lib = self.sysroot / PREFIX.strip('/') / 'usr/lib'
        env = dict(self.env)
        env.update({
            'LIBMD_CFLAGS': f'-I{inc}', 'LIBMD_LIBS': f'-L{lib} -lmd',
            'LIBLZMA_CFLAGS': f'-I{inc}', 'LIBLZMA_LIBS': f'-L{lib} -llzma',
            'PERL': '/usr/bin/perl',        })
        self.run([src / 'configure', '--build=x86_64-apple-darwin',
                  '--host=arm64-apple-darwin', f'--prefix={PREFIX}/usr',
                  f'--sysconfdir={PREFIX}/etc',
                  f'--with-admindir={PREFIX}/Library/dpkg',
                  f'--with-logdir={PREFIX}/var/log',
                  '--disable-nls', '--disable-dselect',
                  '--disable-start-stop-daemon', '--without-libselinux',
                  '--without-libzstd', '--without-libbz2',
                  '--disable-shared', '--disable-silent-rules'],
                 cwd=src, env=env)
        self.run(['make', '-j8'], cwd=src, env=env)
        self.run(['make', 'install', f'DESTDIR={self.sysroot}'], cwd=src, env=env)

    def build_uicache(self):
        src = extract('uikittools-ng', self.work)
        # The public SDK has no MobileContainerManager stub; uicache needs
        # exactly two ObjC classes from it (verified against the 20H392 DSC
        # extraction, which ld rejects as a link input). Emit a minimal tbd.
        tbd = self.work / 'MobileContainerManager.tbd'
        tbd.write_text('''--- !tapi-tbd
tbd-version: 4
targets: [ arm64-ios ]
install-name: /System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager
exports:
  - targets: [ arm64-ios ]
    objc-classes: [ MCMSharedDataContainer, MCMSystemDataContainer ]
...
''')
        out = self.sysroot / PREFIX.strip('/') / 'usr/bin/uicache'
        out.parent.mkdir(parents=True, exist_ok=True)
        self.run([self.clang, '-target', TARGET, '-isysroot', self.sdk,
                  '-O2', '-fobjc-arc', '-Wall', '-Wextra',
                  # uicache.m defaults APP_PATH to /var/jb/Applications
                  # behind #ifndef; point it at the storm app dir.
                  '-DAPP_PATH=@"/var/storm/Applications"',
                  '-framework', 'Foundation', '-framework', 'CoreServices',
                  '-framework', 'Security',
                  src / 'uicache.m', tbd, '-o', out])
        plist = src / 'uicache.plist'
        if plist.is_file():
            self.run(['codesign', '-s', '-', '-f', '--entitlements', plist, out])

    def build_patch(self):
        src = extract('patch', self.work)
        # iOS SDK marks system() unavailable; GNU patch has exactly one
        # call site (systemic(), for ed scripts / PATCH_GET). Replace it
        # with an equivalent fork/exec of the storm prefix shell.
        util = src / 'src/util.c'
        text = util.read_text()
        needle = '  return system (command);'
        assert text.count(needle) == 1
        helper = '''  /* x9: system() is unavailable in the iOS SDK; equivalent via sh. */
  {
    extern char **environ;
    pid_t pid = fork ();
    if (pid == 0)
      {
        execl ("/var/storm/usr/bin/sh", "sh", "-c", command, (char *) 0);
        _exit (127);
      }
    if (pid < 0) return -1;
    {
      int status;
      while (waitpid (pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
      return status;
    }
  }'''
        util.write_text(text.replace(needle, helper))
        # patch 2.7.6 bundles a config.sub too old for arm64-apple-darwin.
        self.configure_make(src, ['--disable-silent-rules'],
                            host='aarch64-apple-darwin')
        bindir = self.sysroot / PREFIX.strip('/') / 'usr/bin'
        shutil.copy2(bindir / 'patch', bindir / 'gpatch')

    STEPS = ('libmd', 'liblzma', 'dash', 'patch', 'dpkg', 'uicache')

    def make_host_shim(self):
        # Non-login shells default to the CLT MacOSX27.2 SDK, whose
        # libSystem.B.tbd the linker rejects (unknown architecture).
        # Pin host-tool compiles to the 26.5 SDK via a wrapper, the same
        # workaround the baseband host tests use.
        shim = self.work / 'hostshim'
        shim.mkdir(parents=True, exist_ok=True)
        sdk = '/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk'
        for name in ('clang', 'clang++', 'cc', 'c++'):
            tool = shim / name
            tool.write_text(f'#!/bin/sh\nexec /usr/bin/{name} -isysroot {sdk} "$@"\n')
            tool.chmod(0o755)
        self.env['CC_FOR_BUILD'] = str(shim / 'clang')
        self.env['PATH'] = str(shim) + os.pathsep + self.env['PATH']
        gpatch = shim / 'gpatch'
        if not gpatch.exists():
            # dpkg's configure hard-requires GNU patch; build the pinned
            # release for the host into the shim dir.
            src = extract('patch', self.work)
            host_env = dict(self.env, CC=str(shim / 'clang'), CXX=str(shim / 'clang++'))
            self.run([src / 'configure', '--disable-silent-rules',
                      f'--prefix={shim}'], cwd=src, env=host_env)
            self.run(['make', '-j8'], cwd=src, env=host_env)
            self.run(['make', 'install'], cwd=src, env=host_env)
            (shim / 'bin/patch').rename(gpatch)

    def build(self, only):
        self.work.mkdir(parents=True)
        self.make_host_shim()
        for step in self.STEPS:
            if only and step not in only:
                continue
            print(f'== {step}')
            getattr(self, 'build_' + step)()


def assemble(out):
    """Copy sysroot installs into the prefix-relative bootstrap tree, add the
    dpkg/apt skeleton, ad-hoc sign every Mach-O, write manifest.json."""
    src_root = out / 'work/sysroot' / PREFIX.strip('/')
    tree = out / 'bootstrap'
    if tree.exists():
        shutil.rmtree(tree)
    shutil.copytree(src_root, tree, symlinks=True)
    # Build-only artifacts do not belong in the device bootstrap tree.
    for rel in ('usr/include', 'usr/lib/pkgconfig', 'usr/share/doc',
                'usr/share/man', 'usr/share/info', 'usr/share/aclocal'):
        shutil.rmtree(tree / rel, ignore_errors=True)
    for rel in ('Library/dpkg/info', 'Library/dpkg/updates', 'Library/dpkg/parts',
                'Library/dpkg/alternatives', 'Library/dpkg/triggers',
                'etc/apt/sources.list.d', 'etc/apt/preferences.d', 'etc/apt/apt.conf.d',
                'var/lib/apt/lists/partial', 'var/cache/apt/archives/partial',
                'var/log/apt', 'var/log', 'Applications'):
        (tree / rel).mkdir(parents=True, exist_ok=True)
    (tree / 'Library/dpkg/status').touch()
    (tree / 'etc/apt/apt.conf.d/00storm').write_text(
        'APT::Architecture "iphoneos-arm64";\n')
    (tree / 'etc/apt/sources.list.d/storm.list').write_text(
        '# storm bootstrap ships no remote source yet; local .deb installs only.\n')
    files = {}
    for path in sorted(tree.rglob('*')):
        rel = path.relative_to(tree).as_posix()
        if path.is_symlink():
            files['/' + rel] = {'symlink': os.readlink(path)}
            continue
        if not path.is_file():
            continue
        data = path.read_bytes()
        if data[:4] in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
            subprocess.run(['codesign', '-s', '-', '-f', str(path)],
                           check=True, capture_output=True)
            data = path.read_bytes()
        files['/' + rel] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                            'mode': oct(path.stat().st_mode & 0o777)}
    manifest = {'abi': 1, 'build': '20H392', 'target': TARGET, 'prefix': PREFIX,
                'files': files,
                'not_included': ['apt (pinned, dependency tree pending)',
                                 'coreutils (pinned, optional)']}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--developer-dir', type=Path)
    ap.add_argument('--only', action='append',
                    help='repeat to limit steps: libmd liblzma dash dpkg uicache')
    ap.add_argument('--assemble-only', action='store_true')
    a = ap.parse_args()
    out = a.out.resolve()
    if not any(out.is_relative_to(p.resolve()) for p in (ROOT / '.local', ROOT / 'x9/out')):
        raise ValueError('output must be in .local or x9/out')
    developer = a.developer_dir
    if developer is None:
        local = json.loads((ROOT / 'x9/build/local.json').read_text())
        developer = Path(local['developer_dir'])
    b = Builder(out, developer)
    if not a.assemble_only:
        out.mkdir(parents=True, exist_ok=False)
        b.build(set(a.only) if a.only else None)
    manifest = assemble(out)
    print(f'bootstrap files: {len(manifest["files"])} -> {out}/bootstrap')


if __name__ == '__main__':
    main()
