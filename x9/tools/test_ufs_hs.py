"""Test actual MT6993 HS code with independent MMIO/async completion model."""
import ctypes as C,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
class HSTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();cls.p=Path(cls.tmp.name);cls.sources=[str(ROOT/'x9/shim/ufs/ufs_hs.c'),str(ROOT/'x9/tools/ufs_hs_harness.c')];cls.flags=['-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'x9/shim/ufs')]
  subprocess.run(['clang',*cls.flags,'-O2','-shared','-fPIC',*cls.sources,'-o',str(cls.p/'hs.dylib')],check=True);cls.lib=C.CDLL(str(cls.p/'hs.dylib'));cls.lib.hs_time.restype=C.c_uint64
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def events(self):return [(self.lib.hs_event(i,0),self.lib.hs_event(i,1),self.lib.hs_event(i,2)) for i in range(self.lib.hs_events())]
 def test_verified_g1_fastauto_and_actual_readback(self):
  self.assertEqual(self.lib.hs_run(0),0);self.assertEqual(self.lib.hs_stage(),10);self.assertEqual([self.lib.hs_value(i) for i in range(6)],[0x44,1,1,2,2,2]);self.assertEqual(self.lib.hs_changed(),1);self.assertEqual(self.lib.hs_notifies(),2)
  attrs=[a for reg,cmd,a in self.events() if reg==0x90 and cmd==2];self.assertEqual(attrs[-1],0x1571);self.assertIn(0x15d4,attrs);self.assertEqual(self.lib.hs_is(),1<<16)
 def test_upms_and_pwr_local_are_both_mandatory(self):
  for mode in [1,2]:self.assertNotEqual(self.lib.hs_run(mode),0);self.assertEqual(self.lib.hs_stage(),7);self.assertLessEqual(self.lib.hs_time(),500100)
 def test_power_completion_can_precede_uic_completion(self):self.assertEqual(self.lib.hs_run(14),0);self.assertEqual(self.lib.hs_is(),1<<16)
 def test_readback_mismatch_is_not_success(self):
  for mode in [3,17]:self.assertNotEqual(self.lib.hs_run(mode),0);self.assertEqual(self.lib.hs_stage(),8)
 def test_configuration_error_stops_before_power_change(self):
  self.assertNotEqual(self.lib.hs_run(4),0);self.assertEqual(self.lib.hs_changed(),0)
 def test_stale_events_busy_dma_unsupported_peer_and_wrong_soc(self):
  for mode in [5,6,7,15,16]:self.assertNotEqual(self.lib.hs_run(mode),0);self.assertEqual(self.lib.hs_sets(),0);self.assertEqual(self.lib.hs_changed(),0)
 def test_only_connected_lanes_are_selected(self):
  self.assertEqual(self.lib.hs_run(8),0);self.assertEqual([self.lib.hs_value(i) for i in [3,4]],[1,2])
 def test_already_fast_link_keeps_gear_and_does_not_reconfigure(self):
  self.assertEqual(self.lib.hs_run(9),0);self.assertEqual(self.lib.hs_sets(),0);self.assertEqual(self.lib.hs_changed(),0);self.assertEqual(self.lib.hs_value(1),4)
 def test_refclk_and_uic_timeouts_fail_closed_and_are_bounded(self):
  for mode in [10,11,12,13]:self.assertNotEqual(self.lib.hs_run(mode),0);self.assertLessEqual(self.lib.hs_time(),500100)
 def test_no_transfer_doorbells_or_unrelated_interrupt_ack(self):
  for mode in range(18):
   self.lib.hs_run(mode)
   self.assertFalse(any(reg in [0x58,0x78] for reg,_,_ in self.events()))
   self.assertTrue(all(value in [1<<10,1<<4] for reg,value,_ in self.events() if reg==0x20))
 def test_all_model_cases_under_address_and_undefined_sanitizers(self):
  source=self.p/'main.c';source.write_text('''#include <assert.h>
int hs_run(unsigned);unsigned hs_stage(void);int main(void){
const int expect[]={0,-6,-7,-7,-7,-3,-3,-5,0,0,-6,-7,-7,-6,0,-5,-7,-7};
for(unsigned i=0;i<18;i++){assert(hs_run(i)==expect[i]);if(!expect[i])assert(hs_stage()==10);}return 0;}''')
  subprocess.run(['/opt/homebrew/opt/llvm/bin/clang',*self.flags,'-O1','-fsanitize=address,undefined',*self.sources,str(source),'-o',str(self.p/'sanitize')],check=True);subprocess.run([str(self.p/'sanitize')],check=True)
if __name__=='__main__':unittest.main()
