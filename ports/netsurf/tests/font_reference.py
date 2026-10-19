"""Independent exact pixel reference for the checked-in 16px font strikes."""
from pathlib import Path
import struct
import unicodedata


def word_pixels(word,face=8):
    data=(Path(__file__).resolve().parents[1]/'fonts/web.nfa').read_bytes()
    n=struct.unpack_from('<H',data,12)[0]
    codes=struct.unpack_from('<'+'H'*n,data,20)
    glyphs=[];pen=0
    for ch in unicodedata.normalize('NFC',word):
        record=20+n*2+(face*2*n+codes.index(ord(ch)))*12
        offset,length,left,top,w,h,advance,_=struct.unpack_from('<IHbbBBBB',data,record)
        assert offset!=0xffffffff
        mask=[]
        for byte in data[offset:offset+length]:mask.extend([(byte&15)*17]*((byte>>4)+1))
        assert len(mask)==w*h
        glyphs.append((pen+left,top,w,h,mask));pen+=advance
    left=min(g[0] for g in glyphs);top=min(g[1] for g in glyphs)
    right=max(g[0]+g[2] for g in glyphs);bottom=max(g[1]+g[3] for g in glyphs)
    image=[[255 for _ in range(right-left)] for _ in range(bottom-top)]
    for x,y,w,h,mask in glyphs:
        for dy in range(h):
            for dx in range(w):image[y-top+dy][x-left+dx]=255-mask[dy*w+dx]
    def channel(value,maximum):
        q=(maximum*value+127)//255
        return q*255//maximum
    return [[(channel(v,31),channel(v,63),channel(v,31)) for v in row] for row in image]


def find_word(rgb,width,height,word,faces=(8,),x_range=None,y_range=None):
    """Locate a complete black word on white, allowing RGB565 expansion rounding."""
    for face in faces:
        pixels=word_pixels(word,face);h=len(pixels);w=len(pixels[0])
        # Only a few dark samples nominate candidates; compare the whole word
        # after that. This keeps large-frame searches inexpensive.
        dark=[(x,y) for y,row in enumerate(pixels) for x,c in enumerate(row) if max(c)<24]
        probes=dark[::max(1,len(dark)//5)][:5]
        def matches(x,y):
            if any(max(rgb[((y+dy)*width+x+dx)*3:((y+dy)*width+x+dx)*3+3])>32 for dx,dy in probes):return False
            return all(all(abs(rgb[((y+dy)*width+x+dx)*3+c]-expected[c])<=4 for c in range(3))
                       for dy,row in enumerate(pixels) for dx,expected in enumerate(row))
        for y in (y_range or range(64,height-h)):
            for x in (x_range or range(2,width-w)):
                if x+w<=width and y+h<=height and matches(x,y):return True
    return False
