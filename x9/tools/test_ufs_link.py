"""Fake HCI/UIC MMIO tests, not evidence of MT6993 link or PHY operation."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
class LinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='x9-ufs-wake-');p=Path(cls.tmp.name)/'link.dylib'
        subprocess.run(['clang','-O2','-shared','-fPIC','-Wall','-Wextra','-Werror','-I',str(ROOT/'x9/shim/ufs'),str(ROOT/'x9/shim/ufs/ufs_link.c'),str(ROOT/'x9/shim/ufs/ufs_platform.c'),str(ROOT/'x9/shim/fdt_property.c'),str(ROOT/'x9/tools/ufs_link_harness.c'),'-o',str(p)],check=True)
        cls.lib=C.CDLL(str(p));cls.lib.link_time.restype=C.c_uint64
        cls.lib.link_event.restype=C.c_uint32
        cls.lib.link_get.restype=C.c_uint32
        cls.lib.x9u_x9_reset_route.argtypes=[C.c_void_p]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):self.lib.link_reset(0)
    def events(self):return [(self.lib.link_event(i,0),self.lib.link_event(i,1)) for i in range(self.lib.link_events())]
    def test_disabled_hci_is_enabled_then_link_started_and_original_hce_restored(self):
        self.assertEqual(self.lib.link_start(1),0);self.assertEqual(self.lib.link_stage(),5)
        self.assertEqual(self.events(),[(0x24,0),(0x34,1),(0x94,0),(0x98,0),(0x9c,0),(0x90,0x16),(0x20,0x400)])
        self.assertEqual(self.lib.link_get(0x30)&3,3)
        self.assertEqual(self.lib.link_stop(),0);self.assertEqual(self.lib.link_get(0x34),0);self.assertEqual(self.lib.link_get(0x24),0x1234)
    def test_retained_ready_link_requires_no_write_or_restart(self):
        self.lib.link_set(0x34,1);self.lib.link_set(0x30,0xb)
        self.assertEqual(self.lib.link_start(1),0);self.assertEqual(self.lib.link_stop(),0);self.assertEqual(self.events(),[])
    def test_hce_reenable_with_existing_device_skips_link_startup(self):
        self.lib.link_reset(7);self.assertEqual(self.lib.link_start(1),0)
        self.assertEqual(self.events(),[(0x24,0),(0x34,1)])
        self.assertEqual(self.lib.link_stop(),0)
    def test_each_failure_is_bounded_and_identifies_its_stage(self):
        for mode,want,stage in [(1,-6,1),(2,-6,2),(3,-6,3),(4,-7,3),(5,-6,4)]:
            self.lib.link_reset(mode);self.assertEqual(self.lib.link_start(1),want)
            self.assertEqual(self.lib.link_stage(),stage);self.assertLessEqual(self.lib.link_time(),601000)
            self.assertEqual(self.lib.link_stop(),0);self.assertEqual(self.lib.link_get(0x34),0)
    def test_no_hardware_mutation_without_verified_dma_or_when_queue_busy(self):
        self.assertEqual(self.lib.link_start(0),-4);self.assertEqual(self.events(),[])
        for reg,value in [(0x58,1),(0x78,1),(0x300,1),(0x20,0x400)]:
            self.lib.link_reset(0);self.lib.link_set(reg,value)
            self.assertEqual(self.lib.link_start(1),-3);self.assertEqual(self.events(),[])
    def test_never_disable_controller_with_pending_dma_or_running_queue(self):
        for reg,want in [(0x58,-11),(0x78,-11),(0x60,-3)]:
            self.lib.link_reset(0);self.assertEqual(self.lib.link_start(1),0)
            count=self.lib.link_events();self.lib.link_set(reg,1)
            self.assertEqual(self.lib.link_stop(),want);self.assertEqual(self.lib.link_events(),count)
            self.assertEqual(self.lib.link_get(0x34),1)
    def test_unrelated_interrupts_are_not_acknowledged(self):
        self.lib.link_set(0x20,0x10);self.assertEqual(self.lib.link_start(1),0)
        self.assertEqual(self.lib.link_get(0x20),0x10)
        self.assertEqual([v for r,v in self.events() if r==0x20],[0x400])

    def test_mtk_prelink_releases_only_documented_local_attributes(self):
        self.lib.link_reset(10);self.assertEqual(self.lib.link_start(1),-7)
        self.lib.link_reset(10);self.assertEqual(self.lib.link_start_mtk(1),0)
        self.assertEqual(self.lib.link_completed(),4);self.assertEqual(self.lib.link_stage(),5)
        commands=[v for r,v in self.events() if r==0x90]
        attributes=[v for r,v in self.events() if r==0x94]
        self.assertEqual(commands,[2,2,1,2,0x16])
        self.assertEqual(attributes,[0xd0a80000,0x15850000,0xd0a60000,0xd0a60000,0])
        self.assertEqual(C.c_uint32(self.lib.link_savepower()).value,0xabcdefef&~0x40)
        self.assertEqual(self.lib.link_stop(),0)
    def test_mtk_prelink_failure_identifies_attribute_and_does_not_start_link(self):
        for mode,stage,attr,completed in [(8,6,0xd0a8,0),(9,8,0xd0a6,2)]:
            self.lib.link_reset(mode);self.assertEqual(self.lib.link_start_mtk(1),-7)
            self.assertEqual(self.lib.link_stage(),stage);self.assertEqual(self.lib.link_attribute(),attr)
            self.assertEqual(self.lib.link_completed(),completed)
            self.assertNotIn((0x90,0x16),self.events());self.assertEqual(self.lib.link_stop(),0)
    def test_mtk_prelink_is_not_attempted_without_authoritative_dma_gate(self):
        self.assertEqual(self.lib.link_start_mtk(0),-4);self.assertEqual(self.events(),[])

    def test_full_x9_init_resets_device_configures_host_and_postlink(self):
        self.lib.link_reset(13);self.assertEqual(self.lib.link_start_full(1),0)
        self.assertEqual(self.lib.link_stage(),5);self.assertEqual(self.lib.link_resets(),2)
        self.assertEqual(self.lib.link_get(0x140),0x80)
        self.assertEqual(self.lib.link_get(0x160),0x02453000)
        self.assertEqual(self.lib.link_get(0x2308),3)
        commands=[v for r,v in self.events() if r==0x90]
        self.assertEqual(commands.count(0x16),1)
        resets=[v for r,v in self.events() if r==0xffff0000]
        self.assertEqual(resets,[0,1]);self.assertEqual(self.lib.link_stop(),0)
        for reg in [0x140,0x160,0x2308]:self.assertEqual(self.lib.link_get(reg),0)
    def test_full_x9_reset_config_failure_and_coherence_ack_stop_before_media(self):
        for mode,stage in [(11,11),(12,12),(14,13),(16,15)]:
            self.lib.link_reset(mode);self.assertNotEqual(self.lib.link_start_full(1),0)
            self.assertEqual(self.lib.link_stage(),stage)
            self.assertNotIn((0x90,0x16),self.events());self.assertEqual(self.lib.link_stop(),0)
    def test_only_one_bounded_linereset_recovery_attempt(self):
        self.lib.link_reset(15);self.assertEqual(self.lib.link_start_full(1),0)
        self.assertEqual(self.lib.link_attempts(),2)
        self.assertEqual([v for r,v in self.events() if r==0x90].count(0x16),2)
    def test_persistent_linereset_stops_after_two_attempts_and_restores_host_config(self):
        self.lib.link_reset(17);self.assertEqual(self.lib.link_start_full(1),-7)
        self.assertEqual(self.lib.link_attempts(),2)
        self.assertEqual([v for r,v in self.events() if r==0x90].count(0x16),2)
        self.assertEqual(self.lib.link_stop(),0)
        for reg in [0x140,0x160,0x2308,0x18]:self.assertEqual(self.lib.link_get(reg),0)
    def test_full_x9_wrong_ip_cannot_reset_or_program_vendor_registers(self):
        self.lib.link_set(0x2240,0x01450000)
        self.assertEqual(self.lib.link_start_full(1),-5)
        self.assertEqual(self.lib.link_resets(),0);self.assertEqual(self.events(),[])

    def test_current_x9_fdt_reset_bank_validates_and_modified_flag_is_rejected(self):
        path=Path('/Users/tosasitill/Downloads/01-Android/内测/x9内测/final/x9/logs/fdt.dtb')
        if not path.exists():self.skipTest('requires captured X9 FDT')
        data=path.read_bytes();self.assertEqual(self.lib.x9u_x9_reset_route(C.create_string_buffer(data)),1)
        needle=bytes.fromhex('00000148000000000000014c0000000000000000000000000000001c')
        at=data.index(needle);bad=bytearray(data);bad[at+27]^=8
        self.assertEqual(self.lib.x9u_x9_reset_route(C.create_string_buffer(bytes(bad))),0)

    def test_pan76_active_device_rejects_second_startup_so_only_one_is_sent(self):
        self.lib.link_reset(18)
        self.assertEqual(self.lib.link_start_full(1),0)
        self.assertEqual(self.lib.link_get(0x30),0xf)
        self.assertEqual([v for r,v in self.events() if r==0x90].count(0x16),1)
        self.assertEqual(self.lib.link_stage(),5)
        self.assertEqual(self.lib.link_stop(),0)

    def test_native_handoff_keeps_active_link_and_coherence_but_detaches_lists(self):
        self.lib.link_reset(13);self.assertEqual(self.lib.link_start_full(1),0)
        self.lib.link_set(0x50,0x8003c000);self.lib.link_set(0x54,1)
        self.assertEqual(self.lib.link_handoff(),0)
        self.assertEqual(self.lib.link_get(0x34),1);self.assertEqual(self.lib.link_get(0x2308),3)
        self.assertEqual(self.lib.link_get(0x50),0);self.assertEqual(self.lib.link_get(0x54),0)
        self.assertEqual(self.lib.link_get(0x24),0)
    def test_native_handoff_refuses_pending_dma_or_running_transfer_list(self):
        for reg,want in [(0x58,-11),(0x78,-11),(0x60,-3)]:
            self.lib.link_reset(13);self.assertEqual(self.lib.link_start_full(1),0)
            self.lib.link_set(reg,1);count=self.lib.link_events()
            self.assertEqual(self.lib.link_handoff(),want);self.assertEqual(self.lib.link_events(),count)

if __name__=='__main__':unittest.main()
