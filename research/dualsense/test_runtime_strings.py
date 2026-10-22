"""Synthetic and source-backed controls for the bounded native-string decoder."""
import unittest,struct
import decode_runtime_strings as decoder

class DecoderControls(unittest.TestCase):
 def test_ciphertext_feedback_and_utf16_roundtrip(self):
  for seed in [0,1,0xffffffffffffffff,0xb0e96460dd073626]:
   plain='catcache4.bin \u03bb \U0001f600\0'.encode('utf-16le');state=seed;cipher=bytearray()
   for (word,) in struct.iter_unpack('<H',plain):
    encrypted=word^(state&65535);cipher.extend(struct.pack('<H',encrypted));state=(((state<<5)|(state>>59))^encrypted)&((1<<64)-1)
   self.assertEqual(decoder.decode_words(cipher,seed),plain)
 def test_odd_ciphertext_is_rejected(self):
  with self.assertRaises(ValueError):decoder.decode_words(b'X',0)
 def test_source_backed_known_string(self):
  result=decoder.recognize(0x200219690)
  self.assertIsNotNone(result);self.assertEqual(result['text'],'catcache4.bin\0');self.assertEqual(result['source_va'],'0x2000f04c8')
 def mutated_copy_is_rejected(self,va,byte):
  original=decoder.raw
  try:
   fixture=bytearray(original);offset=decoder.p.get_offset_from_rva(va-decoder.base);fixture[offset]=byte;decoder.raw=bytes(fixture)
   self.assertIsNone(decoder.recognize(0x200219690))
  finally:decoder.raw=original
 def test_wrong_rotation_is_not_accepted(self):self.mutated_copy_is_rejected(0x2002196a7,4)
 def test_wrong_loop_target_is_not_accepted(self):self.mutated_copy_is_rejected(0x2002196b8,0xd8)

if __name__=='__main__':unittest.main()
