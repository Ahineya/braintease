"""Versioned shared C16 libc/curses SDK, built from the existing port sources."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
from .link import pack

ROOT=Path(__file__).resolve().parents[4]


def prepare(build,compiler,assembler,linker,curses=False):
    build=Path(build).resolve();directory=build/'lib';directory.mkdir(parents=True,exist_ok=True)
    command=['make','-s','--no-print-directory','-C',str(ROOT/'ports/fuzix'),f'BUILD={build}',f'RCC={compiler}',
             f'RASM={assembler}',f'RLINK={linker}','RFL_CURSES=1','RFL_UNICODE=1']
    with (directory/'.sdk.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        subprocess.run([*command,'-j4','rfl-sdk','float-sdk'],check=True)
        info=json.loads(subprocess.check_output([*command,'rfl-sdk-info'],text=True))
        floats=json.loads(subprocess.check_output([*command,'float-sdk-info'],text=True))
        companions=[Path(p) for p in info['libraries'] if '/curses/' in p]
        core=[Path(p) for p in info['libraries'] if '/curses/' not in p and Path(p)!=build/'libc/format.pobj']
        core+=map(Path,floats['libraries'])
        unicode=build/'libc/unicode.pobj'
        if unicode not in core:core.append(unicode)
        entry=Path(info['entry'])
        def library(name,objects,dependencies=(),startup=None):
            output=directory/name;stamp=directory/(name+'.sha256');digest=hashlib.sha256()
            for p in [*objects,*([startup] if startup else []),*[Path(p) for p,_ in dependencies],linker,assembler,
                      *sorted(Path(__file__).parent.glob('*.py'))]:
                digest.update(str(p).encode());digest.update(p.read_bytes())
            key=digest.hexdigest()
            if not output.exists() or not stamp.exists() or stamp.read_text()!=key:
                pack(objects,output,linker=linker,assembler=assembler,name=name,dependencies=dependencies,startup=startup)
                stamp.write_text(key)
            return (output,'/lib/riplix/'+name)
        libc=library('libc.rsl.1',core,startup=entry)
        libraries=[libc]
        if curses:libraries.append(library('libcurses.rsl.1',companions,[libc]))
    return libraries,entry
