"""Relocate RCC objects through the ordinary branch-relaxing Ripple linker.

Only global-address materialization changes. Calls retain the ordinary C16 ABI;
the module loader resolves code imports once, before publishing shared text.
"""
import copy
import re

LI, ADD, ADDI, LOAD, GP = 14, 1, 10, 17, 31


def instruction(op, a=0, b=0, c=0):
    return dict(opcode=op,word0=op,word1=a,word2=b,word3=c)


def expression(value):
    match=re.fullmatch(r'(.+)\+(-?\d+)',value)
    return (match[1],int(match[2])) if match else (value,0)


def label(index):
    return dict(name="",absolute_address=index*4,bank=0,offset=0)


def rewrite(original, tag, definitions, imports):
    obj=copy.deepcopy(original);old=obj['instructions'];refs={int(k):v for k,v in obj['unresolved_references'].items()}
    out=[];newrefs={};mapping={};patches=[];got=[]
    def mark(kind,target=0):
        name=f'L_rxm_patch_{tag}_{len(patches)}'
        obj['labels'][name]=label(len(out));patches.append((name,kind,target))
    i=0
    while i<len(old):
        mapping[i]=len(out)
        ref=refs.get(i)
        if ref and ref['ref_type'] in ('data_offset','data_bank'):
            symbol,addend=expression(ref['label']);reg=old[i]['word1']
            component=int(ref['ref_type']=='data_bank')
            if old[i]['opcode']!=LI:raise ValueError('global projection is not LI')
            if component:
                following=old[i+1] if i+1<len(old) else {}
                if not (following.get('opcode')==ADD and following['word1']==reg and
                        following['word2']==reg and following['word3']==GP):
                    raise ValueError('global bank projection lacks its GP addition')
            if symbol in definitions:
                if component:
                    mark(2);out.append(instruction(LI,reg))
                    out.append(instruction(LOAD,reg,GP,reg))
                    mark('data_delta',ref['label']);out.append(instruction(ADDI,reg,reg))
                else:
                    newrefs[len(out)]=ref;out.append(copy.deepcopy(old[i]))
            else:
                if symbol not in imports or imports[symbol]['kind']!=0:
                    raise ValueError(f'unresolved shared data symbol: {symbol}')
                key=(symbol,addend)
                if key not in got:got.append(key)
                mark(3);out.append(instruction(LI,reg))
                out.append(instruction(LOAD,reg,GP,reg))
                mark('got',(key,component));out.append(instruction(ADDI,reg,reg))
                out.append(instruction(LOAD,reg,GP,reg))
            if component:
                mapping[i+1]=mapping[i];i+=2
            else:i+=1
            continue
        if ref:
            newrefs[len(out)]=ref
            if ref['label'] in imports and imports[ref['label']]['kind']==1:
                kind={'absolute':4,'code_bank':5,'code_offset':6}.get(ref['ref_type'])
                if kind is None:raise ValueError('unsupported imported function reference')
                mark(kind,ref['label'])
        out.append(copy.deepcopy(old[i]));i+=1
    mapping[len(old)]=len(out)
    # Newly inserted markers already refer to expanded code; original labels do not.
    for name,entry in original['labels'].items():obj['labels'][name]=label(mapping[entry['absolute_address']//4])
    # Re-symbolize local branches/jumps so the normal linker handles widening.
    for index,inst in enumerate(old):
        if index in refs or inst['opcode'] not in (19,21,22,23,24):continue
        if inst['opcode']==19:
            if inst['word2']!=0:continue
            destination=inst['word3']
        else:
            delta=inst['word3'];delta=delta-65536 if delta>=32768 else delta
            destination=index+delta
        if destination not in mapping:raise ValueError('object branch outside its instruction section')
        name=f'L_rxm_target_{tag}_{destination}'
        obj['labels'][name]=label(mapping[destination])
        newrefs[mapping[index]]=dict(label=name,ref_type='absolute' if inst['opcode']==19 else 'branch')
    obj['instructions']=out;obj['unresolved_references']={str(k):v for k,v in newrefs.items()}
    return obj,patches,got


def slice_code(original,start,end):
    obj=copy.deepcopy(original);obj['instructions']=obj['instructions'][start:end]
    obj['labels']={name:label(value['absolute_address']//4-start) for name,value in obj['labels'].items()
                   if start<=value['absolute_address']//4<end}
    obj['unresolved_references']={str(int(k)-start):v for k,v in obj['unresolved_references'].items() if start<=int(k)<end}
    for index,inst in enumerate(obj['instructions']):
        if inst['opcode']==19 and str(index) not in obj['unresolved_references'] and inst['word2']==0:
            if not start<=inst['word3']<end:raise ValueError('startup slice has a cross-section jump')
            inst['word3']-=start
    obj['entry_point']='_start' if '_start' in obj['labels'] else None
    return obj
