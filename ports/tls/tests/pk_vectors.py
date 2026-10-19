#!/usr/bin/env python3
"""Generate reproducible ECDH public vectors using independent OpenSSL math."""
from pathlib import Path
import subprocess
import tempfile


def tlv(tag,data):
    return bytes([tag,len(data)])+data


def generate():
    result={}
    with tempfile.TemporaryDirectory(prefix='tls-ec-vectors-') as temporary:
        directory=Path(temporary)
        for prefix,size,oid in [('ec',32,'06082a8648ce3d030107'),('ec384',48,'06052b81040022'),('ec521',66,'06052b81040023')]:
            for scalar in (1,2,'wide'):
                private=(bytes((i*37+19)&255 for i in range(size)) if scalar=='wide'
                         else bytes(size-1)+bytes([scalar]))
                if size==66 and scalar=='wide':private=b'\x01'+private[1:]
                der=tlv(0x30,tlv(2,b'\1')+tlv(4,private)+tlv(0xa0,bytes.fromhex(oid)))
                path=directory/f'{prefix}-key{scalar}.der';path.write_bytes(der)
                key=directory/f'{prefix}-key{scalar}.pem';pub=directory/f'{prefix}-pub{scalar}.pem'
                subprocess.run(['openssl','pkey','-inform','DER','-in',str(path),'-out',str(key)],check=True,capture_output=True)
                subprocess.run(['openssl','pkey','-in',str(key),'-pubout','-out',str(pub)],check=True,capture_output=True)
                public=subprocess.check_output(['openssl','pkey','-in',str(key),'-pubout','-outform','DER'])[-(2*size+1):]
                assert public[0]==4
                result[f'{prefix}_private_{scalar}']=private
                result[f'{prefix}_public_{scalar}']=public
            result[prefix+'_shared']=subprocess.check_output(['openssl','pkeyutl','-derive','-inkey',str(directory/f'{prefix}-key1.pem'),'-peerkey',str(directory/f'{prefix}-pub2.pem')])
            result[prefix+'_shared_wide']=subprocess.check_output(['openssl','pkeyutl','-derive','-inkey',str(directory/f'{prefix}-keywide.pem'),'-peerkey',str(directory/f'{prefix}-pub2.pem')])
        assert result['ec_shared'].hex()=='7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978'
    return result


if __name__=='__main__':
    output=Path(__file__).with_name('pk-vectors.h')
    output.write_text('/* OpenSSL P-256/P-384/P-521 small and full-width ECDH known answers; public test material. */\n'+''.join(
        'static const unsigned char '+name+'[]={'+','.join(f'0x{x:02x}' for x in data)+'};\n'
        for name,data in generate().items()))
