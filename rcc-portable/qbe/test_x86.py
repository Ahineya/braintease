#!/usr/bin/env python3
"""Run the bootstrapped native compiler tools on freestanding x86_64 Riplix.

Private QEMU disks only. Raw C through RXE1 linking and execution runs in the
guest. The seed tools/kernel/SDK and independent test oracles are host-built.
"""
import argparse
import json
from pathlib import Path
import os
import re
import select
import shutil
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
sys.path.insert(0,str(ROOT/'tools/x86_64'))
import boot

BUILDER='''
import fs from 'node:fs';
import {openImage,writeImage} from './ports/fuzix/tools/image.mjs';
import {growRoot,installRoot} from './ports/fuzix/tools/install.mjs';
const [seed,out,manifest]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest,'utf8')).map(f=>({...f,data:fs.readFileSync(f.path)}));
// C16 file octets occupy two physical bytes. Generation-two tool inputs plus
// live compiler intermediates need more than the former 64 MiB test disk.
writeImage(out,installRoot(growRoot(openImage(seed),65536),files));
'''
EXTRACTOR='''
import fs from 'node:fs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
import {BLOCK,CELLS} from './ports/fuzix/tools/layout.mjs';
const [path,out,names]=process.argv.slice(1), checked=checkRoot(openImage(path));
if(checked.issues.length)throw Error(checked.issues.join('\\n'));
function cells(n){const a=new Uint16Array(n.size);for(const [i,b]of n.map)for(let j=0;j<CELLS&&i*CELLS+j<n.size;j++)a[i*CELLS+j]=checked.image.readUInt16LE(b*BLOCK+j*2);return a;}
function lookup(node,name){const a=cells(node);for(let i=0;i<a.length;i+=32){let text='';for(let j=1;j<=30&&a[i+j];j++)text+=String.fromCharCode(a[i+j]);if(a[i]&&text===name)return checked.nodes.get(a[i]);}throw Error('missing '+name);}
for(const name of JSON.parse(names)){const n=lookup(lookup(checked.nodes.get(1),'tmp'),name),a=cells(n);if(a.some(v=>v>255))throw Error('non-octet tool output');fs.writeFileSync(out+'/'+name,Buffer.from(a));}
'''


def run_guest(out, phase, files, commands, timeout, seed_root=None):
    accelerator=os.environ.get('RCC_TEST_ACCEL','tcg')
    if accelerator not in ('tcg','kvm'):
        raise ValueError('RCC_TEST_ACCEL must be tcg or kvm')
    root=out/(phase+'-root.img')
    source=Path(seed_root) if seed_root is not None else out/'normal/root.img'
    if source.resolve()==root.resolve():
        raise ValueError('guest source disk must be separate from the writable test image')
    manifest=out/(phase+'-files.json');manifest.write_text(json.dumps(files))
    subprocess.run(['node','--input-type=module','-e',BUILDER,str(source),str(root),str(manifest)],cwd=ROOT,check=True)
    firmware_code,firmware_vars=boot.firmware(argparse.Namespace(firmware_code=None,firmware_vars=None))
    variables=out/(phase+'-vars.fd');shutil.copyfile(firmware_vars,variables)
    command=[boot.executable('qemu-system-x86_64'),'-machine','q35','-accel',accelerator,'-cpu','qemu64',
        '-m','256M','-smp','1','-no-reboot','-nic','none','-monitor','none','-display','none',
        '-drive',f'if=pflash,format=raw,readonly=on,file={firmware_code}',
        '-drive',f'if=pflash,format=raw,file={variables}',
        '-drive',f'format=raw,snapshot=on,file={out/"normal/boot.img"}',
        '-blockdev',json.dumps(dict(driver='raw',**{'node-name':'compiler-root'},file=dict(driver='file',filename=str(root)))),
        '-device','virtio-blk-pci,drive=compiler-root,disable-legacy=on,num-queues=1,queue-size=8','-serial','stdio']
    process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    transcript=bytearray();at=0;deadline=time.monotonic()+timeout
    def read(marker):
        nonlocal at
        while marker not in transcript[at:]:
            if process.poll() is not None or time.monotonic()>deadline:
                raise RuntimeError('x64 test stopped/timed out:\n'+transcript[-5000:].decode(errors='replace'))
            if select.select([process.stdout],[],[],.05)[0]:
                transcript.extend(os.read(process.stdout.fileno(),65536))
                (out/(phase+'.live.log')).write_bytes(transcript)
            if b'BOOT_FAIL' in transcript or b'EXCEPTION vector=' in transcript or b'RIPLIX_X86_64_BOOT_OK' in transcript:
                until=time.monotonic()+1
                while time.monotonic()<until:
                    if select.select([process.stdout],[],[],.05)[0]:
                        transcript.extend(os.read(process.stdout.fileno(),65536))
                raise RuntimeError('x64 test fault:\n'+transcript[-5000:].decode(errors='replace'))
        end=transcript.index(marker,at)+len(marker);result=bytes(transcript[at:end]);at=end
        return result.replace(b'\r',b'').decode(errors='replace')
    rows=[]
    try:
        read(b'fuse-x# ')
        for line,expected in commands:
            start=time.monotonic();process.stdin.write(line.encode()+b'\r');process.stdin.flush()
            text=read(b'fuse-x# ')
            if expected not in text:raise RuntimeError('command did not pass:\n'+text)
            row=dict(command=line,seconds=round(time.monotonic()-start,3),accelerator=accelerator)
            match=re.search(r'heap peak (\d+) cells; allocations (\d+)',text)
            if match:row['heap_peak_cells']=int(match[1])
            rows.append(row);print(json.dumps(row),flush=True)
        process.stdin.write(b'shutdown\r');process.stdin.flush();read(b'filesystem synced; session stopped')
    finally:
        (out/(phase+'.log')).write_bytes(transcript)
        if process.poll() is None:process.terminate()
        try:process.wait(timeout=10)
        except subprocess.TimeoutExpired:process.kill();process.wait()
        (out/(phase+'-qemu.log')).write_bytes(process.stderr.read())
        process.stdin.close();process.stdout.close();process.stderr.close()
    return root,rows


def file(name,path):return dict(name=name,path=str(path.resolve()),mode=0o100755 if name.startswith('/bin/') else 0o100644)


def exit_probe(body):
    # Existing C16 syscall packet; this is a test harness for conventional
    # native QBE functions, not an alternative application calling convention.
    return '''# rcc-entry 1
# rcc-function 1 1 rcc_user_entry
export function l $rcc_user_entry(l %ctx) {
@entry
 %request =l call $rcc_native_process_frame(l %ctx, l 10)
 %pc =l call $rcc_native_process_pc(l %ctx)
 %first =w ceql %pc, 0
 jnz %first, @run, @bad
@bad
 call $rcc_native_process_bad_pc(l %ctx)
 ret 0
@run
''' + body + '''
 call $rcc_native_store(l %ctx, l %request, l 0, l 4)
 %tail4 =l call $rcc_native_gep(l %ctx, l %request, l 4)
 call $rcc_native_store(l %ctx, l %tail4, l 0, l 4)
 %tail8 =l call $rcc_native_gep(l %ctx, l %request, l 8)
 call $rcc_native_store(l %ctx, l %tail8, l 0, l 2)
 %arg =l call $rcc_native_gep(l %ctx, l %request, l 1)
 call $rcc_native_store(l %ctx, l %arg, l %wide, l 2)
 call $rcc_native_process_set_pc(l %ctx, l 1)
 call $rcc_native_process_request(l %ctx, l 12, l %request, l 0, l 0)
 ret 0
}
'''


def link_features(out, a):
    """Exercise multiple real ELF objects and RXE1 address fixups in x64."""
    source=out/'pointers-exec.ssa'
    source.write_text(exit_probe(' %status =w call $native_pointer_probe()\n %wide =l extuw %status\n'))
    files=[file('/bin/rcc-x64-ld',a.linker),file('/bin/rcc-x64-as',a.assembler),file('/bin/qbe',a.qbe),
           file('/pointers-exec.ssa',source),file('/pointers.s',HERE.parent/'x64-link/tests/pointers.s')]
    commands=[
        ('qbe -t amd64_sysv -o /tmp/pointers-exec.s /pointers-exec.ssa; echo QBE_STATUS:$?','QBE_STATUS:0'),
        ('rcc-x64-as /tmp/pointers-exec.s -o /tmp/pointers-exec.o; echo AS_STATUS:$?','AS_STATUS:0'),
        ('rcc-x64-as /pointers.s -o /tmp/pointers.o; echo AS_STATUS:$?','AS_STATUS:0'),
        ('rcc-x64-ld --metadata /pointers-exec.ssa /tmp/pointers-exec.o /tmp/pointers.o -o /tmp/pointers; echo LINK_STATUS:$?','LINK_STATUS:0'),
        ('/tmp/pointers; echo POINTER_STATUS:$?','POINTER_STATUS:0'),
        ('/tmp/pointers; echo RELOAD_STATUS:$?','RELOAD_STATUS:0')]
    _,rows=run_guest(out,'link-features',files,commands,a.timeout)
    return rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=ROOT/'build/portable-native-x64')
    p.add_argument('--qbe',type=Path,default=ROOT/'build/guest-native-assessment/qbe-x86.fxe')
    p.add_argument('--assembler',type=Path,default=ROOT/'build/guest-native-assessment/rcc-x64-as-x86.fxe')
    p.add_argument('--lowerer',type=Path,default=HERE.parent/'build/apps/rcc-qbe-x86.fxe')
    p.add_argument('--frontend',type=Path,default=HERE.parent/'build/apps/rcc-c-x86.fxe')
    p.add_argument('--linker',type=Path,default=HERE.parent/'build/apps/rcc-x64-ld-x86.fxe')
    p.add_argument('--driver',type=Path,default=HERE.parent/'build/apps/rcc-native-cc-x86.fxe')
    p.add_argument('--link-only',action='store_true',help='rerun only multi-object relocation execution')
    p.add_argument('--skip-build',action='store_true');p.add_argument('--timeout',type=int,default=600)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    if not a.skip_build:subprocess.run(['python3',str(ROOT/'tools/x86_64/boot.py'),'build','--font','rom','--output',str(out)],check=True)
    if a.link_only:
        rows=link_features(out,a)
        (out/'link-features-result.json').write_text(json.dumps(rows,indent=2)+'\n')
        return
    (out/'compiler-test-result.json').unlink(missing_ok=True)
    qbe_fixtures=['spill1','fpcnv','vararg1','mandel'];native_fixtures=['scalar','floating','phi','arithmetic']
    files=[file('/bin/qbe',a.qbe),file('/bin/rcc-x64-as',a.assembler),file('/bin/rcc-qbe',a.lowerer),file('/bin/rcc-c',a.frontend)]
    files += [file('/bin/rcc-x64-ld',a.linker),file('/bin/rcc-native-cc',a.driver)]
    files.append(file('/probe.h',HERE.parent/'native/tests/probe.h'))
    for name in qbe_fixtures:files.append(file('/'+name+'.ssa',HERE.parent/'upstream/qbe/test'/(name+'.ssa')))
    for name in native_fixtures:
        source=HERE.parent/'native/tests'/(name+'.c');ir=out/(name+'.ir')
        if source.exists():
            subprocess.run(['python3',str(HERE.parent/'cc.py'),str(source),'--target','native','--emit-ir','-o',str(ir)],check=True)
            shutil.copyfile(ir,out/(name+'.host.ir'))
            files.append(file('/'+name+'.c',source))
        else:
            shutil.copyfile(source.with_suffix('.ir'),ir)
            files.append(file('/'+name+'.ir',ir))
    malformed=out/'malformed.ssa';malformed.write_text('function w $broken() {\n@entry\n invalid\n}\n')
    files.append(file('/malformed.ssa',malformed))
    bad_register=out/'bad-register.s';bad_register.write_text('.text\ncvttsd2si %rax, %rax\n')
    files.append(file('/bad-register.s',bad_register))
    driver_source=out/'driver.c';driver_source.write_text('#include "probe.h"\n#ifndef ANSWER\n#error missing command-line macro\n#endif\nint main(void) { return ANSWER + (int)PROBE_BASE; }\n')
    files.append(file('/driver.c',driver_source))
    unsupported=out/'unsupported.c';unsupported.write_text('int main(void) { volatile double x=1.5; return (int)x; }\n')
    files.append(file('/unsupported.c',unsupported))
    commands=[]
    for name in qbe_fixtures+native_fixtures:
        if name in ('scalar','floating'):
            commands.append((f'rcc-c --target native /{name}.c -o /tmp/{name}.ir; echo FRONTEND_STATUS:$?','FRONTEND_STATUS:0'))
        if name in native_fixtures:
            ir='/tmp/'+name+'.ir' if name in ('scalar','floating') else '/'+name+'.ir'
            commands.append((f'rcc-qbe {ir} -o /tmp/{name}.ssa; echo LOWER_STATUS:$?','LOWER_STATUS:0'))
        source='/tmp/'+name+'.ssa' if name in native_fixtures else '/'+name+'.ssa'
        commands.append((f'qbe -t amd64_sysv -o /tmp/{name}.s {source}; echo QBE_STATUS:$?','QBE_STATUS:0'))
        commands.append((f'rcc-x64-as /tmp/{name}.s -o /tmp/{name}.o; echo AS_STATUS:$?','AS_STATUS:0'))
        if name in native_fixtures:
            commands.append((f'rcc-x64-ld --metadata /tmp/{name}.ssa /tmp/{name}.o -o /tmp/{name}.fxe; echo LINK_STATUS:$?','LINK_STATUS:0'))
            commands.append((f'/tmp/{name}.fxe; echo EXEC_STATUS:$?','EXEC_STATUS:0'))
    commands.append(('echo preserved >/tmp/preserve; qbe -o /tmp/preserve /malformed.ssa; echo REJECT:$?; cat /tmp/preserve','REJECT:1\npreserved'))
    commands.append(('echo preserved >/tmp/as_keep; rcc-x64-as /bad-register.s -o /tmp/as_keep; echo REJECT:$?; cat /tmp/as_keep','REJECT:1\npreserved'))
    commands.append(('echo preserved >/tmp/ld_keep; rcc-x64-ld --metadata /tmp/scalar.ssa /tmp/spill1.o -o /tmp/ld_keep; echo REJECT:$?; cat /tmp/ld_keep','REJECT:1\npreserved'))
    commands.append(('rcc-native-cc -c /scalar.c -o /tmp/driver-scalar.o; echo DRIVER_STATUS:$?','DRIVER_STATUS:0'))
    commands.append(('rcc-native-cc /tmp/driver-scalar.o -o /tmp/driver-scalar; echo LINK_STATUS:$?','LINK_STATUS:0'))
    commands.append(('/tmp/driver-scalar; echo EXEC_STATUS:$?','EXEC_STATUS:0'))
    commands.append(('rcc-native-cc -c /floating.c -o /tmp/driver-floating.o; echo DRIVER_STATUS:$?','DRIVER_STATUS:0'))
    commands.append(('rcc-native-cc /tmp/driver-floating.o -o /tmp/driver-floating; echo LINK_STATUS:$?','LINK_STATUS:0'))
    commands.append(('/tmp/driver-floating; echo EXEC_STATUS:$?','EXEC_STATUS:0'))
    commands.append(('rcc-native-cc -DANSWER=30 /driver.c -o /tmp/driver-flags; echo DRIVER_STATUS:$?','DRIVER_STATUS:0'))
    commands.append(('/tmp/driver-flags; echo EXEC_STATUS:$?','EXEC_STATUS:37'))
    commands.append(('echo preserved >/tmp/cc_keep; rcc-native-cc /unsupported.c -o /tmp/cc_keep; echo REJECT:$?; cat /tmp/cc_keep','REJECT:1\npreserved'))
    root,compile_rows=run_guest(out,'compiler-test',files,commands,a.timeout)
    outputs=[name+'.'+ext for name in qbe_fixtures+native_fixtures for ext in ['s','o']]
    outputs += [name+'.ssa' for name in native_fixtures]
    outputs += [name+'.ir' for name in ('scalar','floating')]
    outputs += [name+'.fxe' for name in native_fixtures]+['driver-scalar','driver-floating','driver-flags','driver-scalar.o','driver-floating.o']
    subprocess.run(['node','--input-type=module','-e',EXTRACTOR,str(root),str(out),json.dumps(outputs)],cwd=ROOT,check=True)
    # Exact host/guest object parity; the host assembler is an independently
    # compiled copy of the same portable source, LLVM tests are separate.
    for name in qbe_fixtures+native_fixtures:
        reference=out/(name+'.host.o')
        subprocess.run([str(HERE.parent/'build/rcc-x64-as'),str(out/(name+'.s')),'-o',str(reference)],check=True)
        assert (out/(name+'.o')).read_bytes()==reference.read_bytes(),name+' object differs'
    for name in native_fixtures:
        if name in ('scalar','floating'):
            assert (out/(name+'.ir')).read_bytes()==(out/(name+'.host.ir')).read_bytes(),name+' frontend IR differs'
        reference=out/(name+'.host.ssa')
        subprocess.run([str(HERE.parent/'build/rcc-qbe'),str(out/(name+'.ir')),'-o',str(reference)],check=True)
        assert (out/(name+'.ssa')).read_bytes()==reference.read_bytes(),name+' lowering differs'
        linked=out/(name+'.host.fxe')
        subprocess.run([str(HERE.parent/'build/rcc-x64-ld'),'--metadata',str(out/(name+'.ssa')),str(out/(name+'.o')),'-o',str(linked)],check=True)
        assert (out/(name+'.fxe')).read_bytes()==linked.read_bytes(),name+' guest link differs'
    for name in ('scalar','floating'):
        # QBE permits equal-cost spill candidates to compare equal. The guest
        # heapsort and host libc qsort may choose different valid allocations;
        # compare linkers on the actual guest-produced module, then execute it.
        image=out/(name+'.module.fxe')
        subprocess.run([str(HERE.parent/'build/rcc-x64-ld'),str(out/('driver-'+name+'.o')),'-o',str(image)],check=True)
        assert (out/('driver-'+name)).read_bytes()==image.read_bytes(),name+' driver differs'
    # Execute the differently allocated spill1 output, not merely parse it.
    # The original fpcnv test's conventional C driver is compiled only as a
    # test oracle; the actual tested routines come from guest QBE + assembler.
    spill=out/'spill-exec.ssa'
    spill.write_text(exit_probe('''
 %a =w call $f(w 1)
 %b =w call $f(w 7)
 %c =w call $f(w 1000)
 %x =w cnew %a, 136
 %y =w cnew %b, 952
 %z =w cnew %c, 136000
 %xy =w or %x, %y
 %status =w or %xy, %z
 %wide =l extuw %status
'''))
    spill_s=out/'spill-exec.s'
    subprocess.run([str(HERE.parent/'build/qbe-host/qbe'),'-t','amd64_sysv','-o',str(spill_s),str(spill)],check=True)
    spill_wrapper=out/'spill-wrapper.o'
    subprocess.run([str(HERE.parent/'build/rcc-x64-as'),str(spill_s),'-o',str(spill_wrapper)],check=True)
    # Run the upstream FP-conversion driver's independent expected results.
    # Clang supplies only the conventional-C test harness, never the routines
    # being tested: those are the object emitted by our tools inside x64.
    fp=out/'fpcnv-exec.ssa'
    fp.write_text(exit_probe('''
 %status =w call $qbe_test_case()
 %wide =l extuw %status
'''))
    upstream=(HERE.parent/'upstream/qbe/test/fpcnv.ssa').read_text()
    driver=upstream.split('# >>> driver\n',1)[1].split('# <<<',1)[0]
    driver='\n'.join(line[2:] if line.startswith('# ') else '' for line in driver.splitlines())+'\n'
    driver_source=out/'fpcnv-driver.c';driver_object=out/'fpcnv-driver.o';driver_source.write_text(driver)
    subprocess.run([boot.executable('clang'),'--target=x86_64-unknown-none-elf','-ffreestanding','-fPIC','-O2',
                    '-mno-red-zone','-fno-stack-protector','-fno-addrsig','-fno-ident',
                    '-fvisibility=hidden','-Dmain=qbe_test_case','-c',str(driver_source),'-o',str(driver_object)],check=True)
    fp_s=out/'fpcnv-exec.s';fp_wrapper=out/'fpcnv-wrapper.o'
    subprocess.run([str(HERE.parent/'build/qbe-host/qbe'),'-t','amd64_sysv','-o',str(fp_s),str(fp)],check=True)
    subprocess.run([str(HERE.parent/'build/rcc-x64-as'),str(fp_s),'-o',str(fp_wrapper)],check=True)
    execution_files=[file('/bin/rcc-x64-ld',a.linker)]
    execution_names=native_fixtures+['spill-exec','fpcnv-exec']
    execute_commands=[]
    for name in execution_names:
        if name in native_fixtures:
            execution_files.append(file('/bin/'+name,out/(name+'.fxe')))
        else:
            objects=[out/'spill1.o',spill_wrapper] if name=='spill-exec' else [out/'fpcnv.o',driver_object,fp_wrapper]
            execution_files += [file('/'+obj.name,obj) for obj in objects]+[file('/'+name+'.ssa',out/(name+'.ssa'))]
            args=' '.join('/'+obj.name for obj in objects)
            execute_commands.append((f'rcc-x64-ld --metadata /{name}.ssa {args} -o /tmp/{name}; echo LINK_STATUS:$?','LINK_STATUS:0'))
        program=name if name in native_fixtures else '/tmp/'+name
        execute_commands.append((program+'; echo EXEC_STATUS:$?','EXEC_STATUS:0'))
    _,execute_rows=run_guest(out,'execute-test',execution_files,execute_commands,a.timeout)
    link_rows=link_features(out,a)
    result=dict(status='passed',architecture='x86_64',compilation=compile_rows,execution=execute_rows,link_features=link_rows,
                host_dependencies=['seed tools/kernel/SDK','independent LLVM/oracle fixtures'])
    (out/'compiler-test-result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('x86 guest C/IR/QBE/assembly/linking, native driver, output parity and execution passed')


if __name__=='__main__':main()
