#!/usr/bin/env python3
"""Build the native iOS resource/transport archive, without executing it."""
from pathlib import Path
import subprocess
w=Path(__file__).resolve().parent
root=w.parents[1]
src=root/'MaliMetal/X9/General'
out=w/'render-user-objects';out.mkdir(exist_ok=True)
dev=Path('/Users/tosasitill/Downloads/Xcode.app/Contents/Developer')
tools=dev/'Toolchains/XcodeDefault.xctoolchain/usr/bin'
sdk=dev/'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
objects=[]
for name in ['X9RenderTransport','X9RenderResources']:
 obj=out/(name+'.o')
 subprocess.run([str(tools/'clang'),'-target','arm64-apple-ios16.0','-isysroot',str(sdk),'-std=c11','-O2','-Wall','-Wextra','-Werror','-c',str(src/(name+'.c')),'-o',str(obj)],check=True)
 objects.append(str(obj))
subprocess.run([str(tools/'libtool'),'-static','-o',str(out/'libX9RenderResources.a'),*objects],check=True)
print('iOS16/arm64 native resource and transport archive built; no runtime or PanVK integration claim.',flush=True)
