#!/usr/bin/env python3
"""JIT graphics/resource checks on private disks; never update the user's disk."""
import argparse,json,shutil,struct,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/fuzix'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
a=p.parse_args();build=a.build.resolve()
assert (build/'.mmu').read_text().strip()=='1','graphics requires MMU=1'
assert (build/'gfxcheck.rxe').exists(),'build the opt-in gfxcheck.rxe target first'
def run(image,commands,*extra):
    cmd=[str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),'--memory','16384000','--jit','--headless','--input',commands,'--timeout','180',*extra]
    r=subprocess.run(cmd,capture_output=True,text=True,timeout=190)
    assert r.returncode==0,r.stdout+r.stderr
    assert 'FAILED' not in r.stdout and 'panic:' not in r.stdout,r.stdout+r.stderr
    return r.stdout
with tempfile.TemporaryDirectory(prefix='fuzix-gfx-') as temp:
    temp=Path(temp);stage=temp/'build';stage.mkdir()
    for file in build.iterdir():
        if file.is_file() and file.name!='apps.json':(stage/file.name).symlink_to(file)
    # Desktop assets are a directory, so file-only staging omitted them.
    if (build/'fusede').is_dir():(stage/'fusede').symlink_to(build/'fusede',target_is_directory=True)
    names=json.loads((build/'apps.json').read_text());names.append('gfxcheck')
    (stage/'apps.json').write_text(json.dumps(names))
    image=temp/'test.img'
    subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(stage),str(image)],check=True)
    frame_image=temp/'frame.img';shutil.copyfile(image,frame_image)
    out=run(image,"gfxcheck\necho GX_STATUS:$?\ngterm sh -c 'echo Terminal_alive; echo graphical_pty > /gfx-note'\necho GTERM_STATUS:$?\ncat /gfx-note\nshutdown\n")
    for expected in ['GX_STATUS:0','GTERM_STATUS:0','graphical_pty','disk synced; power off.']:
        assert expected in out,(expected,out)
    out=run(image,"gterm --geometry 64x20 -- sh -c 'stty -a > /gterm-size'\necho SMALL_STATUS:$?\ncat /gterm-size\ngterm -g 0x20 true\necho ZERO_STATUS:$?\ngterm -g 999999999999x20 true\necho HUGE_STATUS:$?\ngterm -g 64x31 true\necho TALL_STATUS:$?\ngterm -g 40x12 false\necho CHILD_STATUS:$?\nshutdown\n")
    for expected in ['SMALL_STATUS:0','20 rows 64 columns','320 ypixels 512 xpixels','ZERO_STATUS:2','HUGE_STATUS:2','TALL_STATUS:2','CHILD_STATUS:1','disk synced; power off.']:
        assert expected in out,(expected,out)
    subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
    out=run(image,'cat /gfx-note\nshutdown\n');assert 'graphical_pty' in out,out
    subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
    raw=temp/'frame.rgb565'
    run(frame_image,'gfxcheck frame\n','--raw-frame',str(raw),'--capture-frame','1')
    pixels=[0x001f]*(640*480)
    for x in range(640):
        pixels[99*640+x]=pixels[100*640+x]=0x8000+x
        pixels[101*640+x]=0x07e0+x
    pixels[101*640+3:102*640]=pixels[101*640:102*640-3]
    for y in range(470,480):pixels[y*640+620:(y+1)*640]=[0xf800]*20
    expected=struct.pack('<'+'H'*len(pixels),*pixels)
    actual=raw.read_bytes()
    assert actual==expected,('RGB565 mismatch',next((i//2 for i,(x,y) in enumerate(zip(actual,expected)) if x!=y),None),len(actual))
print('Graphics JIT passed: uploads/overlap/fill, ownership/cleanup, FD31 CLOEXEC, sixteen PTYs, configurable terminal/PTY geometry, invalid-size rejection, child status and persistent restart')
