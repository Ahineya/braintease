#!/usr/bin/env python3
"""Real OpenSSH private-container imports and guest PKCS#8 conversion."""
import argparse
import base64
import fcntl
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=('native','ripple'),default='native')
    args=parser.parse_args()
    suffix='.fxe' if args.target=='native' else '.rxe'
    with (ROOT/'build/ssh/client'/(args.target+'.lock')).open('a') as lock, tempfile.TemporaryDirectory(prefix='ssh-import-',dir='/tmp') as directory:
        fcntl.flock(lock,fcntl.LOCK_SH)
        work=Path(directory);files=[];public={}
        for name,kind,bits in (('rsa','rsa',2048),('rsa4096','rsa',4096),('ed','ed25519',None),('ec','ecdsa',521),('encrypted','ed25519',None)):
            command=['ssh-keygen','-q','-t',kind,'-N','password' if name=='encrypted' else '', '-f',str(work/name)]
            if bits:command+=['-b',str(bits)]
            subprocess.run(command,check=True)
            files.append(dict(name='/root/'+name,mode=0o100600,path=str(work/name)))
            public[name]=(work/(name+'.pub')).read_text().split()[:2]
        # Corrupt only the outer public half, leaving a valid private body.
        raw=bytearray(base64.b64decode(''.join((work/'ed').read_text().splitlines()[1:-1])))
        at=15
        for _ in range(3):n=struct.unpack_from('>I',raw,at)[0];at+=4+n
        at+=4;n=struct.unpack_from('>I',raw,at)[0];raw[at+4+n-1]^=1
        encoded=base64.b64encode(raw).decode()
        files.append(dict(name='/root/mismatch',mode=0o100600,text='-----BEGIN OPENSSH PRIVATE KEY-----\n'+encoded+'\n-----END OPENSSH PRIVATE KEY-----\n'))
        files.append(dict(name='/root/'+'k'*30,mode=0o100600,path=str(work/'ed')))
        guest=Guest(work,args.target,{'ssh-keygen':ROOT/'build/ssh'/('ssh-keygen'+suffix)},files,blocks=65536,code_memory=None if args.target=='native' else 8388608)
        try:
            for name in ('rsa','rsa4096','ed','ec'):
                text=guest.shell(f'ssh-keygen -y -f /root/{name}; echo IMPORT_STATUS:$?',300)
                assert 'IMPORT_STATUS:0\n' in text,text
                found=next(line.split()[:2] for line in text.splitlines() if line.startswith(('ssh-rsa ','ssh-ed25519 ','ecdsa-sha2-')))
                assert found==public[name],(name,found,public[name])
                text=guest.shell(f'ssh-keygen -p -f /root/{name} -P "" -N converted; echo CONVERT_STATUS:$?',300)
                assert 'CONVERT_STATUS:0\n' in text,text
                text=guest.shell(f'ssh-keygen -y -f /root/{name} -P converted; echo REOPEN_STATUS:$?',300)
                assert 'REOPEN_STATUS:0\n' in text,text
                found=next(line.split()[:2] for line in text.splitlines() if line.startswith(('ssh-rsa ','ssh-ed25519 ','ecdsa-sha2-')))
                assert found==public[name],(name,found,public[name])
                print(f'PASS OpenSSH {name} import, public-half comparison and encrypted PKCS8 conversion',flush=True)
            name='k'*30
            text=guest.shell(f'ssh-keygen -p -f /root/{name} -P "" -N boundary; echo BOUNDARY:$?',300)
            assert 'BOUNDARY:0\n' in text,text
            text=guest.shell(f'ssh-keygen -y -f /root/{name}x -P boundary; echo LONG_NAME:$?',300)
            assert 'LONG_NAME:1\n' in text and 'name too long' in text.lower(),text
            print('PASS full-length private-name passphrase change and overlong alias rejection',flush=True)
            for name in ('mismatch','encrypted'):
                text=guest.shell(f'ssh-keygen -y -f /root/{name} -P password; echo DENIED:$?',300)
                assert 'DENIED:1\n' in text,text
            print('PASS mismatched key halves and unsupported bcrypt-encrypted OpenSSH containers rejected',flush=True)
        finally:guest.close()


if __name__=='__main__':main()
