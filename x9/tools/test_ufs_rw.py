"""Extent-only write engine tests with separate CPU/device DMA memories.
No hardware storage is accessed. Inherits transport error/read-only regressions.
"""
import ctypes as C
import random
import unittest
import test_ufs_ro as base
Partition,Extent=base.Partition,base.Extent

class WritableUFSTests(base.UFSTests):
 extra_cflags=['-DX9U_EXTENT_WRITES=1']
 @classmethod
 def setUpClass(cls):
  super().setUpClass()
  cls.lib.test_bind_write.argtypes=[C.c_uint,C.POINTER(Partition),C.POINTER(Extent),C.c_size_t,C.c_uint64]
  cls.lib.test_write_image.argtypes=[C.c_uint64,C.c_void_p,C.c_size_t]
  cls.lib.test_disk_copy.argtypes=[C.c_uint,C.c_void_p,C.c_size_t]
 def bound(self):
  self.initial=bytes((i*37+i//4096)%256 for i in range(0x200000))
  self.put_disk(self.initial);self.opened();self.assertEqual(self.lib.test_capacity(0),0)
  self.part=Partition(10,200);self.map=(Extent*3)(Extent(0,30,2),Extent(2,80,3),Extent(5,120,20))
  self.assertEqual(self.lib.test_bind_write(0,C.byref(self.part),self.map,3,25),0)
 def snapshot(self):
  out=C.create_string_buffer(0x200000);self.lib.test_disk_copy(0,out,len(out));return out.raw
 def test_writes_refused_before_binding_and_invalid_maps_never_grant(self):
  self.put_disk(bytes(0x200000));self.opened();self.assertEqual(self.lib.test_capacity(0),0)
  before=self.lib.test_commands();data=C.create_string_buffer(b'X'*512)
  self.assertEqual(self.lib.test_write_image(0,data,512),-12);self.assertEqual(self.lib.test_sync_image(),-12)
  overlap=(Extent*2)(Extent(0,30,2),Extent(2,31,2));p=Partition(10,200)
  self.assertEqual(self.lib.test_bind_write(0,C.byref(p),overlap,2,4),-12)
  self.assertEqual(self.lib.test_write_image(0,data,512),-12);self.assertEqual(self.lib.test_commands(),before)
 def test_random_unaligned_cross_extent_writes_preserve_all_other_disk_bytes(self):
  self.bound();expected=bytearray(self.initial);rnd=random.Random(201)
  for i in range(35):
   off=rnd.randrange(25*4096);n=min(rnd.randrange(1,70000),25*4096-off);data=bytes(rnd.randrange(256) for _ in range(n))
   self.assertEqual(self.lib.test_write_image(off,C.create_string_buffer(data),n),0)
   for j in range(n):
    logical=(off+j)//4096;entry=next(e for e in self.map if e.file<=logical<e.file+e.count)
    physical=(self.part.first+entry.part+logical-entry.file)*4096+(off+j)%4096
    expected[physical]=data[j]
   self.assertEqual(self.snapshot(),expected)
  self.assertEqual(self.lib.test_sync_image(),0);self.assertEqual(self.lib.test_cdb(),0x35);self.assertEqual(self.lib.test_forbidden(),0)
 def test_out_of_range_requests_and_modified_selected_extent_never_write(self):
  self.bound();data=C.create_string_buffer(b'X'*4096);commands=self.lib.test_commands()
  for off,n in [(25*4096,1),(25*4096-1,2),(2**64-1,1)]:self.assertEqual(self.lib.test_write_image(off,data,n),-12)
  self.assertEqual(self.lib.test_commands(),commands)
  self.map[0].part=200
  self.assertEqual(self.lib.test_write_image(0,data,4096),-12)
  self.assertEqual(self.lib.test_commands(),commands);self.assertEqual(self.snapshot(),self.initial)
 def test_write_failure_is_reported_and_partial_read_cdb_is_not_mistaken_for_write(self):
  self.bound();data=C.create_string_buffer(b'X'*4096)
  self.lib.test_mode(2)
  self.assertEqual(self.lib.test_write_image(0,data,4096),-7)
  self.assertEqual(self.lib.test_cdb(),0x2a)
  self.lib.test_mode(0);self.assertEqual(self.lib.test_sync_image(),0)
  self.assertEqual(self.lib.test_cdb(),0x35)
 def test_failed_partial_read_never_submits_write_and_timeout_poison_stops_followups(self):
  self.bound();data=C.create_string_buffer(b'X'*512);self.lib.test_mode(1);commands=self.lib.test_commands()
  self.assertEqual(self.lib.test_write_image(1,data,512),-7)
  self.assertEqual(self.lib.test_commands(),commands+1);self.assertEqual(self.snapshot(),self.initial)
  self.lib.test_mode(6);self.assertEqual(self.lib.test_write_image(4096,data,512),-11)
  commands=self.lib.test_commands();self.assertEqual(self.lib.test_sync_image(),-12)
  self.assertEqual(self.lib.test_write_image(0,data,512),-12);self.assertEqual(self.lib.test_commands(),commands)

if __name__=='__main__':unittest.main()
