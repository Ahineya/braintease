#!/usr/bin/env python3
"""Check the alloca replacement on full host engines before C16 integration."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from adapt import replace
from core_frames import heap_frames
from vendor import PORT, verify
ROOT=PORT.parents[1]
verify()
build=ROOT/'build/quickjs-frames';build.mkdir(parents=True,exist_ok=True)
core=heap_frames((PORT/'upstream/quickjs.c').read_text())
(build/'quickjs.c').write_text(core)
fixture=(PORT/'tests/embed.c').read_text()
fixture=replace(fixture,'static unsigned checks, callbacks, interrupts, unhandled;', '''static unsigned checks, callbacks, interrupts, unhandled;
static JSValue native_data(JSContext *ctx, JSValueConst self, int argc,
                            JSValueConst *argv, int magic, JSValue *data) {
    (void)self;
    if (argc == 0 && !JS_IsUndefined(argv[2])) return JS_ThrowInternalError(ctx,"padding");
    return JS_NewInt32(ctx, JS_VALUE_GET_INT(data[0]) + magic + argc);
}
''')
fixture=replace(fixture,'    if (!eval_ok(ctx, source, JS_EVAL_TYPE_GLOBAL)) goto done;', '''    {
        JSValue data = JS_NewInt32(ctx, 42);
        JSValue fun = JS_NewCFunctionData(ctx, native_data, 3, 7, 1, &data);
        if (JS_IsException(fun) || JS_SetPropertyStr(ctx, global, "nativeData", fun) < 0) goto done;
    }
    if (!eval_ok(ctx, source, JS_EVAL_TYPE_GLOBAL)) goto done;''')
(build/'embed.c').write_text(fixture)
semantics=(PORT/'tests/semantics.js').read_text()+'''
check(nativeData() === 49 && nativeData(1,2,3,4) === 53, 'C data callback padding');
function Bound(a,b) { this.total = a+b; }
const bound = Bound.bind(null, 10);
check(new bound(32).total === 42, 'bound constructor');
check(((a,b) => a+b).bind(null,10)(32) === 42, 'bound ordinary function');
let boundThrown = false;
try { (() => { throw 123; }).bind(null)(); } catch(e) { boundThrown = e === 123; }
check(boundThrown, 'bound exception cleanup');
function *gen() { let x=40; yield () => ++x; yield x+1; return x+2; }
const generator=gen(), held=generator.next().value;
nativeGC();
check(held() === 41 && generator.next().value === 42 && generator.next().value === 43,
      'suspended generator locals retain ownership');
check(generator.next().done, 'completed generator cleanup');
let recurse;
function outer() { let x=42; return () => x; }
for(let i=0;i<1000;i++) recurse=outer();
check(recurse()===42, 'closed references outlive freed frames');
function missing(a,b,c) { return a===undefined && b===undefined && c===undefined; }
check(missing(), 'bytecode argument padding');
let recovered=false;
try { (function recursive(){recursive();})(); } catch(e) { recovered=e instanceof InternalError; }
check(recovered && outer()()===42, 'stack exhaustion and recovery');
'''
(build/'semantics.js').write_text(semantics)
flags=['-std=gnu11','-O2','-fwrapv','-D_GNU_SOURCE','-DCONFIG_VERSION="2026-06-04"','-I'+str(PORT/'upstream')]
if sys.platform=='darwin':flags+=['-isysroot',os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()]
results={}
for profile,path in [('reference',PORT/'upstream/quickjs.c'),('heap-frames',build/'quickjs.c')]:
    binary=build/profile
    cmd=[os.environ.get('CC','cc'),*flags,build/'embed.c',path,*[PORT/'upstream'/(n+'.c') for n in ['dtoa','libunicode','libregexp','cutils']],'-lm','-o',binary]
    with (build/(profile+'-build.log')).open('w') as log:subprocess.run(list(map(str,cmd)),check=True,stdout=log,stderr=subprocess.STDOUT)
    output=subprocess.check_output([str(binary),str(build/'semantics.js')],text=True,timeout=30)
    results[profile]=output.strip();print(profile,output,flush=True)
    for label,source in [('assertion',"nativeAssert(false,'negative control')"),('promise',"globalThis.asyncDone=true; Promise.resolve().then(()=>nativeAssert(false,'async failure'))")]:
        p=build/(label+'.js');p.write_text(source)
        negative=subprocess.run([str(binary),str(p)],capture_output=True,timeout=30)
        assert negative.returncode==1 and b'QUICKJS_REFERENCE' not in negative.stdout
# The only retained-size difference is the runtime's active-frame counter.
import ctypes
fields=lambda text: dict(re.findall(r'(\w+)=(\d+)',text))
a,b=map(fields,[results['reference'],results['heap-frames']])
assert int(b.pop('memory'))-int(a.pop('memory'))==ctypes.sizeof(ctypes.c_size_t),results
assert a==b,results
(build/'report.json').write_text(json.dumps(results,indent=2)+'\n')

# Bypass the arena cache only for deterministic failure injection. The normal
# profile above retains upstream arenas and must preserve its allocation count.
no_arenas=build/'quickjs-no-arenas.c'
no_arenas.write_text(replace(core, '#define JS_MALLOC_LARGE_BLOCKS_ONLY 0',
                            '#define JS_MALLOC_LARGE_BLOCKS_ONLY 1'))
command=[os.environ.get('CC','cc'),*flags,PORT/'tests/frames.c',no_arenas,
         *[PORT/'upstream'/(n+'.c') for n in ['dtoa','libunicode','libregexp','cutils']],
         '-lm','-o',build/'frame-failures']
with (build/'failure-build.log').open('w') as log:
    subprocess.run(list(map(str,command)),check=True,stdout=log,stderr=subprocess.STDOUT)
output=subprocess.check_output([str(build/'frame-failures')],text=True,timeout=30)
assert 'QUICKJS_FRAMES checks=16 failures=0' in output,output
print(output,flush=True)
results['allocation_failures']=output.strip()
(build/'report.json').write_text(json.dumps(results,indent=2)+'\n')
