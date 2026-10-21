#!/usr/bin/env python3
"""Exercise real package bytes and filesystem transactions on disposable roots."""
from pathlib import Path
import hashlib
import functools
import http.server
import threading
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
ENGINE = ROOT / 'build/pkg/host/pkg-engine-test'

class Packages(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        env = dict(os.environ, CC=os.environ.get('CC', 'cc') + ' -DPKG_TESTING -fsanitize=undefined -fno-sanitize-recover=all')
        subprocess.run(['sh', str(ROOT/'ports/pkg/build.sh'), str(ENGINE)], check=True, env=env)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pkg-')
        self.work = Path(self.temp.name).resolve()
        self.root = self.work/'root'; self.root.mkdir()

    def tearDown(self): self.temp.cleanup()

    def runpkg(self, *args, ok=True, stop=None):
        env = dict(os.environ)
        if stop: env['PKG_TEST_STOP'] = stop
        p = subprocess.run([str(ENGINE), '--root', str(self.root), '--target', 'test', *map(str,args)], capture_output=True, env=env)
        if ok: self.assertEqual(p.returncode, 0, p.stderr.decode())
        else: self.assertNotEqual(p.returncode, 0)
        return p

    def package(self, name='demo', version='1', data=b'one\x00\xff\x80', depends='', target='test', path='usr/bin/demo'):
        stage = self.work/(name+version+'stage'); stage.mkdir()
        payload = stage/path; payload.parent.mkdir(parents=True); payload.write_bytes(data); payload.chmod(0o755)
        meta = self.work/(name+version+'.meta')
        meta.write_text(f'name={name}\nversion={version}\ntarget={target}\ndepends={depends}\n')
        out = self.work/(name+version+'.rpk')
        self.runpkg('create', meta, stage, out)
        return out, stage, meta

    def test_octets_hash_and_determinism(self):
        for n in (0,1,55,56,63,64,65,2048,10003):
            f=self.work/'bytes'; f.write_bytes(bytes(i%256 for i in range(n)))
            self.assertEqual(self.runpkg('hash',f).stdout.decode().split()[0], hashlib.sha256(f.read_bytes()).hexdigest())
        p,stage,meta=self.package(data=bytes(range(256))*20)
        another=self.work/'again.rpk'; self.runpkg('create',meta,stage,another)
        self.assertEqual(p.read_bytes(),another.read_bytes())
        self.runpkg('create',meta,stage,stage/'recursive.rpk',ok=False)
        self.assertIn(hashlib.sha256(p.read_bytes()).hexdigest().encode(),self.runpkg('inspect',p).stdout)
        self.runpkg('install',p)
        self.assertEqual((self.root/'usr/bin/demo').read_bytes(),bytes(range(256))*20)
        self.assertTrue((self.root/'usr/bin/demo').is_symlink())
        self.runpkg('install',p)
        self.runpkg('remove','demo'); self.assertFalse((self.root/'usr/bin/demo').exists())

    def test_upgrade_and_bounded_retention(self):
        for version in ('1','2','3'):
            p,_,_=self.package(version=version,data=version.encode())
            self.runpkg('install',p)
        self.runpkg('clean')
        self.assertEqual(len(list((self.root/'var/lib/pkg/objects').iterdir())),2)
        self.assertEqual((self.root/'usr/bin/demo').read_bytes(),b'3')
        self.runpkg('remove','demo'); self.runpkg('clean')
        self.assertEqual(len(list((self.root/'var/lib/pkg/objects').iterdir())),0)

    def test_ownership_target_and_dependencies(self):
        p,_,_=self.package(); self.runpkg('install',p)
        wrong,_,_=self.package('wrong',target='other',path='usr/bin/wrong'); self.runpkg('install',wrong,ok=False)
        clash,_,_=self.package('clash'); self.runpkg('install',clash,ok=False)
        dep,_,_=self.package('dependent',depends='demo@1',path='usr/bin/dependent'); self.runpkg('install',dep)
        self.runpkg('remove','demo',ok=False)
        upgrade,_,_=self.package(version='2'); self.runpkg('install',upgrade,ok=False)
        self.runpkg('remove','dependent'); self.runpkg('install',upgrade)
        dest=self.root/'usr/bin/demo'; dest.unlink(); dest.write_text('mine')
        self.runpkg('remove','demo',ok=False); self.assertEqual(dest.read_text(),'mine')

    def test_malformed_archives(self):
        p,_,_=self.package(); good=p.read_bytes()
        corruptions=[good[:-1],good+b'X',good.replace(b'usr/bin/demo',b'usr/../demo'),good.replace(b'files=1',b'files=129'),good.replace(b'depends=\n',b'depends=bad\n'),good.replace(b'one\x00',b'two\x00')]
        for data in corruptions:
            broken=self.work/'bad.rpk'; broken.write_bytes(data)
            self.runpkg('inspect',broken,ok=False); self.runpkg('install',broken,ok=False)
        self.assertFalse((self.root/'usr/bin/demo').exists())

    def test_recovery_at_commit_boundaries(self):
        p,_,_=self.package(); self.runpkg('install',p)
        for i,point in enumerate(('prepared','linked','committed'),2):
            p,_,_=self.package(version=str(i),data=str(i).encode())
            self.assertEqual(self.runpkg('install',p,ok=False,stop=point).returncode,99)
            self.runpkg('recover'); self.runpkg('recover')
            self.assertEqual((self.root/'usr/bin/demo').read_bytes(),str(i).encode())
            self.assertIn(f'demo {i} [test]'.encode(),self.runpkg('list').stdout)
        for point in ('prepared','committed'):
            self.runpkg('install',p)
            self.assertEqual(self.runpkg('remove','demo',ok=False,stop=point).returncode,99)
            self.runpkg('recover'); self.assertFalse((self.root/'usr/bin/demo').exists())
        txn=self.root/'var/lib/pkg/txn'; txn.mkdir(); (txn/'new').write_text('partial')
        self.runpkg('recover'); self.assertFalse(txn.exists())
        stage=self.root/'var/lib/pkg/stage'; stage.mkdir(); (stage/'partial').write_text('interrupted extraction')
        self.runpkg('recover'); self.assertFalse(stage.exists())

    def test_symlinks_are_not_payload_or_install_parents(self):
        p,stage,meta=self.package(); (stage/'link').symlink_to('/etc/passwd')
        self.runpkg('create',meta,stage,self.work/'bad.rpk',ok=False)
        outside=self.work/'outside'; outside.mkdir(); (self.root/'usr').symlink_to(outside)
        self.runpkg('install',p,ok=False); self.assertFalse((outside/'bin/demo').exists())

    def test_repository_objects_are_verified(self):
        p,_,_=self.package(); repo=self.work/'repo'; repo.mkdir(); shutil.copy(p,repo/'demo.rpk')
        self.runpkg('repo',repo)
        text=(repo/'index.v1').read_text(); digest=hashlib.sha256(p.read_bytes()).hexdigest()
        self.assertIn(digest,text); self.assertEqual((repo/'objects'/f'{digest[:24]}.rpk').read_bytes(),p.read_bytes())
        (repo/'objects'/f'{digest[:24]}.rpk').write_bytes(b'corrupt')
        self.runpkg('repo',repo,ok=False)

    def test_seed_migration_and_configs(self):
        p,stage,meta=self.package(path='usr/bin/demo',data=b'new')
        (stage/'bin').mkdir(); (stage/'bin/demo').write_bytes(b'#!/bin/sh\nexec /usr/bin/demo "$@"\n');(stage/'bin/demo').chmod(0o755)
        (stage/'etc').mkdir();(stage/'etc/demo.conf').write_text('default')
        self.runpkg('create',meta,stage,p)
        (self.root/'bin').mkdir();(self.root/'bin/demo').write_bytes(b'old');(self.root/'bin/demo').chmod(0o755)
        (self.root/'etc').mkdir();(self.root/'etc/demo.conf').write_text('my custom config')
        self.runpkg('install',p,ok=False)
        self.assertEqual(self.runpkg('--adopt-seed','install',p,ok=False,stop='prepared').returncode,99)
        self.runpkg('recover')
        self.assertEqual((self.root/'usr/bin/demo').read_bytes(),b'new')
        self.assertEqual((self.root/'etc/demo.conf').read_text(),'my custom config')
        self.assertIn('version=seed',(self.root/'var/lib/pkg/db/demo.prev').read_text())
        self.assertTrue(any(f.read_bytes()==b'old' for f in (self.root/'var/lib/pkg/objects').glob('*/root/bin/demo')))
        self.runpkg('clean');self.assertEqual(len(list((self.root/'var/lib/pkg/objects').iterdir())),2)
        self.runpkg('--discard-previous','clean');self.assertEqual(len(list((self.root/'var/lib/pkg/objects').iterdir())),1)
        self.runpkg('remove','demo');self.assertEqual((self.root/'etc/demo.conf').read_text(),'my custom config')

    def test_seed_alias_recovery_and_recovery_shell_guard(self):
        p,stage,meta=self.package(path='usr/bin/demo')
        (stage/'usr/bin/alias').write_text('alias command')
        (stage/'bin').mkdir()
        for n in ('demo','alias'):
            (stage/'bin'/n).write_text('#!/bin/sh\nexec /usr/bin/'+n+' "$@"\n')
        self.runpkg('create',meta,stage,p)
        (self.root/'bin').mkdir();(self.root/'bin/demo').write_text('original')
        (self.root/'bin/alias').symlink_to('demo')
        self.assertEqual(self.runpkg('--adopt-seed','install',p,ok=False,stop='linked').returncode,99)
        self.runpkg('recover');self.runpkg('recover')
        self.assertIn('/usr/bin/alias',(self.root/'bin/alias').read_text())
        snapshots=list((self.root/'var/lib/pkg/objects').glob('*/root/bin/alias'))
        self.assertTrue(any(f.read_text()=='demo' for f in snapshots))
        protected,st,mt=self.package('shell',path='usr/bin/sh')
        (st/'bin').mkdir();(st/'bin/sh').write_text('unsafe')
        self.runpkg('create',mt,st,protected)
        self.runpkg('--adopt-seed','install',protected,ok=False)
        self.assertFalse((self.root/'bin/sh').exists())

    def test_default_config_upgrade_and_wildcard_dependency(self):
        p,stage,meta=self.package();(stage/'etc').mkdir();(stage/'etc/demo.conf').write_text('one')
        self.runpkg('create',meta,stage,p);self.runpkg('install',p)
        dep,_,_=self.package('client',depends='demo@*',path='usr/bin/client');self.runpkg('install',dep)
        p2,stage2,meta2=self.package(version='2');(stage2/'etc').mkdir();(stage2/'etc/demo.conf').write_text('two')
        self.runpkg('create',meta2,stage2,p2);self.runpkg('install',p2)
        self.assertEqual((self.root/'etc/demo.conf').read_text(),'two')
        self.runpkg('remove','demo',ok=False)

    def test_clean_support_retains_previous_dependencies(self):
        old='fxlib-1111111111111111';new='fxlib-2222222222222222'
        a,_,_=self.package(old,path='usr/lib/old');self.runpkg('install',a)
        b,_,_=self.package(new,path='usr/lib/new');self.runpkg('install',b)
        c,_,_=self.package('client',depends=old+'@1',path='usr/bin/client');self.runpkg('install',c)
        d,_,_=self.package('client','2',depends=new+'@1',path='usr/bin/client');self.runpkg('install',d)
        self.runpkg('clean');self.assertTrue((self.root/'usr/lib/old').exists())
        self.runpkg('--discard-previous','clean');self.assertFalse((self.root/'usr/lib/old').exists())
        self.assertTrue((self.root/'usr/lib/new').exists())
        self.runpkg('remove','client');self.runpkg('clean')
        self.assertFalse((self.root/'usr/lib/new').exists())

    def test_signed_repository_network_and_tampering(self):
        p,_,_=self.package(); repo=self.work/'repo'; repo.mkdir(); shutil.copy(p,repo/'demo.rpk')
        self.runpkg('repo',repo)
        private=self.work/'key.pem'
        subprocess.run(['openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',str(private)],check=True,capture_output=True)
        pub=subprocess.check_output(['openssl','rsa','-in',str(private),'-noout','-modulus']).decode().strip().split('=')[1].lower()
        keydir=self.root/'etc/pkg'; keydir.mkdir(parents=True); (keydir/'repo.pub').write_text(pub+'\n')
        subprocess.run(['openssl','dgst','-sha256','-sign',str(private),'-out',str(repo/'index.sig'),str(repo/'index.v1')],check=True)
        self.runpkg('verify',repo/'index.v1',repo/'index.sig',keydir/'repo.pub')
        class Quiet(http.server.SimpleHTTPRequestHandler):
            def log_message(self,*args): pass
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=repo))
        thread=threading.Thread(target=server.serve_forever,daemon=True); thread.start()
        try:
            (keydir/'repo.conf').write_text(f'url=http://127.0.0.1:{server.server_port}\n')
            self.runpkg('update'); self.runpkg('add','demo')
            self.assertEqual((self.root/'usr/bin/demo').read_bytes(),b'one\x00\xff\x80')
            self.runpkg('remove','demo')
            cached=self.root/'var/cache/pkg/current/index.v1'
            cached.write_bytes(cached.read_bytes().replace(b'demo',b'evil'))
            self.runpkg('add','demo',ok=False); self.runpkg('update')
            archive=next((repo/'objects').glob('*.rpk')); archive.write_bytes(b'x'*len(archive.read_bytes()))
            self.runpkg('add','demo',ok=False)
            (repo/'index.v1').write_text('RIPINDEX1\n')
            self.runpkg('update',ok=False)
            self.assertIn(b'demo',(self.root/'var/cache/pkg/current/index.v1').read_bytes())
        finally: server.shutdown(); server.server_close(); thread.join()

if __name__=='__main__': unittest.main(verbosity=2)
