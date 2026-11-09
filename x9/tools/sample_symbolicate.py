import re,sys,struct,os,subprocess,bisect,collections
LOG=sys.argv[1];PROC=sys.argv[2];ROOT=sys.argv[3]
slide=None;mh=None;rows=[];rnd=0
for l in open(LOG,errors='replace'):
    if 'X9 SAMPLE: begin' in l: rnd=int(l.split('begin')[1])
    m=re.search(r'SAMPLE: %s\[(\d+)\] slide=(0x[0-9a-f]+) mh=(0x[0-9a-f]+)'%PROC,l)
    if m: slide=int(m.group(2),16);mh=int(m.group(3),16);continue
    m=re.search(r'SAMPLE: %s\[\d+\] (t\d+) (.*)'%PROC,l)
    if m:
        d=m.group(2);pc=re.search(r'pc=(0x[0-9a-f]+)',d);lr=re.search(r'lr=(0x[0-9a-f]+)',d);bt=re.search(r'bt ((?:0x[0-9a-f]+ ?)*)',d)
        sl=re.search(r'sleep=(\d+)',d);run=re.search(r'run=(\d+)',d)
        frames=[int(pc.group(1),16)]+([int(lr.group(1),16)] if lr else [])+([int(x,16) for x in bt.group(1).split()] if bt else [])
        rows.append((rnd,m.group(1),run.group(1) if run else '?',frames))
print('slide',hex(slide),'rows',len(rows),file=sys.stderr)
# image map
imgs=[]
for dp,_,fs in os.walk(ROOT):
    for f in fs:
        p=os.path.join(dp,f)
        try:
            with open(p,'rb') as fh:
                h=fh.read(32)
                if h[:4]!=b'\xcf\xfa\xed\xfe':continue
                nc,sz=struct.unpack_from('<II',h,16)
                fh.seek(32);cmds=fh.read(sz);o=0;lo=None
                for _ in range(nc):
                    c,cs=struct.unpack_from('<II',cmds,o)
                    if c==0x19:
                        nm=cmds[o+8:o+24].rstrip(b'\0');va,vs=struct.unpack_from('<QQ',cmds,o+24)
                        if nm==b'__TEXT':lo=(va,va+vs)
                    o+=cs
                if lo:imgs.append((lo[0],lo[1],p))
        except Exception as e:pass
imgs.sort()
def find(a):
    i=bisect.bisect_right([x[0] for x in imgs],a)-1
    return imgs[i] if i>=0 and a<imgs[i][1] else None
symcache={}
def syms(p):
    if p not in symcache:
        out=subprocess.run(['llvm-nm','-n','--defined-only',p],capture_output=True,text=True).stdout
        L=[]
        for ln in out.splitlines():
            t=ln.split(None,2)
            if len(t)==3:
                try:L.append((int(t[0],16),t[2]))
                except:pass
        symcache[p]=(L,[x[0] for x in L])
    return symcache[p]
def sym(a):
    u=a-slide
    im=find(u)
    if not im:
        return '%#x (exe+%#x)'%(a,a-mh) if mh<=a<mh+0x2000000 else '%#x ?'%a
    L,ks=syms(im[2]);i=bisect.bisect_right(ks,u)-1
    n=L[i][1] if i>=0 else '?'
    return '%s`%s+%#x'%(os.path.basename(im[2]),n,u-L[i][0] if i>=0 else 0)
want=int(sys.argv[4]) if len(sys.argv)>4 else 0
for rnd,t,run,fr in rows:
    if want and rnd!=want:continue
    print('R%d %s run=%s'%(rnd,t,run))
    for a in fr[:int(sys.argv[5]) if len(sys.argv)>5 else 12]:print('    ',sym(a))
