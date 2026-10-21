#!/usr/bin/env python3
"""Guest key generation and encrypted-key interoperability with OpenSSH/OpenSSL."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=('native','ripple'),default='native');p.add_argument('--rsa-generation',action='store_true',help='include expensive RSA generation on RVM too');args=p.parse_args()
    probe=ROOT/'build/ssh'/('secretcheck.fxe' if args.target=='native' else 'secretcheck.rxe')
    subprocess.run([sys.executable,str(ROOT/'ports/ssh/app.py'),'--app','ssh-keygen','--target',args.target,'--source',str(Path(__file__).with_name('secretcheck.c')),'-o',str(probe)],check=True)
    crypto=ROOT/'build/ssh'/('cryptocheck.fxe' if args.target=='native' else 'cryptocheck.rxe')
    subprocess.run([sys.executable,str(ROOT/'ports/ssh/app.py'),'--app','ssh-keygen','--target',args.target,'--source',str(Path(__file__).with_name('cryptocheck.c')),'-o',str(crypto)],check=True)
    with (ROOT/'build/ssh/client'/(args.target+'.lock')).open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_SH)
        run_tests(args)

def run_tests(args):
    exe=ROOT/'build/ssh'/('ssh-keygen.fxe' if args.target=='native' else 'ssh-keygen.rxe')
    with tempfile.TemporaryDirectory(prefix='ssh-keys-',dir='/tmp') as directory:
        work=Path(directory);guest=Guest(work,args.target,{'ssh-keygen':exe,'cryptocheck':ROOT/'build/ssh'/('cryptocheck.fxe' if args.target=='native' else 'cryptocheck.rxe'),'secretcheck':ROOT/'build/ssh'/('secretcheck.fxe' if args.target=='native' else 'secretcheck.rxe')},blocks=65536)
        public={};fingers={}
        try:
            text=guest.shell('cryptocheck; echo CRYPTO_STATUS:$?');assert 'CRYPTO_OK\n' in text and 'CRYPTO_STATUS:0\n' in text,text
            print('PASS octet mask selection, RFC 8032 Ed25519 public/signature vectors, tamper rejection and DER round-trip',flush=True)
            for bits in (256,384,521):
                text=guest.shell(f'ssh-keygen -b {bits} -f /root/key{bits} -N ""; echo GENERATED:$?')
                assert 'GENERATED:0\n' in text,text
                text=guest.shell(f'cat /root/key{bits}.pub');public[bits]=next(l for l in text.splitlines() if l.startswith('ecdsa-sha2-'))
                text=guest.shell(f'ssh-keygen -l -f /root/key{bits}.pub');fingers[bits]=next(l for l in text.splitlines() if l.startswith('SHA256:'))
                text=guest.shell(f'ssh-keygen -y -f /root/key{bits}; echo EXPORTED:$?');assert public[bits].split()[:2]==next(l for l in text.splitlines() if l.startswith('ecdsa-sha2-')).split()[:2],text
                assert 'EXPORTED:0\n' in text,text
            generated=(('ed25519',256),('rsa',2048)) if args.target=='native' or args.rsa_generation else (('ed25519',256),)
            for kind,bits in generated:
                text=guest.shell(f'ssh-keygen -t {kind} -b {bits} -f /root/{kind} -N "test secret"; echo TYPE_STATUS:$?',600)
                assert 'TYPE_STATUS:0\n' in text,text
                text=guest.shell(f'cat /root/{kind}.pub');public[kind]=next(l for l in text.splitlines() if l.startswith(('ssh-ed25519 ', 'ssh-rsa ')))
                text=guest.shell(f'ssh-keygen -y -f /root/{kind} -P "test secret"; echo IMPORTED:$?',300)
                assert 'IMPORTED:0\n' in text and public[kind].split()[:2]==next(l for l in text.splitlines() if l.startswith(('ssh-ed25519 ', 'ssh-rsa '))).split()[:2],text
            os.write(guest.master,b'secretcheck; echo LONG_STATUS:$?\r')
            guest.read(b'Secret: ')
            os.write(guest.master,b'x'*30+b'\r')
            text=guest.read(guest.prompt);assert 'LONG_STATUS:1\n' in text and 'x'*10 not in text and 'SECRET_OVERFLOW' in text,text
            text=guest.shell('echo KEY_PROMPT_RECOVERED');assert '\nKEY_PROMPT_RECOVERED\n' in text,text
            os.write(guest.master,b'secretcheck; echo CANCEL_STATUS:$?\r')
            guest.read(b'Secret: ')
            os.write(guest.master,b'\x03')
            text=guest.read(guest.prompt);assert 'CANCEL_STATUS:1\n' in text and 'SECRET_INTERRUPTED' in text,text
            os.write(guest.master,b'secretcheck signal; echo QUIT_STATUS:$?\r')
            guest.read(b'Secret: ')
            text=guest.read(guest.prompt);assert 'QUIT_STATUS:1\n' in text and 'SECRET_INTERRUPTED' in text,text
            text=guest.shell('ssh-keygen -f /root/protected -N "test secret"; echo ENCRYPTED:$?');assert 'ENCRYPTED:0\n' in text,text
            text=guest.shell('ssh-keygen -y -f /root/protected -P wrong; echo BADPASS:$?');assert 'BADPASS:1\n' in text,text
            text=guest.shell('ssh-keygen -y -f /root/protected -P "test secret"; echo GOODPASS:$?');assert 'GOODPASS:0\n' in text,text
            text=guest.shell('ssh-keygen -p -f /root/key256 -P "" -N changed; echo CHANGED:$?');assert 'CHANGED:0\n' in text,text
            text=guest.shell('ssh-keygen -f /root/key384 -N ""; echo EXISTS:$?');assert 'EXISTS:1\n' in text,text
            text=guest.shell('ssh-keygen -y -f /root/key256 -P changed; echo REOPENED:$?');assert 'REOPENED:0\n' in text,text
        finally:guest.close()
        def read(path):
            if args.target=='ripple':
                sys.path.insert(0,str(ROOT/'ports/fuzix/tools'));from root_reader import RootReader
                return bytes(RootReader(guest.image).read(path))
            script="import {openImage} from './ports/fuzix/tools/image.mjs';import {rootReader} from './ports/fuzix/tools/read-root.mjs';process.stdout.write(Buffer.from(rootReader(openImage(process.argv[1])).read(process.argv[2])));"
            return subprocess.check_output(['node','--input-type=module','-e',script,str(guest.image),path],cwd=ROOT)
        for bits in (256,384,521):
            key=work/f'key{bits}';key.write_bytes(read(f'/root/key{bits}'));key.chmod(0o600)
            (work/f'key{bits}.pub').write_text(public[bits]+'\n')
            result=subprocess.check_output(['ssh-keygen','-lf',str(work/f'key{bits}.pub')],text=True)
            assert fingers[bits] in result,(fingers[bits],result)
            subprocess.run(['openssl','pkey','-in',str(key),'-passin','pass:changed' if bits==256 else 'pass:','-check','-noout'],check=True,stdout=subprocess.DEVNULL)
            exported=subprocess.check_output(['ssh-keygen','-y','-P','changed' if bits==256 else '', '-f',str(key)],text=True)
            assert exported.split()[:2]==public[bits].split()[:2],exported
        for kind in (name for name in ('rsa','ed25519') if name in public):
            key=work/kind;key.write_bytes(read('/root/'+kind));key.chmod(0o600)
            subprocess.run(['openssl','pkey','-in',str(key),'-passin','pass:test secret','-check','-noout'],check=True,stdout=subprocess.DEVNULL)
            # OpenSSH does not import PKCS#8 Ed25519; convert with OpenSSL and
            # compare the raw public value against the RFC 4253 public blob.
            der=subprocess.check_output(['openssl','pkey','-in',str(key),'-passin','pass:test secret','-pubout','-outform','DER'])
            import base64
            blob=base64.b64decode(public[kind].split()[1])
            if kind=='ed25519':assert blob[-32:]==der[-32:],(blob.hex(),der.hex())
            else:
                exported=subprocess.check_output(['ssh-keygen','-y','-P','test secret','-f',str(key)],text=True)
                assert exported.split()[:2]==public[kind].split()[:2],exported
        print('PASS guest '+('RSA-2048 and ' if 'rsa' in public else '')+'Ed25519 generation, encrypted PKCS8, import and independent public-key checks',flush=True)
        key=work/'protected';key.write_bytes(read('/root/protected'));key.chmod(0o600)
        subprocess.run(['openssl','pkey','-in',str(key),'-passin','pass:test secret','-check','-noout'],check=True,stdout=subprocess.DEVNULL)
        exported=subprocess.check_output(['ssh-keygen','-y','-P','test secret','-f',str(key)],text=True)
        assert exported.startswith('ecdsa-sha2-nistp256 '),exported
        print('PASS hidden secret input, overflow draining, Ctrl-C/SIGQUIT interruption and terminal restoration')
        print('PASS guest ECDSA 256/384/521 keys, OpenSSH fingerprints, encrypted PKCS8, passphrase changes and overwrite refusal')
if __name__=='__main__':main()
