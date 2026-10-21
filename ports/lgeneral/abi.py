"""Keep LGeneral's original 32-bit integer model on the C16 SDK.

Only generated game sources change. SDK and SDL declarations retain their ABI.
The lexical pass does not rewrite comments or literal text. Formatted-output
arguments get explicit casts, including enum values and small constants.
"""
import re
from pathlib import Path

TOKEN = re.compile(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|\s+|.', re.S)
FORMAT = re.compile(r'%(?:%|([-+ #0]*)(\*|\d*)(\.(?:\*|\d*))?(hh|ll|[hljztL])?([diuoxXcspnaAeEfFgG]))')


def tokens(text):
    return TOKEN.findall(text)


def significant(t):
    return not t.isspace() and not t.startswith(('/*','//'))


def widen(text):
    ts=tokens(text); ids=[i for i,t in enumerate(ts) if significant(t)]
    for n,i in enumerate(ids):
        t=ts[i]; prev=ts[ids[n-1]] if n else ''; nxt=ts[ids[n+1]] if n+1<len(ids) else ''
        if t=='unsigned' and nxt not in ('char','short','long'):
            ts[i]='lg_uint'
            if nxt=='int':ts[ids[n+1]]=''
        elif t=='signed' and nxt not in ('char','short','long'):
            ts[i]='lg_int'
            if nxt=='int':ts[ids[n+1]]=''
        elif t=='int':
            ts[i]='' if prev in ('short','long','lg_uint','lg_int') else 'lg_int'
        elif t=='atoi':ts[i]='atol'
        elif t=='abs':ts[i]='labs'
    return ''.join(ts)


def split_args(ts,start,end):
    depth=0;begin=start;out=[]
    for i in range(start,end):
        if ts[i] in ('(','[','{'):depth+=1
        elif ts[i] in (')',']','}'):depth-=1
        elif ts[i]==',' and depth==0:out.append((begin,i));begin=i+1
    out.append((begin,end));return out


def output_formats(text):
    ts=tokens(text); replacements=[]
    functions={'printf':0,'fprintf':1,'sprintf':1,'snprintf':2,'abortf':0,'AI_DEBUG':1}
    i=0
    while i<len(ts):
        fn=ts[i]
        if fn not in functions:i+=1;continue
        j=i+1
        while j<len(ts) and not significant(ts[j]):j+=1
        if j==len(ts) or ts[j]!='(':i+=1;continue
        depth=1;k=j+1
        while k<len(ts) and depth:
            if ts[k]=='(':depth+=1
            elif ts[k]==')':depth-=1
            k+=1
        if depth:raise ValueError('Unclosed formatted call')
        args=split_args(ts,j+1,k-1);fi=functions[fn]
        if len(args)<=fi:i=k;continue
        a,b=args[fi];literals=[n for n in range(a,b) if ts[n].startswith('"')]
        if not literals:i=k;continue  # declaration / dynamic format (abortf forwards only strings)
        # Formats may concatenate marker macros. None of the marker macros has
        # conversions; parse each literal in order and retain the original text.
        casts=[]
        for n in literals:
            def convert(m):
                if m.group(0)=='%%':return '%%'
                flags,width,precision,length,conv=m.groups()
                if width=='*':casts.append('int')
                if precision=='.*':casts.append('int')
                if conv in 'diuoxX':
                    casts.append('unsigned long' if conv in 'uoxX' else 'long')
                    return '%'+flags+width+(precision or '')+'l'+conv
                if conv=='c':casts.append('int')
                else:casts.append(None)
                return m.group(0)
            ts[n]=FORMAT.sub(convert,ts[n])
        if len(args)-fi-1!=len(casts):
            raise ValueError(f'{fn}: format/argument mismatch: '+''.join(ts[j:k]))
        for (a,b),cast in zip(args[fi+1:],casts):
            if cast:replacements.append((a,b,f'({cast})('+''.join(ts[a:b]).strip()+')'))
        i=k
    for a,b,value in reversed(replacements):ts[a:b]=[value]
    return ''.join(ts)


def adapt(source:Path,c16:bool):
    (source/'lg-types.h').write_text('#ifndef LG_TYPES_H\n#define LG_TYPES_H\n#include <stdint.h>\ntypedef int32_t lg_int;\ntypedef uint32_t lg_uint;\n#endif\n')
    for folder in ('src','util'):
        for path in sorted((source/folder).glob('*')):
            if path.suffix not in ('.c','.h','.inc'):continue
            text=path.read_text(encoding='latin1')
            text=output_formats(widen(text))
            text=re.sub(r'__attribute(?:__)?\s*\(\(\s*packed\s*\)\)', '',text)
            # These callbacks are called by SDL / libc, whose int stays native.
            if path.name in ('event.c','event.h'):
                text=re.sub(r'lg_int (event_filter|all_filter)\s*\(',r'int \1(',text)
            if path.name=='file.c':text=re.sub(r'static lg_int file_compare\s*\(', 'static int file_compare(', text)
            if path.name=='main.c':
                text=text.replace('lg_int main(lg_int argc,', 'int main(int argc,')
            path.write_text('#include "lg-types.h"\n'+text,encoding='latin1')
