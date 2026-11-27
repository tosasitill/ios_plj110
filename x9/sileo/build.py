#!/usr/bin/env python3
"""Build the X9 storm bootstrap subset; never mount, stage or modify a device.

Downloads the pinned sources in sources.json (SHA256 verified, content-
addressed cache under .local/x9/sileo-cache), cross-compiles for
arm64-apple-ios16.0 with the public prefix /var/storm, assembles a
prefix-relative bootstrap tree and emits a runtime.json-style manifest.

Built here: libmd, liblzma (xz), dash (as sh), dpkg, the apt dependency
tree (gmp, libtasn1, nettle, libgpg-error, libgcrypt, xxhash, lz4, zstd,
berkeleydb, gnutls), apt, coreutils, uicache. Everything pinned in
sources.json is built; the manifest never claims more than the tree.
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
            'CPPFLAGS': f'-I{inc}',
            'LDFLAGS': f'-L{lib}',
            'MACOSX_DEPLOYMENT_TARGET': '',
            'ZERO_AR_DATE': '1',
        })

    def run(self, command, cwd=None, env=None):
        print('+', ' '.join(map(str, command)), flush=True)
        subprocess.run(list(map(str, command)), cwd=cwd,
                       env=env or self.env, check=True)

    def configure_make(self, src, args, make_targets=('all',), jobs=8,
                       host='arm64-apple-darwin', dirs=False):
        extra = []
        if dirs:
            extra = [f'--sysconfdir={PREFIX}/etc',
                     f'--localstatedir={PREFIX}/var']
        self.run([src / 'configure', '--build=x86_64-apple-darwin',
                  f'--host={host}', f'--prefix={PREFIX}/usr'] + extra + args,
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
                  # Otherwise dpkg derives the perl module dir from the
                  # host perl and installs to /Network/Library/Perl.
                  f'--with-perllibdir={PREFIX}/usr/share/perl5',
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


    # ---- apt dependency tree (Procursus pins, see sources.json) ----

    def _pfx(self):
        """Absolute sysroot path of the storm prefix (\u2026/var/storm/usr)."""
        return self.sysroot / PREFIX.strip('/') / 'usr'

    def build_gmp(self):
        src = extract('gmp', self.work)
        # Procursus libgmp10-setup sed: keep the -nostdlib probe link on
        # the target arch/min-version. Our CC already carries both, so
        # this is a safeguard, applied only if the string is present.
        cfg = src / 'configure'
        text = cfg.read_text()
        needle = '-keep_private_externs -nostdlib'
        if needle in text:
            cfg.write_text(text.replace(
                needle, '-keep_private_externs -miphoneos-version-min=16.0'
                        ' -arch arm64 -nostdlib'))
        env = dict(self.env)
        env['CPP_FOR_BUILD'] = f"{self.env['CC_FOR_BUILD']} -E"
        # --disable-assembly per Procursus (their issue #750); the plain
        # C build is slower but portable, fine for nettle's use.
        self.run([src / 'configure', '--build=x86_64-apple-darwin',
                  '--host=arm64-apple-darwin', f'--prefix={PREFIX}/usr',
                  f'--sysconfdir={PREFIX}/etc',
                  f'--localstatedir={PREFIX}/var',
                  '--disable-assembly', '--enable-cxx',
                  '--disable-silent-rules'], cwd=src, env=env)
        self.run(['make', '-j8'], cwd=src, env=env)
        self.run(['make', 'install', f'DESTDIR={self.sysroot}'],
                 cwd=src, env=env)

    def build_libtasn1(self):
        src = extract('libtasn1', self.work)
        self.configure_make(src, ['--disable-silent-rules'], dirs=True)

    def build_nettle(self):
        src = extract('nettle', self.work)
        env = dict(self.env)
        env['CPP_FOR_BUILD'] = f"{self.env['CC_FOR_BUILD']} -E"
        self.run([src / 'configure', '--build=x86_64-apple-darwin',
                  '--host=arm64-apple-darwin', f'--prefix={PREFIX}/usr',
                  f'--sysconfdir={PREFIX}/etc',
                  f'--localstatedir={PREFIX}/var',
                  '--disable-silent-rules'], cwd=src, env=env)
        self.run(['make', '-j8'], cwd=src, env=env)
        self.run(['make', 'install', f'DESTDIR={self.sysroot}'],
                 cwd=src, env=env)

    def build_libgpg_error(self):
        src = extract('libgpg-error', self.work)
        # Procursus's mkheader sed targets an older libgpg-error whose
        # host table was static. 1.48 canonicalizes the host triplet in
        # mkheader itself and ships syscfg/lock-obj-pub.aarch64-apple-
        # darwin.h, so no patch is needed (verified in the tarball).
        self.configure_make(src, ['--disable-silent-rules', '--disable-nls',
                                  '--enable-install-gpg-error-config'],
                            host='aarch64-apple-darwin', dirs=True)

    def build_libgcrypt(self):
        src = extract('libgcrypt', self.work)
        # The iPhoneOS SDK omits the getentropy() declaration (no
        # <sys/random.h>, unistd.h does not declare it) although the
        # function is exported by libSystem and configure's link check
        # finds it; rndgetentropy.c then fails under
        # -Wimplicit-function-declaration. Add the missing declaration
        # at its single use site. (A -include shim was tried first and
        # leaked into the MPI assembler rules, breaking .S files.)
        rnd = src / 'random/rndgetentropy.c'
        text = rnd.read_text()
        needle = '#include <unistd.h>\n'
        assert text.count(needle) == 1
        rnd.write_text(text.replace(
            needle, needle +
            '/* x9: declaration missing from the iPhoneOS SDK headers. */\n'
            'extern int getentropy (void *__buf, size_t __len);\n'))
        # Procursus libgcrypt-setup: Mach-O's assembler rejects the ELF
        # .type/.size directives in the aarch64 asm and the symbol
        # prefix differs (__gcry).
        for asm in sorted((src / 'mpi/aarch64').glob('*.S')):
            lines = [l for l in asm.read_text().splitlines()
                     if '.type' not in l and '.size' not in l]
            asm.write_text('\n'.join(lines).replace('_gcry', '__gcry') + '\n')
        self.configure_make(
            src, ['--disable-silent-rules', '--disable-doc',
                  f'--with-gpg-error-prefix={self._pfx()}'],
            host='aarch64-apple-darwin', dirs=True)

    def build_xxhash(self):
        src = extract('xxhash', self.work)
        cc = self.env['CC']
        # PREFIX must be present at build time too: xxhash's Makefile
        # bakes the dylib install name from it, and the install step
        # alone left it at the /usr/local default.
        self.run(['make', '-j8', 'UNAME=Darwin', f'CC={cc}',
                  f'CFLAGS={self.env["CFLAGS"]}',
                  f'PREFIX={PREFIX}/usr'], cwd=src)
        self.run(['make', 'install', 'UNAME=Darwin', f'CC={cc}',
                  f'PREFIX={PREFIX}/usr', f'DESTDIR={self.sysroot}'],
                 cwd=src)

    def build_lz4(self):
        src = extract('lz4', self.work)
        # Procursus lz4-setup rewrites literal 'ln -s' in Makefile.inc
        # to '$(LN_S)' because their make environment defines LN_S.
        # Ours does not, and the only 'ln -s' in the 1.9.4 tarball is
        # the definition `LN_SF = ln -sf` itself: the rewrite produced
        # `LN_SF = $(LN_S)f` = `f`, so make tried to execute a program
        # named `f` at the versioned-links step (install Error 1).
        # Upstream's literal works as-is on the build host; keep it.
        cc = self.env['CC']
        self.run(['make', '-j8', 'install', 'TARGET_OS=Darwin',
                  f'CC={cc}', f'CFLAGS={self.env["CFLAGS"]}',
                  f'PREFIX={PREFIX}/usr', f'DESTDIR={self.sysroot}'],
                 cwd=src)

    def build_zstd(self):
        src = extract('zstd', self.work)
        cc = self.env['CC']
        self.run(['make', '-j8', 'install', 'UNAME=Darwin', f'CC={cc}',
                  f'PREFIX={PREFIX}/usr', f'DESTDIR={self.sysroot}'],
                 cwd=src)

    def build_berkeleydb(self):
        src = extract('berkeleydb', self.work)
        build = src / 'build_unix'
        # Deviations from Procursus berkeleydb.mk, both deliberate:
        # their mutex Darwin/_spin_lock_try is OSSpinLock, absent from
        # the iOS 16 SDK; POSIX/pthreads is the supported equivalent.
        # apt needs only the C library, so no --enable-cxx/sql/stl.
        # Headers land in include/db181, matching the
        # BERKELEY_INCLUDE_DIRS apt.mk passes.
        # Their nodoc.diff is NOT cosmetic, as first assumed: with
        # sql/stl off, install_docs copies doc subdirs (bdb-sql,
        # gsg_db_server) that this configuration never produces and
        # make install fails outright. Apply its one-line change to
        # dist/Makefile.in (drop install_docs from library_install).
        makefile_in = src / 'dist/Makefile.in'
        text = makefile_in.read_text()
        needle = ('library_install: install_include install_lib '
                  'install_utilities @ADDITIONAL_INSTALL@ install_docs')
        assert text.count(needle) == 1
        makefile_in.write_text(text.replace(
            needle, needle[: -len(' install_docs')]))
        self.run([src / 'dist/configure', '--build=x86_64-apple-darwin',
                  '--host=aarch64-apple-darwin', f'--prefix={PREFIX}/usr',
                  f'--includedir={PREFIX}/usr/include/db181',
                  '--with-mutex=POSIX/pthreads'], cwd=build)
        self.run(['make', '-j8'], cwd=build)
        self.run(['make', 'install', f'DESTDIR={self.sysroot}'], cwd=build)

    def build_gnutls(self):
        src = extract('gnutls', self.work)
        cfg = src / 'configure'
        text = cfg.read_text()
        needle = '-keep_private_externs -nostdlib'
        if needle in text:
            cfg.write_text(text.replace(
                needle, '-keep_private_externs -miphoneos-version-min=16.0'
                        ' -arch arm64 -nostdlib'))
        env = dict(self.env)
        env['CPP_FOR_BUILD'] = f"{self.env['CC_FOR_BUILD']} -E"
        # Leaner than Procursus gnutls.mk, deliberately: apt's https
        # method needs a TLS client, not the full feature set. Skipped
        # trees: p11-kit (--without-p11-kit), libidn2 and brotli
        # (absent from the sysroot, so off); libunistring uses
        # gnutls's included copy. Kept from the recipe:
        # --enable-local-libopts, the trust-store path shape and the
        # malloc/realloc cross answers.
        self.run([src / 'configure', '--build=x86_64-apple-darwin',
                  '--host=arm64-apple-darwin', f'--prefix={PREFIX}/usr',
                  f'--sysconfdir={PREFIX}/etc',
                  f'--localstatedir={PREFIX}/var',
                  '--disable-silent-rules', '--disable-doc',
                  '--disable-nls', '--disable-guile',
                  '--enable-local-libopts', '--without-p11-kit',
                  '--with-included-unistring',
                  f'--with-default-trust-store-file={PREFIX}/etc/ssl/certs/cacert.pem',
                  'ac_cv_func_malloc_0_nonnull=yes',
                  'ac_cv_func_realloc_0_nonnull=yes'],
                 cwd=src, env=env)
        self.run(['make', '-j8'], cwd=src, env=env)
        self.run(['make', 'install', f'DESTDIR={self.sysroot}'],
                 cwd=src, env=env)


    def build_apt(self):
        src = extract('apt', self.work)
        recipes = extract('procursus_recipes', self.work)
        pdir = recipes / 'build_patch' / 'apt'
        for patch in sorted(pdir.iterdir()):
            if patch.is_file():
                data = patch.read_bytes()
                data = data.replace(b'@MEMO_PREFIX@', PREFIX.encode())
                data = data.replace(b'@MEMO_SUB_PREFIX@', b'/usr')
                print(f'+ patch apt/{patch.name}')
                subprocess.run(['patch', '-p1', '-f'], cwd=src,
                               input=data, check=True)
        # Procursus apt-setup: two translation units become ObjC++ on
        # Apple targets, and ftparchive gets its own memrchr copy.
        # (apt-macos/apt-key.diff is for macOS targets only; skipped.)
        for cc_path in (src / 'apt-private/private-output.cc',
                        src / 'apt-pkg/algorithms.cc'):
            if cc_path.exists():
                cc_path.rename(cc_path.with_suffix('.mm'))
        memrchr = src / 'apt-pkg/memrchr.cc'
        if memrchr.exists():
            shutil.copy2(memrchr, src / 'ftparchive/memrchr.cc')
        # CURRENT_VENDOR=storm needs a vendor dir; doc is off, so only
        # the shape matters (no apt.conf-* fragments get installed).
        vdir = src / 'vendor/storm'
        vdir.mkdir(exist_ok=True)
        (vdir / 'sources.list.in').write_text(
            '# storm ships no remote source yet; see\n'
            '# /var/storm/etc/apt/sources.list.d/storm.list\n')
        (vdir / 'apt-vendor.ent').write_text(
            '<!-- details about the keys used by the distribution -->\n'
            '<!ENTITY keyring-distro "storm">\n'
            '<!ENTITY keyring-package "">\n'
            '<!ENTITY keyring-filename "">\n'
            '<!ENTITY keyring-removed-filename "">\n'
            '<!ENTITY keyring-master-filename "">\n'
            '<!ENTITY keyring-uri "">\n'
            '<!ENTITY current-codename "storm">\n')
        pfx = self._pfx()
        # Cross pkg-config: re-root the storm .pc files into the sysroot
        # (their compiled-in prefix is the device path /var/storm/usr).
        pcwrap = self.work / 'cross-pkg-config'
        pcwrap.write_text(
            '#!/bin/sh\n'
            f'export PKG_CONFIG_LIBDIR={pfx}/lib/pkgconfig:{pfx}/share/pkgconfig\n'
            f'export PKG_CONFIG_SYSROOT_DIR={self.sysroot}\n'
            'exec /opt/homebrew/bin/pkg-config "$@"\n')
        pcwrap.chmod(0o755)
        tc = self.work / 'apt-toolchain.cmake'
        tc.write_text(
            'set(CMAKE_SYSTEM_NAME Darwin)\n'
            'set(CMAKE_SYSTEM_PROCESSOR arm64)\n'
            f'set(CMAKE_OSX_SYSROOT "{self.sdk}")\n'
            'set(CMAKE_OSX_ARCHITECTURES arm64)\n'
            f'set(CMAKE_C_COMPILER "{self.clang}")\n'
            f'set(CMAKE_CXX_COMPILER "{self.clang}++")\n'
            'set(CMAKE_C_COMPILER_TARGET arm64-apple-ios16.0)\n'
            'set(CMAKE_CXX_COMPILER_TARGET arm64-apple-ios16.0)\n'
            f'set(CMAKE_FIND_ROOT_PATH "{pfx}" "{self.sdk}")\n'
            f'set(CMAKE_PREFIX_PATH "{pfx}")\n'
            'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n'
            'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n'
            'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n'
            'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n'
            f'set(PKG_CONFIG_EXECUTABLE "{pcwrap}")\n')
        build = src / 'build'
        build.mkdir(exist_ok=True)
        # CMAKE_POLICY_VERSION_MINIMUM: the host cmake is 4.x, which
        # removed compatibility with apt 2.9.4's minimum (3.4).
        # USE_NLS=0 (Procursus uses 1): no gettext tree in the
        # bootstrap, same call as dpkg's --disable-nls in round 1.
        # CURRENT_VENDOR=storm with the vendor dir injected above.
        self.run(['cmake', '-S', str(src), '-B', str(build),
                  f'-DCMAKE_TOOLCHAIN_FILE={tc}',
                  '-DCMAKE_BUILD_TYPE=Release',
                  '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
                  f'-DCMAKE_INSTALL_PREFIX={PREFIX}/usr',
                  f'-DCMAKE_INSTALL_SYSCONFDIR={PREFIX}/etc',
                  f'-DCMAKE_INSTALL_NAME_DIR={PREFIX}/usr/lib',
                  f'-DSTATE_DIR={PREFIX}/var/lib/apt',
                  f'-DCACHE_DIR={PREFIX}/var/cache/apt',
                  f'-DLOG_DIR={PREFIX}/var/log/apt',
                  f'-DCONF_DIR={PREFIX}/etc/apt',
                  '-DROOT_GROUP=wheel', '-DCURRENT_VENDOR=storm',
                  '-DCOMMON_ARCH=iphoneos-arm64',
                  '-DUSE_NLS=0', '-DWITH_DOC=0', '-DWITH_TESTS=0',
                  f'-DDPKG_DATADIR={PREFIX}/usr/share/dpkg',
                  f'-DBERKELEY_INCLUDE_DIRS={pfx}/include/db181'])
        self.run(['make', '-j8'], cwd=build)
        self.run(['make', 'install', f'DESTDIR={self.sysroot}'], cwd=build)

    def build_coreutils(self):
        src = extract('coreutils', self.work)
        env = dict(self.env)
        env['MAKEINFO'] = 'true'  # tarball ships .info; don't regenerate
        # Procursus coreutils.mk runs autoreconf for their patch set;
        # we patch nothing, so the shipped configure is used as-is.
        # gl_cv_macro_MB_CUR_MAX_good is their cross answer, kept.
        self.run([src / 'configure', '--build=x86_64-apple-darwin',
                  '--host=arm64-apple-darwin', f'--prefix={PREFIX}/usr',
                  f'--sysconfdir={PREFIX}/etc',
                  f'--localstatedir={PREFIX}/var',
                  '--disable-nls', '--disable-silent-rules',
                  '--disable-dependency-tracking',
                  'gl_cv_macro_MB_CUR_MAX_good=yes'],
                 cwd=src, env=env)
        self.run(['make', '-j8'], cwd=src, env=env)
        self.run(['make', 'install', f'DESTDIR={self.sysroot}'],
                 cwd=src, env=env)

    STEPS = ('libmd', 'liblzma', 'dash', 'patch', 'dpkg', 'gmp',
             'libtasn1', 'nettle', 'libgpg_error', 'libgcrypt', 'xxhash',
             'lz4', 'zstd', 'berkeleydb', 'gnutls', 'apt', 'coreutils',
             'uicache')

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
        'APT::Architecture "iphoneos-arm64";\n'
        # apt derives Dir::State::status from Dir::State (debsystem.cc
        # getDpkgStatusLocation), which with STATE_DIR=<prefix>/var/lib/apt
        # yields <prefix>/var/lib/dpkg/status. dpkg's admindir in the
        # rootless layout is <prefix>/Library/dpkg, so pin the status
        # file explicitly; verify.py checks the two agree.
        f'Dir::State::status "{PREFIX}/Library/dpkg/status";\n')
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
                'not_included': []}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--developer-dir', type=Path)
    ap.add_argument('--only', action='append',
                    help='repeat to limit steps (see Builder.STEPS)')
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
