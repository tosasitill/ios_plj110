from pathlib import Path
import struct,json,capstone
src=Path('/Users/tosasitill/Downloads/01-Android/内测/x9内测/final/x9/partitions/lk_a.img')
b=src.read_bytes();code=b[0x200:0x1c0550];out=Path(__file__).parent
wanted=[b'[USB] U2 pullup',b'[USB] U3 pullup',b'[USB] IP version',b'[USB] usb_online',b'usb_init\0',b'[USB] Refer clocks',b'jump to linux kernel 64Bit']
targets={b.index(s)-0x200:s.decode(errors='replace') for s in wanted if s in b}
refs=[]
for off in range(0,len(code)-4,4):
 word=struct.unpack_from('<I',code,off)[0]
 if word&0x9f000000!=0x90000000:continue
 reg=word&31;imm=(((word>>5)&0x7ffff)<<2)|((word>>29)&3)
 if imm&(1<<20):imm-=1<<21
 page=(off&~4095)+(imm<<12)
 for ahead in range(4,28,4):
  if off+ahead+4>len(code):break
  nxt=struct.unpack_from('<I',code,off+ahead)[0]
  if nxt&0xffc00000==0x91000000 and ((nxt>>5)&31)==reg:
   address=page+((nxt>>10)&4095)
   if address in targets:refs.append({'adrp':hex(off),'add':hex(off+ahead),'string_rva':hex(address),'text':targets[address]})
md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM)
lines=[]
for r in refs:
 at=int(r['adrp'],16);lo=max(0,at-100);hi=min(len(code),at+240)
 lines.append(str(r))
 for ins in md.disasm(code[lo:hi],lo):lines.append(f'{ins.address:08x}  {ins.mnemonic:8s} {ins.op_str}')
 lines.append('')
(out/'usb-string-xrefs.json').write_text(json.dumps({'source':str(src),'address_kind':'RVA from LK text at file offset 0x200','refs':refs},indent=2)+'\n')
(out/'usb-string-xrefs.asm').write_text('\n'.join(lines))
print(json.dumps(refs,indent=2))
