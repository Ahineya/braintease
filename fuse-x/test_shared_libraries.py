#!/usr/bin/env python3
"""Exercise guest library resolution, code sharing and private process state."""
import argparse
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent

LIBRARY = r'''
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
int shared_counter=7;
char shared_bytes[]="abc";
char *shared_pointer=shared_bytes+1;
int shared_callback(int (*callback)(int)) { return callback(shared_counter); }
void *shared_allocate(void) { return malloc(37); }
void shared_release(void *p) { free(p); }
int shared_error(void) { return open("/no-such-shared-file",O_RDONLY); }
int shared_format(char *out) {return sprintf(out,"%d:%s",42,"ok");}
FILE *shared_stream(void) { return fopen("/tmp/shared-stream","w+"); }
'''
APPLICATION = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
extern int shared_counter;
extern char *shared_pointer;
extern char shared_bytes[4];
extern int shared_callback(int (*)(int));
static char *imported_pointer=shared_bytes+2;
static int (*imported_function)(int (*)(int))=shared_callback;
extern void *shared_allocate(void);
extern void shared_release(void *);
extern int shared_error(void);
extern int shared_format(char *);
extern FILE *shared_stream(void);
static int plus(int value) {return value+5;}
static int compare(const void *a,const void *b) {return *(const int *)a-*(const int *)b;}
static void done(void) {puts("SHARED_ATEXIT_OK");}
#define CHECK(x) do {if(!(x)){printf("SHARED_FAILED line %d errno %d\n",__LINE__,errno);return 1;}} while(0)
int main(int argc,char **argv) {
    int pid,status,i,array[3]={3,1,2};void *p;FILE *f;char text[8];
    CHECK(shared_counter==7 && shared_pointer[0]=='b' && shared_pointer[1]=='c');
    if(argc>1) {shared_counter=99;sleep(1);puts("SHARED_EXEC_FRESH_OK");return 21;}
    CHECK(atexit(done)==0);
    CHECK(shared_callback(plus)==12 && imported_function(plus)==12 && *imported_pointer=='c');
    CHECK(shared_format(text)==5 && strcmp(text,"42:ok")==0);
    qsort(array,3,sizeof(int),compare);CHECK(array[0]==1 && array[2]==3);
    p=shared_allocate();CHECK(p);memset(p,42,37);free(p);
    p=malloc(19);CHECK(p);shared_release(p);
    errno=0;CHECK(shared_error()==-1 && errno==ENOENT);
    f=shared_stream();CHECK(f);CHECK(fputs("hello",f)>=0);CHECK(fflush(f)==0);
    rewind(f);CHECK(fgets(text,sizeof(text),f) && strcmp(text,"hello")==0);CHECK(fclose(f)==0);
    pid=fork();CHECK(pid>=0);
    if(!pid) {shared_counter=83;shared_pointer[0]='X';exit(17);}
    CHECK(waitpid(pid,&status,0)==pid && status==(17<<8));
    CHECK(shared_counter==7 && shared_pointer[0]=='b');
    shared_counter=25;
    for(i=0;i<2;i++) {
        pid=fork();CHECK(pid>=0);
        if(!pid) {char *args[]={"shared-probe","--hold",0};execv("/bin/shared-probe",args);exit(2);}
    }
    for(i=0;i<2;i++) {CHECK(wait(&status)>0 && status==(21<<8));}
    CHECK(shared_counter==25);
    {char *args[]={"bad-dependency",0};
     CHECK(execv("/bin/bad-dependency",args)==-1 && shared_counter==25);
     CHECK(execv("/bin/bad-interface",args)==-1 && errno==ENOEXEC && shared_counter==25);}
    puts("SHARED_LIBRARIES_OK");return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build/shared-test')
    p.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    args = p.parse_args()
    build, compiler = args.build.resolve(), args.compiler.resolve()
    env = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
               RCC_NATIVE_TIMEOUT='45', FUSE_X_MODULE_TRACE='1', RIPPLE_IMAGE_BACKUPS='0')
    def run(*command):
        subprocess.run(list(map(str,command)),check=True,env=env)
    with tempfile.TemporaryDirectory(prefix='fuse-x-shared-') as temporary:
        directory=Path(temporary)
        library_source=directory/'library.c';library_source.write_text(LIBRARY)
        application=directory/'application.c';application.write_text(APPLICATION)
        library_ir=directory/'library.ir'
        includes=[ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include']
        with library_ir.open('w') as out:
            subprocess.run([str(compiler),'compile',str(library_source),'--target','native','-DRIPPLE_MMU=1','--emit-ir',
                            *[f'-I{path}' for path in includes]],stdout=out,check=True)
        run('python3',ROOT/'fuse-x/cc.py','--shared','--sdk-only','--build',build,'--compiler',compiler,'-o',directory/'sdk')
        library=directory/'libprobe.fxl.1'
        run(compiler,'link-shared',library_ir,'--soname',library.name,'--library',f'{build}/lib/libc.fxl.1=/lib/fuse-x/libc.fxl.1','-o',library)
        program=directory/'program.fxe'
        run('python3',ROOT/'fuse-x/cc.py',application,'--shared','--build',build,'--compiler',compiler,
            '--library',f'{library}=/lib/fuse-x/{library.name}','-o',program)
        package=program.read_bytes();json_size,code_size=struct.unpack_from('<II',package,8)
        manifest=json.loads(package[16:16+json_size]);rest=package[16+json_size:]
        def malformed(name,mutate):
            m=json.loads(json.dumps(manifest));mutate(m);j=json.dumps(m,separators=(',',':')).encode()
            path=directory/name;path.write_bytes(package[:8]+struct.pack('<II',len(j),code_size)+j+rest);return path
        def change(m,field,value):
            next(d for d in m['module']['dependencies'] if d['name']=='libprobe.fxl.1')[field]=value
        missing=malformed('missing',lambda m:change(m,'path','/lib/fuse-x/absent.fxl.1'))
        bad_abi=malformed('bad-abi',lambda m:change(m,'interface','wrong-interface'))
        # Build a small disposable image in a single pass, without disk backups.
        image=directory/'root.img'
        files=[('/init',None,b'FXN1:init:1\n'),('/bin/sh',None,b'FXN1:sh:1\n'),
               ('/bin/shared-probe',program,None),('/bin/bad-dependency',missing,None),('/bin/bad-interface',bad_abi,None),
               ('/lib/fuse-x/libprobe.fxl.1',library,None),('/lib/fuse-x/libc.fxl.1',build/'lib/libc.fxl.1',None)]
        description=directory/'files.json'
        description.write_text(json.dumps([{'name':name,'mode':0o100755,'path':str(path) if path else None,'text':text.decode() if text else None} for name,path,text in files]))
        script="""import fs from 'node:fs';import {rootImage} from './ports/fuzix/tools/disk.mjs';import {writeImage} from './ports/fuzix/tools/image.mjs';const files=JSON.parse(fs.readFileSync(process.argv[1])).map(f=>({...f,data:f.path?fs.readFileSync(f.path):Buffer.from(f.text)}));writeImage(process.argv[2],rootImage(files,32768),true);"""
        run('node','--input-type=module','-e',script,description,image)
        scratch=directory/'code';scratch.mkdir();env['TMPDIR']=str(scratch)
        child=subprocess.Popen([str(build/'fuse-x'),'--run',str(image)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env)
        transcript=bytearray();pending=bytearray()
        def expect(marker,seconds=30):
            deadline=time.monotonic()+seconds
            while marker not in pending:
                assert time.monotonic()<deadline,transcript.decode(errors='replace')
                if select.select([child.stdout],[],[],.1)[0]:
                    chunk=os.read(child.stdout.fileno(),65536)
                    assert chunk,transcript.decode(errors='replace')
                    pending.extend(chunk);transcript.extend(chunk)
            del pending[:pending.index(marker)+len(marker)]
        try:
            expect(b'fuse-x# ')
            child.stdin.write(b'shared-probe\n');child.stdin.flush()
            expect(b'SHARED_LIBRARIES_OK');expect(b'SHARED_ATEXIT_OK');expect(b'fuse-x# ')
            child.stdin.write(b'shutdown\n');child.stdin.flush();expect(b'session stopped')
            assert child.wait(timeout=10)==0,transcript.decode(errors='replace')
        finally:
            if child.poll() is None:child.kill();child.wait()
        output=transcript.decode(errors='replace')
        assert 'SHARED_FAILED' not in output,output
        assert output.count('loaded shared code libprobe.fxl.1 ')==1,output
        assert output.count('loaded shared code libc.fxl.1 ')==1,output
        assert output.count('SHARED_EXEC_FRESH_OK')==2,output
        assert not list(scratch.iterdir()),list(scratch.iterdir())
        print(output)
    print('Shared libraries: independent exec code reuse, callbacks, imported data, libc ownership, fork/COW, exec reset, atexit and failed-exec rollback passed')


if __name__=='__main__':
    main()
