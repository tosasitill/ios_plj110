#!/usr/bin/env python3
"""Compile native PanVK memory ops against the frozen iOS Mesa build headers."""
from pathlib import Path
import json,shlex,subprocess
w=Path(__file__).resolve().parent
root=w.parents[1]
entries=json.loads((w/'build-panvk-ios/compile_commands.json').read_text())
entry=next(e for e in entries if e['file'].endswith('/panthor_kmod.c'))
out=w/'render-user-objects';out.mkdir(exist_ok=True)
objects=[];commands=[]
for name in ['X9PanVKMemory','X9PanVKSync','X9PanVKTilerHeap','X9PanVKSubmitPlan','X9PanVKQueue']:
 args=shlex.split(entry['command'])
 obj=out/(name+'.o')
 for flag,suffix in [('-o',''),('-MQ',''),('-MF','.d')]:args[args.index(flag)+1]=str(obj)+suffix
 args[args.index('-c')+1]=str(root/'MaliMetal/X9/General/panvk'/(name+'.c'))
 commands.append({'args':args,'cwd':entry['directory']})
 subprocess.run(args,cwd=entry['directory'],check=True)
 objects.append(str(obj))
(out/'panvk-native-command.json').write_text(json.dumps(commands,indent=2)+'\n')
# Compile transport as a normal archive dependency, without relying on a
# previously built object or adding optional unresolved-symbol linker flags.
dev=Path('/Users/tosasitill/Downloads/Xcode.app/Contents/Developer')
tools=dev/'Toolchains/XcodeDefault.xctoolchain/usr/bin'
sdk=dev/'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
transport=out/'X9PanVKTransport.o'
subprocess.run([str(tools/'clang'),'-target','arm64-apple-ios16.0','-isysroot',str(sdk),'-std=c11','-O2','-Wall','-Wextra','-Werror','-c',str(root/'MaliMetal/X9/General/X9RenderTransport.c'),'-o',str(transport)],check=True)
subprocess.run([str(tools/'libtool'),'-static','-o',str(out/'libX9PanVKNative.a'),*objects,str(transport)],check=True)
print('PanVK native memory/group/submission adapter archive built for iOS16/arm64; not registered in device discovery; queue call-site/tiler/hardware engine integration remains required.',flush=True)
