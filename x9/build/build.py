#!/usr/bin/env python3
"""Build the X9 kernel and boot.img from this repository.

    python3 -I x9/build/build.py                 # kernel + boot.img
    python3 -I x9/build/build.py kernel          # compile + link + strip only
    python3 -I x9/build/build.py boot --kernel K # package an existing kernel
    python3 -I x9/build/build.py --dry-run       # print every command
    python3 -I x9/build/build.py plugins --revision 208       # Metal/PanVK + package
    python3 -I x9/build/build.py all --revision 208           # plugins, kernel, boot

Inputs:
  x9/kernel/recipe.json      compile templates, per-object sources, link/strip
  x9/kernel/link-order.txt   exact link order: xnu:<object in the frozen XNU
                             tree> or x9:<overlay object>
  x9/kernel/src, generated   overlay sources (XNU-relative paths)
  x9/kernel/prebuilt         objects whose compile command is not known yet
  x9/build/boot.json         packaging options and boot-args
  x9/build/local.json        machine paths (copy config.example.json)
  .local/x9/wifi-fw          MT6653 vendor firmware, embedded via x9/tools/wifi_firmware.py
  .local/x9/wifi-nvram/WIFI  device Wi-Fi NVRAM (nvdata), embedded via x9/tools/wifi_nvram.py
                             ("wifi_firmware" in local.json overrides; absent = stub)
  .local/x9/baseband/Mav17-9.00.02.Release.bbfw  original Apple ABM assets, not physical MTK firmware
                             ("baseband_firmware" in local.json overrides; absent = explicit stub)

Outputs go to x9/out/<name>/ (default name: x9-<git short hash>[-dirty]),
with everything printed also written to x9/out/<name>/build.log.
Compiles are incremental through compiler depfiles; with "compiler_launcher"
(e.g. "ccache") in local.json, a fresh output directory reuses earlier
compiles too. Nothing touches a device.
"""
import argparse
import concurrent.futures
import json
import os
import shlex
import shutil
import subprocess
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
X9 = HERE.parent
ROOT = X9.parent
KERNEL = X9 / 'kernel'
REQUIRED = ['developer_dir', 'dxb']


def load_config(path):
    if not path.is_file():
        sys.exit(f'missing {path}: copy {HERE / "config.example.json"} and fill in the paths')
    cfg = json.loads(path.read_text())
    missing = [k for k in REQUIRED if not cfg.get(k)]
    if missing:
        sys.exit(f'{path}: missing {", ".join(missing)}')
    for key in ('dxb', 'root_manifest'):
        if cfg.get(key):
            path = Path(cfg[key]).expanduser()
            cfg[key] = str(path if path.is_absolute() else ROOT / path)
    return cfg


def default_name():
    try:
        rev = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'], text=True).strip()
        dirty = subprocess.run(['git', '-C', str(ROOT), 'diff', '--quiet', 'HEAD', '--', 'x9'],
                               check=False).returncode != 0
        return f'x9-{rev}' + ('-dirty' if dirty else '')
    except (OSError, subprocess.CalledProcessError):
        return 'x9-local'


class Builder:
    def __init__(self, cfg, out, dry_run, jobs):
        self.cfg, self.out, self.dry_run, self.jobs = cfg, out, dry_run, jobs
        self.recipe = json.loads((KERNEL / 'recipe.json').read_text())
        self.objdir = out / 'kernel-objects'
        self.vars = {'{DEVELOPER_DIR}': cfg['developer_dir'].rstrip('/'),
                     '{DXB}': cfg['dxb'].rstrip('/'),
                     '{SRC}': str(KERNEL / 'src'),
                     '{GEN}': str(KERNEL / 'generated')}
        self.env = dict(os.environ, DEVELOPER_DIR=self.vars['{DEVELOPER_DIR}'])
        self.lock = threading.Lock()
        self.logfile = None if dry_run else open(out / 'build.log', 'a')
        self.launcher = []
        if cfg.get('compiler_launcher'):
            found = shutil.which(cfg['compiler_launcher'])
            if found:
                self.launcher = [found]
            else:
                self.log(f'warning: compiler_launcher {cfg["compiler_launcher"]!r} not found; compiling without it')

    def log(self, text):
        with self.lock:
            print(text, flush=True)
            if self.logfile:
                self.logfile.write(text + '\n')
                self.logfile.flush()

    def sub(self, text):
        for key, value in self.vars.items():
            text = text.replace(key, value)
        return text

    def run(self, cmd, cwd, env=None, label=None, stream=False):
        """Run one command with stdout+stderr captured and logged as a block
        (stream=True: line by line as it runs, for long sequential steps).
        Returns (ok, output); a failure is reported, not raised."""
        shown = f'+ (cd {shlex.quote(str(cwd))} && ' + ' '.join(shlex.quote(c) for c in cmd) + ')'
        if self.dry_run:
            self.log(shown)
            return True, ''
        if stream:
            self.log(shown)
            p = subprocess.Popen(cmd, cwd=cwd, env=env or self.env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, errors='replace')
            for line in p.stdout:
                self.log(line.rstrip('\n'))
            p.wait()
            if p.returncode:
                self.log(f'!! {label or Path(cmd[0]).name} failed with exit code {p.returncode}')
            return p.returncode == 0, ''
        p = subprocess.run(cmd, cwd=cwd, env=env or self.env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, errors='replace')
        block = shown + ('\n' + p.stdout.rstrip() if p.stdout.strip() else '')
        if p.returncode:
            block += f'\n!! {label or Path(cmd[0]).name} failed with exit code {p.returncode}'
        self.log(block)
        return p.returncode == 0, p.stdout

    def run_or_exit(self, cmd, cwd, env=None, label=None, stream=False):
        ok, _ = self.run(cmd, cwd, env, label, stream)
        if not ok:
            self.log(f'build stopped: {label or Path(cmd[0]).name} failed; full log: {self.out / "build.log"}')
            sys.exit(1)

    # ---- compile -------------------------------------------------------------
    def stale(self, obj, dep, src):
        if not obj.is_file() or not dep.is_file():
            return True
        built = obj.stat().st_mtime
        inputs = [src, KERNEL / 'recipe.json']
        text = dep.read_text(errors='replace').replace('\\\n', ' ')
        inputs += [Path(p) for line in text.splitlines() if ':' in line for p in line.split(':', 1)[1].split()]
        return any(not p.exists() or p.stat().st_mtime > built for p in inputs)

    def compile_one(self, entry):
        t = self.recipe['templates'][entry['template']]
        src = KERNEL / 'src' / entry['source']
        obj, dep = self.objdir / entry['name'], self.objdir / (entry['name'] + '.d')
        if not self.stale(obj, dep, src):
            return False
        cmd = self.launcher + [self.sub(t['compiler'])] + [self.sub(a) for a in t['args']] + \
              ['-MD', '-MF', str(dep), '-c', str(src), '-o', str(obj)]
        ok, _ = self.run(cmd, self.sub(t['cwd']), label=entry['name'])
        if not ok:
            obj.unlink(missing_ok=True)   # never link a stale object after a failed compile
            return entry['name']
        return True

    def compile(self):
        if not self.dry_run:
            self.objdir.mkdir(parents=True, exist_ok=True)
        # MT6653 firmware -> generated/x9_wifi_firmware.inc (gitignored; stub when absent)
        fw = Path(self.cfg.get('wifi_firmware') or ROOT / '.local/x9/wifi-fw').expanduser()
        self.run_or_exit([sys.executable, '-I', str(X9 / 'tools/wifi_firmware.py'), '--src', str(fw)], ROOT,
                         label='wifi_firmware.py')
        # Device Wi-Fi NVRAM -> generated/x9_wifi_nvram.inc (gitignored; stub when absent)
        nv = Path(self.cfg.get('wifi_nvram') or ROOT / '.local/x9/wifi-nvram/WIFI').expanduser()
        self.run_or_exit([sys.executable, '-I', str(X9 / 'tools/wifi_nvram.py'), '--src', str(nv)], ROOT,
                         label='wifi_nvram.py')
        # Vendor MD OTA tree (T: drive of the modem FSD) -> generated/x9_mdota.inc (gitignored; stub when absent)
        self.run_or_exit([sys.executable, '-I', str(X9 / 'tools/md_ota_pack.py')], ROOT, label='md_ota_pack.py')
        # Original Apple Mav17 files are for ABM/BBUpdater only, never MTK MD firmware.
        bb = [sys.executable, '-I', str(X9 / 'tools/baseband_firmware.py')]
        if self.cfg.get('baseband_firmware'):
            bb += ['--src', str(Path(self.cfg['baseband_firmware']).expanduser())]
        self.run_or_exit(bb, ROOT, label='baseband_firmware.py')
        todo = [e for e in self.recipe['objects'] if 'prebuilt' not in e]
        with concurrent.futures.ThreadPoolExecutor(self.jobs) as pool:
            results = list(pool.map(self.compile_one, todo))
        failed = [r for r in results if isinstance(r, str)]
        built = sum(1 for r in results if r is True)
        self.log(f'compiled {built}/{len(todo)} overlay objects '
                 f'({len(self.recipe["objects"]) - len(todo)} prebuilt)'
                 + (f' via {self.launcher[0]}' if self.launcher else ''))
        if failed:
            self.log(f'build stopped: {len(failed)} object(s) failed: {", ".join(failed)}; '
                     f'compiler output is above each "!!" line in {self.out / "build.log"}')
            sys.exit(1)

    # ---- link ----------------------------------------------------------------
    def link(self):
        xnu = Path(self.sub(self.recipe['xnu_object_tree']))
        overlay = {e['name']: (KERNEL / e['prebuilt']) if 'prebuilt' in e else self.objdir / e['name']
                   for e in self.recipe['objects']}
        paths, missing = [], []
        for line in (KERNEL / 'link-order.txt').read_text().split():
            kind, name = line.split(':', 1)
            p = xnu / name if kind == 'xnu' else overlay[name]
            if kind == 'xnu' and not p.is_file():
                missing.append(str(p))
            paths.append(str(p))
        if missing and not self.dry_run:
            self.log(f'build stopped: {len(missing)} frozen XNU objects missing, e.g. {missing[0]}')
            sys.exit(1)
        filelist = self.out / 'kernel-link.filelist'
        unstripped = self.out / 'kernel.development.sweet.unstripped'
        kernel = self.out / 'kernel.development.sweet'
        if not self.dry_run:
            filelist.write_text('\n'.join(paths) + '\n')
        link = self.recipe['link']
        self.vars.update({'{FILELIST}': str(filelist), '{UNSTRIPPED}': str(unstripped)})
        self.run_or_exit([self.sub(link['compiler'])] + [self.sub(a) for a in link['args']], self.sub(link['cwd']),
                         label='link')
        strip = self.recipe['strip']
        self.run_or_exit([self.sub(strip['tool'])] + strip['args'] + [str(unstripped), '-o', str(kernel)], self.out,
                         label='strip')
        return kernel

    # ---- Metal/PanVK plugins ---------------------------------------------------
    def plugins(self, revision, panvk):
        """Build X9PanVK<N> and X9GeneralMetal<N> from x9/metal/General and the
        .local third-party trees, package them for the mailbox into the output
        directory and rewrite the kernel allowlist, so a following kernel build
        accepts exactly these files (mailbox/update.py stages the package)."""
        scripts, work = HERE / 'scripts', ROOT / '.local/x9/work/x9-metal-render'
        env = dict(self.env, X9_HOMEBREW=self.cfg.get('homebrew') or '/opt/homebrew')
        py = [sys.executable, '-I']
        self.run_or_exit(py + [str(HERE / 'prepare-local.py'), '--check'], ROOT, env, 'prepare-local --check', True)
        configured = (work / 'build-panvk-native-ios/build.ninja').is_file()
        if panvk == 'full' or (panvk == 'auto' and not configured):
            self.run_or_exit(py + [str(scripts / 'build-panvk-native-ios.py')], ROOT, env, 'PanVK full build', True)
        elif panvk != 'skip':
            self.run_or_exit(py + [str(scripts / 'build-panvk-incremental.py')], ROOT, env, 'PanVK incremental build', True)
        rev = ['--revision', str(revision)]
        self.run_or_exit(py + [str(scripts / 'package-panvk-native.py')] + rev, ROOT, env, 'package X9PanVK', True)
        self.run_or_exit(py + [str(scripts / 'build-general-metal.py')] + rev, ROOT, env, 'build X9GeneralMetal', True)
        self.run_or_exit(py + [str(scripts / 'package-plugins.py')] + rev + ['--out', str(self.out)], ROOT, env,
                         'package plugins + kernel allowlist', True)

    # ---- boot.img --------------------------------------------------------------
    def boot(self, kernel, boot_config, extra_args=()):
        opts = json.loads(boot_config.read_text())
        self.log(f'boot configuration: {boot_config}')
        for arg in extra_args:
            if not arg or any(c.isspace() for c in arg):
                sys.exit(f'--boot-arg must be one non-empty token: {arg!r}')
            if arg in opts['boot_args']:
                sys.exit(f'--boot-arg {arg} is already in {boot_config}')
            opts['boot_args'].append(arg)
            self.log(f'extra boot arg: {arg}')
        manifest = self.cfg.get('root_manifest')
        if not manifest:
            sys.exit('local.json: root_manifest is required to package boot.img')
        dev = self.vars['{DEVELOPER_DIR}']
        tools = self.out / 'build-tools'
        wrapper = tools / 'x9-clang'
        # Bare-metal shim objects use LLVM (matches lld); host-side helpers use Xcode clang.
        script = ('#!/bin/sh\nfor arg in "$@"; do\n case "$arg" in\n'
                  f'  --target=aarch64-none-elf) exec {shlex.quote(self.cfg["llvm_clang"])} "$@" ;;\n'
                  ' esac\ndone\n'
                  f'exec {shlex.quote(dev + "/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang")} '
                  f'-isysroot {shlex.quote(dev + "/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk")} "$@"\n')
        if not self.dry_run:
            tools.mkdir(parents=True, exist_ok=True)
            wrapper.write_text(script)
            wrapper.chmod(0o755)
        env = dict(self.env, X9_CC=str(wrapper), X9_LD=self.cfg['lld'],
                   SDKROOT=dev + '/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
        cmd = [sys.executable, '-I', str(X9 / 'tools/boot-xnu.py'), '--kernel', str(kernel), '--out', str(self.out),
               '--ufs-native-manifest', manifest, '--boot-args', ' '.join(opts['boot_args'])] + opts['options']
        self.run_or_exit(cmd, ROOT, env, label='boot-xnu.py')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('step', nargs='?', choices=['all', 'kernel', 'boot', 'plugins'], default='all')
    ap.add_argument('--revision', type=int, help='plugin revision (X9GeneralMetal<N>/X9PanVK<N>); with "all", build plugins first')
    ap.add_argument('--panvk', choices=['auto', 'incremental', 'full', 'skip'], default='auto',
                    help='PanVK rebuild: auto = incremental if already configured, else full')
    ap.add_argument('--config', type=Path, default=HERE / 'local.json')
    ap.add_argument('--name', help='output directory name under x9/out')
    ap.add_argument('--kernel', type=Path, help='boot step only: kernel to package')
    ap.add_argument('--boot-config', type=Path, default=HERE / 'boot.json',
                    help='packaging/boot-args JSON; default unchanged, diagnostic overrides stay separate')
    ap.add_argument('--boot-arg', action='append', default=[], metavar='TOKEN',
                    help='append one boot-arg token to the boot config (repeatable), e.g. x9h0')
    ap.add_argument('--dry-run', action='store_true', help='print commands, run nothing')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    cfg = load_config(a.config)
    out = X9 / 'out' / (a.name or default_name())
    if not a.dry_run:
        out.mkdir(parents=True, exist_ok=True)
    b = Builder(cfg, out, a.dry_run, a.jobs)
    if a.step == 'plugins' and a.revision is None:
        sys.exit('plugins needs --revision N')
    if a.revision is not None and a.step in ('plugins', 'all'):
        if not 170 <= a.revision <= 9999:
            sys.exit('--revision must be 170..9999')
        b.plugins(a.revision, a.panvk)
    if a.step == 'plugins':
        b.log(f'output: {out}')
        return
    kernel = a.kernel
    if a.step in ('all', 'kernel'):
        b.compile()
        kernel = b.link()
    if a.step in ('all', 'boot'):
        if kernel is None:
            sys.exit('boot step needs --kernel or a kernel step')
        for key in ('llvm_clang', 'lld'):
            if not cfg.get(key):
                sys.exit(f'{a.config}: {key} is required to package boot.img')
        b.boot(kernel, a.boot_config, a.boot_arg)
    b.log(f'output: {out}')


if __name__ == '__main__':
    main()
