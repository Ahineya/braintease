"""RXE2 module envelope. All file integers are little-endian host octets."""
import hashlib
import json
import struct

BANK = 64000
HEADER = 128
DEPENDENCY = 160
EXPORT = 104
IMPORT = 40
GOT = 12
RELOC = 16
MAX_MODULES = 32
MAX_DEPENDENCIES = 8
DESCRIPTORS = 128
GOT_START = 1024


def digest(value):
    return hashlib.sha256(value).digest()


def interface(exports):
    return digest(json.dumps([(s['name'], s['kind'], s['type']) for s in exports],
                             separators=(',', ':'), ensure_ascii=False).encode())


def text(value, size):
    data = value.encode('ascii')
    if len(data) >= size or b'\0' in data:
        raise ValueError(f'module string exceeds {size-1} characters: {value}')
    return data.ljust(size, b'\0')


def read(path):
    data = path.read_bytes()
    if len(data) < HEADER or data[:4] != b'RXE2':
        raise ValueError(f'not an RXE2 module: {path}')
    bank, flags, entry, count, cells, relocations, metadata = struct.unpack_from('<HHIIIII', data, 4)
    deps, exports, got, imports = struct.unpack_from('<4H', data, 28)
    initialized, = struct.unpack_from('<I', data, 100)
    minimum = HEADER+deps*DEPENDENCY+exports*EXPORT+imports*IMPORT+got*GOT
    if (bank != BANK or flags > 1 or deps > MAX_DEPENDENCIES or exports > 1024 or
            got > 16000 or imports > 8192 or minimum > metadata or metadata % 256 or
            initialized > cells or len(data) != metadata+count*8+initialized+relocations*RELOC):
        raise ValueError(f'invalid RXE2 layout: {path}')
    result = dict(flags=flags, entry=entry, count=count, cells=cells, relocations=relocations,
                  metadata=metadata, initialized=initialized, name=data[104:128].split(b'\0')[0].decode(),
                  interface=data[36:68], identity=data[68:100], dependencies=[], exports=[], imports=[], got=[])
    cursor = HEADER
    for _ in range(deps):
        result['dependencies'].append(dict(name=data[cursor:cursor+28].split(b'\0')[0].decode(),
            path=data[cursor+28:cursor+124].split(b'\0')[0].decode(), interface=data[cursor+124:cursor+156]))
        cursor += DEPENDENCY
    for _ in range(exports):
        name = data[cursor:cursor+64].split(b'\0')[0].decode()
        value, kind = struct.unpack_from('<IH', data, cursor+64)
        result['exports'].append(dict(name=name, value=value, kind=kind, type=data[cursor+72:cursor+104].hex()))
        cursor += EXPORT
    for _ in range(imports):
        dependency, symbol, kind = struct.unpack_from('<3H', data, cursor)
        result['imports'].append(dict(dependency=dependency, symbol=symbol, kind=kind,
                                      type=data[cursor+8:cursor+40].hex()))
        cursor += IMPORT
    for _ in range(got):
        kind, _, value, addend = struct.unpack_from('<HHIi', data, cursor)
        result['got'].append((kind, value, addend));cursor += GOT
    if interface(result['exports']) != result['interface']:
        raise ValueError(f'invalid module interface: {path}')
    return result


def write(path, *, name, entry, code, data, exports, dependencies, imports, got, relocations):
    if len(dependencies)>MAX_DEPENDENCIES or len(exports)>1024 or len(got)>16000 or len(imports)>8192:
        raise ValueError('module metadata exceeds loader limits')
    metadata = bytearray(HEADER)
    for dep in dependencies:
        metadata += text(dep['name'],28)+text(dep['path'],96)+dep['interface']+bytes(4)
    for symbol in exports:
        metadata += text(symbol['name'],64)+struct.pack('<IHH',symbol['value'],symbol['kind'],0)+bytes.fromhex(symbol['type'])
    for symbol in imports:
        metadata += struct.pack('<4H',symbol['dependency'],symbol['symbol'],symbol['kind'],0)+bytes.fromhex(symbol['type'])
    for kind,value,addend in got:
        metadata += struct.pack('<HHIi',kind,0,value,addend)
    metadata += bytes((-len(metadata))%256)
    initialized = data.rstrip(b'\0')
    metadata[:4] = b'RXE2'
    struct.pack_into('<HHIIIII4H',metadata,4,BANK,int(bool(name)),entry,len(code)//8,len(data),len(relocations),len(metadata),
                     len(dependencies),len(exports),len(got),len(imports))
    metadata[36:68] = interface(exports)
    struct.pack_into('<I',metadata,100,len(initialized))
    metadata[104:128] = text(name,24)
    tail = code+initialized+b''.join(struct.pack('<HHIII',kind,0,index,target,0) for kind,index,target in relocations)
    metadata[68:100] = digest(metadata+tail)
    path.parent.mkdir(parents=True,exist_ok=True)
    temporary=path.with_name('.'+path.name+'.new')
    temporary.write_bytes(metadata+tail);temporary.replace(path)
