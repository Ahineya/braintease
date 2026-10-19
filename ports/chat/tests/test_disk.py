#!/usr/bin/env python3
"""Offline Chat transfer finds the account home on old and new C16 images."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
SETUP=r'''
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {writeImage} from './ports/fuzix/tools/image.mjs';
const [file,home,state]=process.argv.slice(1);
writeImage(file,rootImage([
 {name:'/etc/passwd',mode:0o100644,data:Buffer.from('root:x:0:0:Root:'+home+':/bin/sh\n')},
 {name:state+'/host-id',mode:0o100600,data:Buffer.from('urn:uuid:01234567-89ab-cdef-0123-456789abcdef')}
],2048,128),true);
'''


class DiskTests(unittest.TestCase):
    def test_account_home_legacy_and_explicit_override(self):
        for home,state,explicit in [('/root','/root/.chat',False),
                                    ('/','/.chat',False),
                                    ('/home/my user','/home/my user/.chat',False),
                                    ('/root','/custom-chat',True)]:
            with self.subTest(home=home,state=state), tempfile.TemporaryDirectory(prefix='chat-home-') as temp:
                disk=Path(temp)/'disk.img';request=Path(temp)/'request.json'
                subprocess.run(['node','--input-type=module','-e',SETUP,str(disk),home,state],cwd=ROOT,check=True)
                command=[sys.executable,str(ROOT/'ports/chat/disk.py'),'request',str(disk),str(request)]
                if explicit:command+=['--state',state]
                result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(json.loads(request.read_text())['ext_agent_host_id'],
                                 'urn:uuid:01234567-89ab-cdef-0123-456789abcdef')
                self.assertEqual(request.stat().st_mode&0o777,0o600)


if __name__=='__main__':unittest.main()
