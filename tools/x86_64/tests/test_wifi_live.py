"""Live build guards and explicit manual IP configuration packaging."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest
HERE=Path(__file__).resolve().parents[1]
ROOT=HERE.parents[1]


class LiveProfile(unittest.TestCase):
    def test_profile_guards_before_build(self):
        cases=[(['--network','wifi'],'explicit --network-config'),
               (['--network','wifi','--network-config','missing'],'requires --ram-root'),
               (['--network-config','missing'],'requires --network')]
        for flags,message in cases:
            run=subprocess.run(['python3',str(HERE/'boot.py'),'build',*flags],capture_output=True,text=True)
            self.assertEqual(run.returncode,2)
            self.assertIn(message,run.stderr)

    def test_root_configuration_and_rejections(self):
        with tempfile.TemporaryDirectory() as directory:
            d=Path(directory);apps=d/'apps';apps.mkdir();(apps/'manifest.json').write_text('[]')
            cfg=d/'network.json';image=d/'root.img'
            valid=dict(address='192.0.2.7',netmask='255.255.255.0',gateway='192.0.2.1',dns='192.0.2.1')
            cfg.write_text(json.dumps(valid))
            command=['node',str(HERE/'riplix/root.mjs'),str(image),str(apps),'--network','--wifi-test','--network-config',str(cfg)]
            subprocess.run(command,capture_output=True,text=True,check=True)
            reader='''import {rootReader} from './ports/fuzix/tools/read-root.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
const r=rootReader(openImage(process.argv[1]));
console.log(JSON.stringify(Object.fromEntries(['/network.sh','/etc/resolv.conf'].map(p=>[p,String.fromCharCode(...r.read(p))]))));'''
            got=json.loads(subprocess.check_output(['node','--input-type=module','-e',reader,str(image)],cwd=ROOT,text=True))
            self.assertEqual(got['/etc/resolv.conf'],'nameserver 192.0.2.1\n')
            script=got['/network.sh'];self.assertIn('ifconfig eth0 192.0.2.7 netmask 255.255.255.0 gateway 192.0.2.1 up || exit 1',script)
            self.assertLess(script.index('if ping -c 3 192.0.2.1'),script.index('telnetd &'))
            subprocess.run(['/bin/sh','-n'],input=script,text=True,check=True)
            before=image.read_bytes()
            bad=[dict(valid,address='192.0.2.7; echo bad'),dict(valid,dns='999.1.1.1'),dict(valid,netmask='255.0.255.0'),
                 dict(valid,address='192.0.2.0'),dict(valid,address='192.0.2.255'),dict(valid,gateway='198.51.100.1'),
                 dict(valid,address='192.0.2.1'),dict(valid,extra='x'),dict(valid,dns='127.0.0.1')]
            for config in bad:
                cfg.write_text(json.dumps(config));run=subprocess.run(command,capture_output=True,text=True)
                self.assertNotEqual(run.returncode,0);self.assertEqual(image.read_bytes(),before)


if __name__=='__main__':unittest.main()
