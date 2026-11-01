#!/usr/bin/env python3
"""Build the X9 kernel and boot.img from this repository.

    python3 -I x9/build/build.py                 # kernel + boot.img
    python3 -I x9/build/build.py kernel          # compile + link + strip only
    python3 -I x9/build/build.py boot --kernel K # package an existing kernel
    python3 -I x9/build/build.py --dry-run       # print every command

Inputs:
  x9/kernel/recipe.json      compile templates, per-object sources, link/strip
  x9/kernel/link-order.txt   exact link order: xnu:<object in the frozen XNU
                             tree> or x9:<overlay object>
  x9/kernel/src, generated   overlay sources (XNU-relative paths)
  x9/kernel/prebuilt         objects whose compile command is not known yet
  x9/build/boot.json         packaging options and boot-args
  x9/build/local.json        machine paths (copy config.example.json)

Outputs go to x9/out/<name>/ (default name: x9-<git short hash>[-dirty]).
Compiles are incremental through compiler depfiles. Nothing touches a device.
"""
import argparse
import concurrent.futures
import json
import os
import shlex
import subprocess
import sys
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

    def sub(self, text):
        for key, value in self.vars.items():
            text = text.replace(key, value)
        return text

    def run(self, cmd, cwd, env=None):
        print('+', f'(cd {shlex.quote(str(cwd))} &&', ' '.join(shlex.quote(c) for c in cmd) + ')', flush=True)
        if not self.dry_run:
            subprocess.run(cmd, cwd=cwd, env=env or self.env, check=True)

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
        cmd = [self.sub(t['compiler'])] + [self.sub(a) for a in t['args']] + \
              ['-MD', '-MF', str(dep), '-c', str(src), '-o', str(obj)]
        self.run(cmd, self.sub(t['cwd']))
        return True

    def compile(self):
        if not self.dry_run:
            self.objdir.mkdir(parents=True, exist_ok=True)
        todo = [e for e in self.recipe['objects'] if 'prebuilt' not in e]
        with concurrent.futures.ThreadPoolExecutor(self.jobs) as pool:
            built = sum(pool.map(self.compile_one, todo))
        print(f'compiled {built}/{len(todo)} overlay objects '
              f'({len(self.recipe["objects"]) - len(todo)} prebuilt)')

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
            sys.exit(f'{len(missing)} frozen XNU objects missing, e.g. {missing[0]}')
        filelist = self.out / 'kernel-link.filelist'
        unstripped = self.out / 'kernel.development.sweet.unstripped'
        kernel = self.out / 'kernel.development.sweet'
        if not self.dry_run:
            filelist.write_text('\n'.join(paths) + '\n')
        link = self.recipe['link']
        self.vars.update({'{FILELIST}': str(filelist), '{UNSTRIPPED}': str(unstripped)})
        self.run([self.sub(link['compiler'])] + [self.sub(a) for a in link['args']], self.sub(link['cwd']))
        strip = self.recipe['strip']
        self.run([self.sub(strip['tool'])] + strip['args'] + [str(unstripped), '-o', str(kernel)], self.out)
        return kernel

    # ---- boot.img --------------------------------------------------------------
    def boot(self, kernel):
        opts = json.loads((HERE / 'boot.json').read_text())
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
        self.run(cmd, ROOT, env)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('step', nargs='?', choices=['all', 'kernel', 'boot'], default='all')
    ap.add_argument('--config', type=Path, default=HERE / 'local.json')
    ap.add_argument('--name', help='output directory name under x9/out')
    ap.add_argument('--kernel', type=Path, help='boot step only: kernel to package')
    ap.add_argument('--dry-run', action='store_true', help='print commands, run nothing')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    cfg = load_config(a.config)
    out = X9 / 'out' / (a.name or default_name())
    if not a.dry_run:
        out.mkdir(parents=True, exist_ok=True)
    b = Builder(cfg, out, a.dry_run, a.jobs)
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
        b.boot(kernel)
    print(f'output: {out}')


if __name__ == '__main__':
    main()
