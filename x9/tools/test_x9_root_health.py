#!/usr/bin/env python3
"""Exercise the actual shim HFS attribute gate with representative headers."""
import pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
class HealthTest(unittest.TestCase):
 def test_actual_native_root_gate(self):
  src=(ROOT/'x9/shim/ufs/ufs_probe.c').read_text();start=src.index(' if(X9_ROOT_MUTABLE){\n  uint32_t attrs=be32(native_header+1028);');end=src.index('\n plog_str("UFS NATIVE ROOT HEADER VERIFIED:',start);gate=src[start:end]
  code="""#include <stdint.h>\n#include <assert.h>\n#define X9_ROOT_MUTABLE 1\n#define X9U_GEOMETRY 23\nstatic uint8_t native_header[65536];\nstatic uint32_t be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}\nstatic void plog_hex(const char*s,uint32_t x){(void)s;(void)x;}\nstatic void plog_flush(void){}\nint test(void){\n"""+gate+"""\nreturn 0;}\nint main(void){uint32_t x[]={0x80000100,0x80004000,0x80000000,0x80002000,0x80006100};int want[]={0,23,23,0,23};for(int i=0;i<5;i++){native_header[1028]=x[i]>>24;native_header[1029]=x[i]>>16;native_header[1030]=x[i]>>8;native_header[1031]=x[i];assert(test()==want[i]);}return 0;}\n"""
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'t.c').write_text(code);subprocess.run(['clang','-fsanitize=address,undefined','-Wall','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True);subprocess.run([str(p/'t')],check=True)
if __name__=='__main__':unittest.main()
