"""Assert the PMIF probe can only issue bounded reads to the known repeater."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class RepeaterReadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='x9-pmif-test-')
        output=Path(cls.tmp.name)/'probe.dylib'
        source=Path(__file__).resolve().parents[1]/'shim/usb_repeater_probe.c'
        subprocess.run([shutil.which('clang'),'-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',str(source),'-o',str(output)],check=True)
        cls.module=ctypes.CDLL(str(output))
        cls.READ=ctypes.CFUNCTYPE(ctypes.c_uint32,ctypes.c_void_p,ctypes.c_size_t)
        cls.WRITE=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32)
        cls.DELAY=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_uint)
        cls.module.x9_usb_repeater_read.argtypes=[ctypes.c_void_p,ctypes.c_uint16,ctypes.POINTER(ctypes.c_uint8),ctypes.POINTER(ctypes.c_uint32),cls.READ,cls.WRITE,cls.DELAY]

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def run_probe(self,address=0xd88,initial=0,complete=12,init=1):
        writes=[];delays=[];requested=False
        def read(_,a):
            if a==0x1c018000:return init
            if a==0x1c0188a8:return complete if requested else initial
            if a==0x1c018894:return 0x48
            raise AssertionError(hex(a))
        def write(_,a,v):
            nonlocal requested
            writes.append((a,v))
            if a==0x1c018880:requested=True
        def delay(_,us):delays.append(us)
        value=ctypes.c_uint8(0xa5);status=ctypes.c_uint32()
        result=self.module.x9_usb_repeater_read(None,address,ctypes.byref(value),ctypes.byref(status),self.READ(read),self.WRITE(write),self.DELAY(delay))
        return result,value.value,status.value,writes,delays

    def test_read_command_and_valid_ack_without_pmic_write(self):
        result,value,status,writes,delays=self.run_probe()
        self.assertEqual((result,value,status),(0,0x48,12))
        self.assertEqual(writes,[(0x1c018880,0xce000d88),(0x1c0188a4,1)])
        self.assertFalse(writes[0][1]&(1<<29))
        self.assertEqual(delays,[])

    def test_busy_or_uninitialized_interface_is_never_force_cleared(self):
        for kwargs in [{'initial':12},{'init':0}]:
            result,_,_,writes,_=self.run_probe(**kwargs)
            self.assertLess(result,0);self.assertEqual(writes,[])

    def test_timeout_is_bounded_and_error_never_returns_valid_data(self):
        result,value,_,writes,delays=self.run_probe(complete=0)
        self.assertEqual((result,value),(-5,0xa5))
        self.assertEqual(writes,[(0x1c018880,0xce000d88)])
        self.assertEqual(delays,[10]*1000)
        result,value,_,writes,_=self.run_probe(complete=12|(1<<18))
        self.assertEqual((result,value),(-4,0xa5))
        self.assertEqual(writes[-1],(0x1c0188a4,1))

    def test_unrelated_pmic_addresses_are_rejected_without_bus_access(self):
        for address in [1,0xd01,0xd87,0xffff]:
            result,_,_,writes,delays=self.run_probe(address=address)
            self.assertEqual(result,-1);self.assertEqual(writes,[]);self.assertEqual(delays,[])


if __name__=='__main__':unittest.main()
