import struct,unittest
from usb_log import parse,MAGIC
class Tests(unittest.TestCase):
 def test_valid_and_gap(self):
  frame=struct.pack('<6I',MAGIC,2,50,53,60,1)+b'abc';self.assertEqual(parse(frame),(2,50,53,60,1,b'abc'))
 def test_invalid_frames(self):
  for frame in [b'',struct.pack('<6I',MAGIC,0,0,0,0,0),struct.pack('<6I',MAGIC,1,0,4,3,0)+b'abcd',struct.pack('<6I',MAGIC,1,0,2,3,0)+b'a',struct.pack('<6I',MAGIC,1,0,0,0,4)]:
   with self.subTest(frame=frame),self.assertRaises(ValueError):parse(frame)
if __name__=='__main__':unittest.main()
