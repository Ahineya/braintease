"""Host-side RXE2 module linking; the ISA linker still owns code placement."""
import copy
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from . import format as fmt
from .objects import expression, instruction, label, rewrite, slice_code

INITIALIZE = '''_rxe_init_all:
STORE RA, SB, SP
ADDI SP, SP, 1
STORE RAB, SB, SP
ADDI SP, SP, 1
STORE S0, SB, SP
ADDI SP, SP, 1
STORE S1, SB, SP
ADDI SP, SP, 1
LI S0, 1
LOAD S1, GP, R0
__rxe_init_loop:
BEQ S1, R0, __rxe_init_done
LOAD T0, GP, S0
ADDI S0, S0, 1
LOAD T1, GP, S0
ADDI S0, S0, 1
JALR RA, T1, T0
ADDI S1, S1, -1
BEQ R0, R0, __rxe_init_loop
__rxe_init_done:
CALL _init_globals
ADDI SP, SP, -1
LOAD S1, SB, SP
ADDI SP, SP, -1
LOAD S0, SB, SP
ADDI SP, SP, -1
LOAD RAB, SB, SP
ADDI SP, SP, -1
LOAD RA, SB, SP
RET
'''
SUPPORT = ('ripple_syscall','setjmp','longjmp','ripple_signal_entry','ripple_absolute_pointer')


def load_objects(paths):
    result=[]
    for path in paths:
        obj=json.loads(Path(path).read_text())
        result.extend([entry['object'] for entry in obj['objects']] if 'objects' in obj else [obj])
    return result


def split_startup(obj, library):
    boundary=obj['labels']['ripple_syscall']['absolute_address']//4
    return slice_code(obj,boundary if library else 0,len(obj['instructions']) if library else boundary)


def pack(paths, output, *, linker, assembler, name='', exports=None, dependencies=(), startup=None,map_output=None):
    objects=load_objects(paths)
    if startup:
        objects.insert(0,split_startup(load_objects([startup])[0],bool(name)))
    definitions={n for obj in objects for n in (*obj['labels'],*obj['data_labels'])}
    types={n:t for obj in objects for n,t in (obj.get('rcc') or {}).get('types',{}).items()}
    public={n for obj in objects for n in (obj.get('rcc') or {}).get('exports',[])}
    if name and startup:public.update(SUPPORT)
    selected=sorted(set(exports) if exports is not None else public) if name else []
    if set(selected)-definitions:raise ValueError('missing library exports: '+str(set(selected)-definitions))
    # Assembly support entries have a versioned, explicitly opaque interface.
    # C declarations/calls retain ordinary C16 pointer/register conventions.
    for symbol in SUPPORT:
        if symbol in definitions:types.setdefault(symbol,'c16-assembly-v1')
    missing=[symbol for symbol in selected if symbol not in types]
    if missing:raise ValueError('objects lack shared ABI metadata; rebuild with the current RCC: '+str(missing))
    deps=[];available={}
    for path,guest in dependencies:
        module=fmt.read(Path(path))
        if not module['flags'] or not guest.startswith('/'):raise ValueError('dependency must be a library with an absolute guest path')
        index=len(deps);deps.append(dict(name=module['name'],path=guest,interface=module['interface']))
        for number,symbol in enumerate(module['exports']):
            if symbol['name'] in definitions:continue
            if symbol['name'] in available:raise ValueError('ambiguous library symbol: '+symbol['name'])
            available[symbol['name']]=dict(dependency=index,symbol=number,kind=symbol['kind'],type=symbol['type'])
    for symbol in set(types)&set(available):
        expected=fmt.digest(types[symbol].encode()).hex()
        actual=available[symbol]['type']
        opaque=fmt.digest(b'c16-assembly-v1').hex()
        # Generated soft-float declarations use IR float types; their C
        # implementations take the equivalent integer bit carriers. Ripple
        # passes both through the same registers/slots (unlike a host C ABI).
        if symbol.startswith('__rcc_'):
            carrier=types[symbol].replace('f32','i32').replace('f64','i64')
            if fmt.digest(carrier.encode()).hex()==actual:continue
        if expected!=actual and actual!=opaque:
            # An incomplete external array declaration does not fix its length.
            if not types[symbol].startswith('array:0:'):
                raise ValueError('incompatible shared declaration: '+symbol)
    if name:
        first=next((n for n in selected if any(n in o['labels'] for o in objects)),None)
        if first is None:first=next(n for o in objects for n in o['labels'])
        objects[0]['entry_point']=first
        for obj in objects:
            if obj.get('rcc'):obj['rcc']['roots']=list(set(obj['rcc']['roots'])|set(selected))
    else:
        for obj in objects:
            for ref in obj['unresolved_references'].values():
                if ref['label']=='_init_globals':ref['label']='_rxe_init_all'
    with tempfile.TemporaryDirectory(prefix='riplix-module-') as folder:
        work=Path(folder);rewritten=[];patches=[]
        for index,obj in enumerate(objects):
            if obj.get('rcc') and not obj['rcc'].get('banked_data'):
                raise ValueError('shared modules require banked global addresses')
            obj,entries,_=rewrite(obj,index,definitions,available)
            rewritten.append(obj);patches+=entries
        # Defined placeholders let the ordinary linker resolve and relax all
        # calls. Loader relocations replace live imports with their real targets.
        stub=dict(version=1,instructions=[],data=[],labels={},data_labels={},
                  unresolved_references={},entry_point=None,rcc=None)
        for symbol,value in sorted(available.items()):
            if value['kind']:
                stub['labels'][symbol]=label(len(stub['instructions']))
                stub['instructions'].append(instruction(20,0,4,3))
            else:
                stub['data_labels'][symbol]=len(stub['data']);stub['data'].append(0)
        rewritten.append(stub)
        if not name:
            source=work/'initialize.asm';source.write_text(INITIALIZE)
            obj=work/'initialize.pobj'
            subprocess.run([str(assembler),'assemble',str(source),'-o',str(obj),'--bank-size','64000','--max-immediate','65535'],check=True,stdout=subprocess.DEVNULL)
            rewritten+=load_objects([obj])
        files=[]
        for index,obj in enumerate(rewritten):
            for key,value in obj['labels'].items():value['name']=key
            path=work/f'{index}.pobj';path.write_text(json.dumps(obj,separators=(',',':')));files.append(path)
        linked=work/'linked.rxe';symbols=work/'symbols.json'
        subprocess.run([str(linker),*map(str,files),'-f','rxe','--bank-size','64000','--map',str(symbols),'-o',str(linked)],check=True,stdout=subprocess.DEVNULL)
        locations=json.loads(symbols.read_text());code_map=locations['code'];data_map=locations['data']
        if map_output:
            Path(map_output).parent.mkdir(parents=True,exist_ok=True)
            Path(map_output).write_text(json.dumps(locations,indent=2)+'\n')
        binary=linked.read_bytes();entry,nins,cells,nreloc=struct.unpack_from('<4I',binary,8)
        code=bytearray(binary[24:24+nins*8]);data=binary[24+nins*8:24+nins*8+cells]
        live=[patch for patch in patches if patch[0] in code_map]
        keys=sorted({value[0] for _,kind,value in live if kind=='got'})
        imported=sorted({value for _,kind,value in live if kind in (4,5,6)}|{key[0] for key in keys})
        used=sorted({available[s]['dependency'] for s in imported})
        dep_map={old:new for new,old in enumerate(used)}
        imports=[dict(available[s],dependency=dep_map[available[s]['dependency']]) for s in imported]
        import_map={name:index for index,name in enumerate(imported)}
        got=[(1,import_map[symbol],addend) for symbol,addend in keys]
        relocations=[];imported_at={code_map[marker] for marker,kind,_ in live if kind in (4,5,6)}
        for i in range(nreloc):
            kind,_,index=struct.unpack_from('<HHI',binary,24+nins*8+cells+i*8)
            if index in imported_at:continue
            if kind==0:relocations.append((0 if code[index*8]==19 else 1,index,0))
            else:raise ValueError('unbanked data relocation in shared code')
        for marker,kind,value in live:
            index=code_map[marker]
            if kind=='data_delta':
                symbol,addend=expression(value);delta=(data_map[symbol]+addend)//fmt.BANK
                struct.pack_into('<H',code,index*8+6,delta)
            elif kind=='got':
                key,component=value;struct.pack_into('<H',code,index*8+6,keys.index(key)*2+component)
            else:
                relocations.append((kind,index,import_map[value] if kind in (4,5,6) else 0))
        symbols=[]
        for symbol in selected:
            function=symbol in code_map
            value=code_map[symbol] if function else data_map[symbol]
            symbols.append(dict(name=symbol,value=value,kind=int(function),type=fmt.digest(types[symbol].encode()).hex()))
        if name:entry=code_map.get('_init_globals',0xffffffff)
        fmt.write(Path(output),name=name,entry=entry,code=code,data=data,exports=symbols,
                  dependencies=[deps[i] for i in used],imports=imports,got=got,relocations=relocations)
    print(f'Built RXE2 {name or "executable"}: {output}',flush=True)
