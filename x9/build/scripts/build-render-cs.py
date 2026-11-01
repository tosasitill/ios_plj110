#!/usr/bin/env python3
"""Generate native CS root/body packing from the pinned Mesa v14 XML."""
from pathlib import Path
import xml.etree.ElementTree as E
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
root=w.parents[1]
xml=E.parse(w/'mesa-render-source/src/panfrost/genxml/v14.xml').getroot()
enums={e.get('name'):{v.get('name'):int(v.get('value'),0) for v in e.findall('value')} for e in xml.findall('enum')}
structs={s.get('name'):s for s in xml.findall('struct')}
out=w/'render-kernel-generated';out.mkdir(exist_ok=True)
lines=['/* Generated normal build input: frozen Mesa v14 CS XML. */','#pragma once','#include <stdint.h>']
def emit(name,arguments,constants):
 s=structs['CS '+name];terms=[]
 for f in s.findall('field'):
  key=f.get('name');v=arguments.get(key,constants.get(key,f.get('default','0')))
  if isinstance(v,str) and v in enums.get(f.get('type'),{}):v=enums[f.get('type')][v]
  shift=int(f.get('start'));width=int(f.get('size'))
  if isinstance(v,str) and v in arguments.values():terms.append('((uint64_t)('+v+')<<'+str(shift)+')')
  else:
   v=int(v,0) if isinstance(v,str) else v
   if v<0 or v>=1<<width:raise RuntimeError('field overflow '+name+'/'+key)
   if v:terms.append(hex(v<<shift)+'ULL')
 params=','.join('uint64_t '+v for v in arguments.values())
 lines.append('static inline uint64_t x9_cs_'+name.lower()+'('+params+') { return '+'|'.join(terms or ['0'])+'; }')
emit('MOVE48',{'Destination':'reg','Immediate':'value'},{})
emit('MOVE32',{'Destination':'reg','Immediate':'value'},{})
emit('CALL',{'Address':'address','Length':'length'},{})
emit('WAIT',{'Wait mask':'mask'},{})
emit('FLUSH_CACHE2',{'Latest Flush ID':'reg'}, {'L2 Flush Mode':'Clean and invalidate','LSC Flush Mode':'Clean and invalidate','Other Flush Mode':'Invalidate','Signal slot':0})
emit('SYNC_SET64',{'Address':'address','Data':'data'}, {'Signal slot':0})
(out/'x9_render_cs.inc').write_text('\n'.join(lines)+'\n')
print('Native CSF packing generated from pinned v14 XML',flush=True)
