#!/usr/bin/env python3
"""Package the just-linked native ICD for its Metal runtime path. No deployment."""
from pathlib import Path
import shutil,subprocess,plistlib,argparse
# Machine paths: environment, then x9/build/local.json, then the defaults.
import json as _json, os as _os
from pathlib import Path as _Path
_local = _Path(__file__).resolve().parents[1] / 'local.json'
_cfg = _json.loads(_local.read_text()) if _local.is_file() else {}
DEV = _os.environ.get('DEVELOPER_DIR') or _cfg.get('developer_dir') or '/Applications/Xcode.app/Contents/Developer'
BREW = _os.environ.get('X9_HOMEBREW') or _cfg.get('homebrew') or '/opt/homebrew'
ap=argparse.ArgumentParser();ap.add_argument("--revision",type=int,default=170)
ap.add_argument("--source-library",type=Path,help="Repackage a specific existing ICD without rebuilding or changing its driver code")
opts=ap.parse_args()
if not 170<=opts.revision<=9999:ap.error("revision must be170..9999")
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
source=opts.source_library.resolve() if opts.source_library else w/'build-panvk-native-ios/src/panfrost/vulkan/libvulkan_panfrost.dylib'
bundle=w/f'X9PanVK{opts.revision}.bundle';bundle.mkdir(exist_ok=True)
executable=bundle/'X9PanVK'
shutil.copyfile(source,executable)
subprocess.run(['/usr/bin/install_name_tool','-id',f'/System/Library/Extensions/X9PanVK{opts.revision}.bundle/X9PanVK',str(executable)],check=True)
(bundle/'Info.plist').write_bytes(plistlib.dumps({
 'CFBundleIdentifier':'org.heliodarwin.X9PanVK'+str(opts.revision),'CFBundleExecutable':'X9PanVK',
 'CFBundlePackageType':'BNDL','CFBundleVersion':'1','MinimumOSVersion':'16.0',
},fmt=plistlib.FMT_XML))
subprocess.run([DEV+'/Toolchains/XcodeDefault.xctoolchain/usr/bin/strip','-S',str(executable)],check=True)
subprocess.run(['/usr/bin/codesign','--force','--sign','-','--pagesize','16384',str(executable)],check=True)
print('Native PanVK bundle packaged and ad-hoc signed; not deployed, loaded or hardware-validated.',flush=True)
