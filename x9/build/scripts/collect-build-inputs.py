#!/usr/bin/env python3
"""Collect everything needed to reproduce one X9 candidate kernel link.

Run on the build Mac (stdlib only, read-only on the workspace):

    python3 -I collect-build-inputs.py
    python3 -I collect-build-inputs.py /path/to/x9/out/pan220-batched-cache-ranges

Starting from the candidate's kernel-link.filelist and commands.json it finds,
for every linked object that is not part of the frozen XNU object tree, the
candidate directory that produced it, the compile command that built it, its
compiler dependency file and every workspace source/header it depends on. It
also records the frozen XNU tree, the XNU source git state, every candidate's
build recipe and the tool versions.

Never collected: boot images, payloads, kernels, logs, root manifests (they
hold the device serial and the userdata extent map) and firmware blobs.
The XNU objects are only listed unless --with-xnu-objects is given.
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import re
import shlex
import subprocess
import sys
import tarfile
from pathlib import Path

DEFAULT_CANDIDATE = str(Path(__file__).resolve().parents[3] / '.local/x9/x9/out/pan220-batched-cache-ranges')
RECIPE_FILES = ["commands.json", "kernel-compile-commands.json", "kernel-link.filelist", "build.sh", "build-kernel.py",
                "boot-args.txt", "boot.json", "cache-batch-inputs.json",
                "external-metal-verification.patch"]
NEVER = re.compile(r"(root-manifest|boot\.img|payload\.bin|kernel\.development|\.adt$|"
                   r"\.log$|\.raw$|pstore|disk\.|console\.bin|\.tar\.gz$|\.DS_Store$|__pycache__)")
MAX_FILE = 64 << 20


class Collector:
    def __init__(self, args):
        self.args = args
        self.maps = [tuple(m.split("=", 1)) for m in args.path_map]
        self.files = {}      # archive name -> local path
        self.missing = []
        self.notes = []

    def local(self, path):
        """Mac path -> readable local path (identity on the Mac)."""
        for old, new in self.maps:
            if path == old or path.startswith(old.rstrip("/") + "/"):
                return new + path[len(old):]
        return path

    def add(self, path, name=None):
        if NEVER.search(path):
            return False
        lp = Path(self.local(path))
        if not lp.is_file():
            self.missing.append(path)
            return False
        if lp.stat().st_size > MAX_FILE:
            self.notes.append(f"skipped >64MiB: {path}")
            return False
        self.files[name or "files/" + path.lstrip("/")] = lp
        return True

    def add_bytes(self, name, data):
        self.files[name] = data if isinstance(data, bytes) else data.encode()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_depfile(path):
    text = Path(path).read_text(errors="replace").replace("\\\n", " ")
    deps = []
    for line in text.splitlines():
        if ":" not in line:
            continue
        _, rest = line.split(":", 1)
        deps += [d.replace("\\ ", " ") for d in re.split(r"(?<!\\)\s+", rest.strip()) if d]
    return deps


def dwarf_paths(path, workspace):
    """Workspace source paths named in an object's debug info (compiled -g).
    Line tables often name headers relative to the compile directory (the
    home directory here), e.g. "HelioDarwin-x9-lk-preserve/x9/...h"."""
    data = Path(path).read_bytes()
    parent, base = workspace.rsplit("/", 1)
    pat = rb"(?:" + re.escape(parent.encode()) + rb"/)?" + re.escape(base.encode()) + \
        rb"/[\x21-\x7e]+?\.(?:c|cc|cpp|s|S|m|mm|h|hpp|inc)(?=[\x00\s])"
    found = set()
    for m in re.findall(pat, data):
        p = m.decode()
        found.add(p if p.startswith("/") else parent + "/" + p)
    return sorted(found)


def output_of(record):
    a = record.get("args", [])
    return a[a.index("-o") + 1] if "-o" in a and a.index("-o") + 1 < len(a) else None


def sources_of(record):
    return [x for x in record.get("args", []) if re.search(r"\.(c|cc|cpp|s|S|m|mm)$", x)]


def pan_number(name):
    m = re.match(r"pan(\d+)", name or "")
    return int(m.group(1)) if m else -1


def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout.strip()
    except Exception as e:  # tool missing on this host
        return f"<unavailable: {e}>"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("candidate", nargs="?", default=DEFAULT_CANDIDATE)
    ap.add_argument("--out", help="output .tar.gz (default: ./x9-build-inputs-<candidate>-<date>.tar.gz)")
    ap.add_argument("--with-xnu-objects", action="store_true",
                    help="also archive the frozen XNU objects named by the link filelist")
    ap.add_argument("--path-map", action="append", default=[], metavar="MAC=LOCAL",
                    help="read MAC path prefix from LOCAL (only for running off the build Mac)")
    args = ap.parse_args()
    c = Collector(args)

    cand = args.candidate.rstrip("/")
    lcand = Path(c.local(cand))
    if not (lcand / "kernel-link.filelist").is_file():
        sys.exit(f"not a candidate directory (no kernel-link.filelist): {lcand}")
    m = re.match(r"(.*)/x9/out/([^/]+)$", cand)
    if not m:
        sys.exit("candidate must be <workspace>/x9/out/<name>")
    workspace, name = m.group(1), m.group(2)
    outdir = workspace + "/x9/out"

    # Every candidate's compile records, indexed by output object.
    # Objects are often copied forward (cp -cR kernel-objects) from the
    # candidate that compiled them, so also index by object file name.
    producers, by_name = {}, {}
    louts = Path(c.local(outdir))
    chains = sorted((p for p in louts.iterdir() if p.is_dir()), key=lambda p: pan_number(p.name)) \
        if louts.is_dir() else []
    if lcand not in chains:
        chains.append(lcand)
    for d in chains:
        for f in d.glob("*.json"):
            try:
                data = json.loads(f.read_text())
            except Exception:
                continue
            if isinstance(data, list):
                for r in data:
                    if isinstance(r, dict) and output_of(r):
                        producers.setdefault(output_of(r), []).append((d.name, f.name, r))
                        by_name.setdefault(Path(output_of(r)).name, []).append((d.name, f.name, r))
        for n in RECIPE_FILES + ["prepare.py"] + [p.name for p in d.glob("prepare*.py")]:
            p = d / n
            if p.is_file() and not NEVER.search(n):
                c.files[f"chain/{d.name}/{n}"] = p

    for n in RECIPE_FILES + ["prepare.py"]:
        if (lcand / n).is_file():
            c.files[f"candidate/{n}"] = lcand / n
    for sub in ["generated", "compute-source", "build-tools", "kernel-source", "metal-source",
                "packaging-source"]:
        for p in sorted((lcand / sub).rglob("*")) if (lcand / sub).is_dir() else []:
            if p.is_file() and not NEVER.search(str(p)) and p.stat().st_size <= MAX_FILE:
                c.files[f"candidate/{sub}/{p.relative_to(lcand / sub)}"] = p
    pp = lcand / "plugin-package"
    for n in ["manifest.json", "mailbox.json"]:
        if (pp / n).is_file():
            c.files[f"candidate/plugin-package/{n}"] = pp / n

    # Classify the link inputs.
    linked = (lcand / "kernel-link.filelist").read_text().split()
    xnu_objs, overlay = [], []
    for o in linked:
        (xnu_objs if "/xnu.obj/" in o else overlay).append(o)
    xnu_obj_root = re.sub(r"(/xnu\.obj/[^/]+)/.*", r"\1", xnu_objs[0]) if xnu_objs else None
    xnu_src_root = re.sub(r"/build[^/]*/.*", "", xnu_objs[0]) if xnu_objs else None

    objects = []
    for o in overlay:
        entry = {"object": o, "candidate": None, "command": None, "sources": [], "depfile": None,
                 "workspace_deps": [], "other_deps": 0}
        mm = re.match(re.escape(outdir) + r"/([^/]+)/", o)
        entry["candidate"] = mm.group(1) if mm else None
        lo = Path(c.local(o))
        entry["object_sha256"] = sha256(lo) if lo.is_file() else None
        c.add(o)
        found = producers.get(o, [])
        how = "exact"
        if not found:
            # Latest candidate at or before the one that holds the object.
            limit = pan_number(entry["candidate"]) if entry["candidate"] else pan_number(name)
            found = [f for f in by_name.get(lo.name, []) if pan_number(f[0]) <= limit]
            how = "by-name"
        if found:
            pan, file, record = found[-1]
            entry["command"] = {"from": f"{pan}/{file}", "match": how, "cwd": record.get("cwd"),
                                "args": record["args"]}
            entry["sources"] = sources_of(record)
        for dep in [o + ".d", re.sub(r"\.(o|cpo)$", ".d", o), re.sub(r"\.cpp\.o$", ".cpp.d", o)]:
            if dep != o and dep.endswith(".d") and Path(c.local(dep)).is_file():
                entry["depfile"] = dep
                c.add(dep)
                deps = read_depfile(c.local(dep))
                entry["depfile_source"] = deps[0] if deps else None
                for d in deps:
                    if d.startswith(workspace) or "/MaliMetal/" in d:
                        entry["workspace_deps"].append(d)
                        c.add(d)
                    else:
                        entry["other_deps"] += 1
                break
        for s in entry["sources"]:
            c.add(s)
        # Debug info names the file that was really compiled; a by-name
        # command can point at a later copy. Collect it plus its directory's
        # depfiles and command records when no depfile exists.
        if lo.is_file():
            entry["dwarf_sources"] = dwarf_paths(lo, workspace)
            for p in entry["dwarf_sources"]:
                if c.add(p) and not entry["depfile"]:
                    for sib in Path(c.local(p)).parent.glob("*"):
                        if sib.suffix in (".d", ".json") and sib.is_file():
                            c.add(str(sib).replace(c.local(p).rsplit("/", 1)[0], p.rsplit("/", 1)[0], 1))
        # Without a depfile the header set is unknown (debug info omits
        # macro-only headers): take the headers next to the source and in
        # every workspace -I directory of its command, non-recursively.
        if not entry["depfile"]:
            dirs = {s.rsplit("/", 1)[0] for s in entry["sources"] + entry.get("dwarf_sources", [])
                    if re.search(r"\.(c|cc|cpp|s|S)$", s)}
            for a in (entry["command"] or {}).get("args", []):
                if a.startswith("-I" + workspace):
                    dirs.add(a[2:].rstrip("/"))
            entry["header_dirs"] = sorted(dirs)
            for d in dirs:
                ld = Path(c.local(d))
                for h in sorted(ld.glob("*")) if ld.is_dir() else []:
                    if h.suffix in (".h", ".hpp", ".inc") and h.is_file():
                        c.add(d + "/" + h.name)
        objects.append(entry)

    if args.with_xnu_objects:
        for o in xnu_objs:
            c.add(o)

    # XNU source state: the frozen object tree was built from this checkout.
    xnu = {"object_root": xnu_obj_root, "source_root": xnu_src_root, "objects": len(xnu_objs)}
    # The compiles include the XNU tree under the build directory (-I .../dxb/xnu/...),
    # which may differ from the checkout the object tree is named after.
    roots = {xnu_src_root} if xnu_src_root else set()
    for e in objects:
        for a in (e["command"] or {}).get("args", []):
            mm = re.match(r"-I(.*?/dxb/xnu)(/|$)", a)
            if mm:
                roots.add(mm.group(1))
    xnu["source_trees"] = {}
    for i, root in enumerate(sorted(roots)):
        info = {}
        if Path(c.local(root)).is_dir():
            g = ["git", "-C", c.local(root)]
            info["toplevel"] = run(g + ["rev-parse", "--show-toplevel"])
            info["head"] = run(g + ["rev-parse", "HEAD"])
            info["describe"] = run(g + ["log", "-1", "--format=%H %ad %s"])
            info["status"] = run(g + ["status", "--porcelain", "--untracked-files=normal", "--", "."])
            diff = run(g + ["diff", "HEAD", "--", "."])
            if diff and not diff.startswith("<unavailable"):
                c.add_bytes(f"xnu/tree{i}-working-tree.diff", diff)
            # Untracked kernel sources (e.g. added sweet_* files), text only.
            for line in info["status"].splitlines():
                if line.startswith("?? "):
                    rel = line[3:].strip('"')
                    if re.search(r"\.(c|cpp|h|s|inc|exports|txt)$|/$", rel) and not NEVER.search(rel):
                        lp = Path(c.local(root)) / rel
                        for f in ([lp] if lp.is_file() else lp.rglob("*") if lp.is_dir() else []):
                            if f.is_file() and f.stat().st_size <= 4 << 20:
                                c.files[f"xnu/tree{i}-untracked/{f.relative_to(c.local(root))}"] = f
        else:
            info["present"] = False
        xnu["source_trees"][root] = info
    lxo = Path(c.local(xnu_obj_root)) if xnu_obj_root else None
    if lxo and lxo.is_dir():
        listing = []
        for o in xnu_objs:
            lp = Path(c.local(o))
            listing.append({"object": o, "bytes": lp.stat().st_size if lp.is_file() else None})
        c.add_bytes("xnu/linked-objects.json", json.dumps(listing, indent=1))
        hdrs = lxo.parent / "EXPORT_HDRS"
        xnu["export_hdrs_present"] = hdrs.is_dir()
    for p in sorted(Path(c.local(workspace + "/xnu/patches")).glob("*.patch")):
        c.files[f"xnu/workspace-patches/{p.name}"] = p

    # Tools named by the recorded commands.
    tools = {}
    rec = json.loads((lcand / "commands.json").read_text())
    for r in rec:
        exe = r["args"][0]
        if exe not in tools:
            tools[exe] = run([exe, "--version"]) if Path(exe).exists() else "<missing>"
    host = {"date": datetime.datetime.now().isoformat(timespec="seconds"),
            "python": sys.version, "uname": " ".join(os.uname()),
            "sw_vers": run(["sw_vers"]), "xcodebuild": run(["xcodebuild", "-version"]),
            "tools": tools}
    build_sh = (lcand / "build.sh").read_text() if (lcand / "build.sh").is_file() else ""
    for word in shlex.split(build_sh, comments=True, posix=True) if build_sh else []:
        for part in word.split("="):
            if part.startswith("/") and ":" not in part and ("lld" in part or "llvm" in part or part.endswith("python3")):
                host["tools"].setdefault(part, run([part, "--version"]) if Path(part).exists() else "<missing>")

    summary = {"candidate": name, "workspace": workspace, "linked_objects": len(linked),
               "overlay_objects": len(overlay), "xnu": xnu, "host": host,
               "overlay_without_command": [e["object"] for e in objects if not e["command"]],
               "overlay_without_depfile": [e["object"] for e in objects if not e["depfile"]],
               "missing_files": sorted(set(c.missing)), "notes": c.notes, "objects": objects}
    c.add_bytes("manifest.json", json.dumps(summary, indent=1))

    out = args.out or f"x9-build-inputs-{name}-{datetime.date.today():%Y%m%d}.tar.gz"
    with tarfile.open(out, "w:gz") as tar:
        for arc, src in sorted(c.files.items()):
            if isinstance(src, bytes):
                info = tarfile.TarInfo(arc)
                info.size = len(src)
                tar.addfile(info, io.BytesIO(src))
            else:
                tar.add(src, arcname=arc, recursive=False)
    print(f"{out}: {len(c.files)} files")
    print(f"overlay objects {len(overlay)}: without compile command {len(summary['overlay_without_command'])}, "
          f"without depfile {len(summary['overlay_without_depfile'])}; missing files {len(summary['missing_files'])}")


if __name__ == "__main__":
    main()
