#!/usr/bin/env python3
"""Read selected iOS diagnostic directories via the existing read-only rawreadat helper."""
import json,struct,subprocess,shlex,time,re,unicodedata
from pathlib import Path
BASE=Path('/Users/tosasitill/HelioDarwin/logs')/time.strftime('x9-about-hfs-%Y%m%d-%H%M%S')
BASE.mkdir()
m=json.loads(Path('/Users/tosasitill/HelioDarwin-x9-lk-preserve/x9/out/pan138-touch-boundaries/root-manifest.json').read_text())
image=m['device_file'];tool=str(Path(image).parent/'pinfile');q=shlex.quote
header=Path('/Users/tosasitill/HelioDarwin/logs/x9-about-pan138-root-header-20261028-0119.raw').read_bytes()
bs=struct.unpack_from('>I',header,1064)[0]
cache={}
def raw(off,n):
 start=off//4096*4096;end=(off+n+4095)//4096*4096;key=(start,end)
 if key not in cache:
  cmd=q(tool)+' rawreadat '+q(image)+' /dev/block/sdc100 '+str(start)+' '+str(end-start)+' 2>/dev/null'
  b=subprocess.check_output(['adb','-s',m['device_serial'],'exec-out','su','-c',cmd],timeout=25)
  if len(b)!=end-start:raise ValueError('short raw capture')
  cache[key]=b
  (BASE/(str(start)+'-'+str(end)+'.raw')).write_bytes(b)
 return cache[key][off-start:off-start+n]
def fork_info(b,o):
 return struct.unpack_from('>Q',b,o)[0],[struct.unpack_from('>II',b,o+16+i*8) for i in range(8)]
def fork_read(f,off,n):
 size,ext=f
 if not 0<=off<=off+n<=size:raise ValueError('fork range')
 out=b'';logical=0
 for start,count in ext:
  amount=count*bs
  if off<logical+amount and len(out)<n:
   within=max(0,off-logical);take=min(amount-within,n-len(out))
   out+=raw(start*bs+within,take);off+=take
  logical+=amount
 if len(out)!=n:raise ValueError('overflow extents needed')
 return out
catalog=fork_info(header,1024+272)
h=fork_read(catalog,0,256);nodesize=struct.unpack_from('>H',h,32)[0];root=struct.unpack_from('>I',h,16)[0]
print('OUTPUT',BASE,'catalog_size',catalog[0],'node_size',nodesize,'root',root,'keyCompare',hex(h[51]),flush=True)
if h[51]!=0xbc:raise ValueError('expected binary HFSX key comparator')
def node(num):
 b=fork_read(catalog,num*nodesize,nodesize);kind=b[8];count=struct.unpack_from('>H',b,10)[0]
 if count>(nodesize-14)//2-1:raise ValueError('record count')
 records=[]
 for i in range(count):
  off=struct.unpack_from('>H',b,nodesize-2*(i+1))[0];end=struct.unpack_from('>H',b,nodesize-2*(i+2))[0]
  if not 14<=off<end<=nodesize-2*(count+1):raise ValueError('record bounds')
  kl,parent,nchars=struct.unpack_from('>HIH',b,off);body=off+2+kl
  if kl!=6+2*nchars or body+2>end:raise ValueError('catalog key')
  name=b[off+8:body];records.append(((parent,name),b[body:end]))
 return kind,struct.unpack_from('>I',b,0)[0],records
def leaf_for(key):
 num=root;seen=set()
 while True:
  if num in seen:raise ValueError('node cycle')
  seen.add(num);kind,nxt,rows=node(num)
  if kind==255:return num,nxt,rows
  if kind!=0:raise ValueError('not index/leaf')
  choices=[v for k,v in rows if k<=key]
  if not choices:return 0,0,[]
  num=struct.unpack_from('>I',choices[-1],0)[0]
def find(parent,name):
 key=(parent,unicodedata.normalize('NFD',name).encode('utf-16be'));_,_,rows=leaf_for(key)
 return next((v for k,v in rows if k==key),None)
def folder(path):
 cnid=2
 for part in path.strip('/').split('/'):
  b=find(cnid,part)
  if b is None:return None
  if struct.unpack_from('>H',b,0)[0]!=1:return None
  cnid=struct.unpack_from('>I',b,8)[0]
 return cnid
def listing(cnid):
 num,nxt,rows=leaf_for((cnid,b''));seen=set();out=[]
 while num:
  if num in seen:raise ValueError('leaf cycle')
  seen.add(num)
  for (parent,name),b in rows:
   if parent>cnid:return out
   if parent==cnid and struct.unpack_from('>H',b,0)[0] in (1,2):out.append((name.decode('utf-16be'),b))
  num=nxt
  if num:_,nxt,rows=node(num)
 return out
summary=[]
for path in ['/private/var/mobile/Library/Logs/CrashReporter','/private/var/root/Library/Logs/CrashReporter','/Library/Logs/CrashReporter','/private/var/mobile/Library/Logs/CrashReporter/DiagnosticLogs','/private/var/log']:
 cnid=folder(path);print('DIRECTORY',path,'cnid',cnid,flush=True)
 if cnid is None:continue
 rows=listing(cnid);print('ENTRIES',len(rows),flush=True)
 for name,b in rows:
  kind=struct.unpack_from('>H',b,0)[0];size=fork_info(b,88)[0] if kind==2 else 0
  item={'path':path+'/'+name,'kind':kind,'size':size,'cnid':struct.unpack_from('>I',b,8)[0]};summary.append(item)
  print('FILE',name,'kind',kind,'size',size,flush=True)
  if kind==2 and re.search(r'Preferences|Settings|SpringBoard|Jetsam|hang|stall|spin|panic|syslog',name,re.I) and 0<size<=8*1024*1024:
   data=fork_read(fork_info(b,88),0,size);dest=BASE/('file-'+str(item['cnid'])+'-'+re.sub(r'[^A-Za-z0-9._-]','_',name));dest.write_bytes(data);print('EXTRACTED',dest,flush=True);item['artifact']=str(dest)
(BASE/'directory-entries.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')
print('COMPLETE',len(summary),'records',len(cache),'raw ranges',flush=True)
