import importlib.util,tempfile,unittest
from pathlib import Path
p=Path(__file__).resolve().parent;spec=importlib.util.spec_from_file_location('restore',p/'restore_journal.py');r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
class Fake:
 def __init__(self,data):self.data=bytearray(data);self.writes=[]
 def read(self,off,n):return bytes(self.data[off:off+n])
 def write(self,off,data,expected):
  assert r.sha(self.read(off,len(data)))==expected
  self.writes.append((off,len(data)));self.data[off:off+len(data)]=data
class Test(unittest.TestCase):
 def test_changed_only_header_last_and_resume(self):
  old=b'a'*r.BLOCK*6;new=b'b'*r.BLOCK*6
  items=[{'offset':i*r.BLOCK,'old_sha256':r.sha(old[i*r.BLOCK:(i+1)*r.BLOCK]),'new_sha256':r.sha(new[i*r.BLOCK:(i+1)*r.BLOCK])} for i in (0,1,2,4)]
  with tempfile.TemporaryDirectory() as td:
   f=Path(td)/'target';f.write_bytes(new);d=Fake(old);written,skip=r.apply(items,f,d)
   self.assertEqual((written,skip),(4*r.BLOCK,0));self.assertEqual(d.writes[-1][0],0)
   d.writes.clear();self.assertEqual(r.apply(items,f,d),(0,4*r.BLOCK));self.assertEqual(d.writes,[])
 def test_reject_unknown_block_before_writing_group(self):
  old=b'a'*r.BLOCK*4;new=b'b'*r.BLOCK*4;items=[{'offset':i*r.BLOCK,'old_sha256':r.sha(b'a'*r.BLOCK),'new_sha256':r.sha(b'b'*r.BLOCK)} for i in (0,2,3)]
  with tempfile.TemporaryDirectory() as td:
   f=Path(td)/'target';f.write_bytes(new);d=Fake(old);d.data[3*r.BLOCK]=0
   with self.assertRaises(RuntimeError):r.apply(items,f,d)
   self.assertFalse(d.writes)
if __name__=='__main__':unittest.main()
