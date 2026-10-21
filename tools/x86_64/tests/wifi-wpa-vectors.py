"""Reproduce PUBLIC WPA fixtures using Python hashlib/HMAC and OpenSSL AES wrap.
No user credentials/device access. Fixture header is consumed by native tests.
"""
from pathlib import Path
import ctypes,ctypes.util,hashlib,hmac,struct
HERE=Path(__file__).resolve().parent

def mac(key,data):return hmac.new(key,data,'sha1').digest()
def wrap(key,data):
    # OpenSSL enc refuses unstreamable modes; use its EVP wrap API directly.
    path=ctypes.util.find_library('crypto')
    for candidate in ['/opt/homebrew/opt/openssl@3/lib/libcrypto.dylib',path]:
        if candidate and (Path(candidate).exists() or candidate==path):
            lib=ctypes.CDLL(candidate);break
    else:raise RuntimeError('OpenSSL libcrypto is required to regenerate public fixtures')
    pointer=ctypes.c_void_p;integer=ctypes.c_int
    lib.EVP_CIPHER_CTX_new.restype=pointer
    lib.EVP_aes_128_wrap.restype=pointer
    lib.EVP_CIPHER_CTX_set_flags.argtypes=[pointer,integer]
    lib.EVP_EncryptInit_ex.argtypes=[pointer,pointer,pointer,pointer,pointer]
    lib.EVP_EncryptUpdate.argtypes=[pointer,pointer,pointer,pointer,integer]
    lib.EVP_CIPHER_CTX_free.argtypes=[pointer]
    ctx=lib.EVP_CIPHER_CTX_new();assert ctx
    try:
        lib.EVP_CIPHER_CTX_set_flags(ctx,1)  # EVP_CIPHER_CTX_FLAG_WRAP_ALLOW
        keybuf=ctypes.create_string_buffer(key);plain=ctypes.create_string_buffer(bytes(data))
        assert lib.EVP_EncryptInit_ex(ctx,lib.EVP_aes_128_wrap(),None,keybuf,None)==1
        out=ctypes.create_string_buffer(len(data)+16);size=integer()
        assert lib.EVP_EncryptUpdate(ctx,out,ctypes.byref(size),plain,len(data))==1
        assert size.value==len(data)+8
        return out.raw[:size.value]
    finally:lib.EVP_CIPHER_CTX_free(ctx)

def eapol(info,replay,nonce=b'\0'*32,keylen=0,data=b''):
    return bytearray(bytes([1,3])+struct.pack('>H',95+len(data))+bytes([2])+struct.pack('>HHQ',info,keylen,replay)+nonce+bytes(16+8+8+16)+struct.pack('>H',len(data))+data)
def sign(packet,ptk):packet[81:97]=mac(ptk[:16],packet)[:16];return packet

def generate():
    pmk=bytes(range(32));sn=bytes(range(32,64));an=bytes(range(64,96));own=bytes(range(2,8));ap=bytes(range(4,10))
    context=min(own,ap)+max(own,ap)+min(sn,an)+max(sn,an)
    ptk=b''.join(mac(pmk,b'Pairwise key expansion\0'+context+bytes([i])) for i in range(3))[:48]
    result=dict(pmk=pmk,snonce=sn,anonce=an,own=own,ap=ap,ptk=ptk)
    for group in [2,4]:
        rsn=bytes([48,20,1,0,0,15,172,group,1,0,0,15,172,4,1,0,0,15,172,2,0,0])
        gtk=bytes(range(100,132 if group==2 else 116));kde=bytes([221,6+len(gtk),0,15,172,1,1,0])+gtk
        data=rsn+kde
        if len(data)%8:data+=b'\xdd'+bytes(7-len(data)%8)
        m1=eapol(0x8a,7,an,keylen=16)
        m2=sign(eapol(0x10a,7,sn,data=rsn),ptk)
        m3=sign(eapol(0x13ca,8,an,16,wrap(ptk[16:32],data)),ptk)
        m4=sign(eapol(0x30a,8),ptk)
        for name,buf in [('rsn',rsn),('gtk',gtk),('m1',m1),('m2',m2),('m3',m3),('m4',m4)]:result[f'{name}_{group}']=buf
        changed=bytearray(data);changed[7]=4 if group==2 else 2
        result[f'bad_rsn_{group}']=sign(eapol(0x13ca,8,an,16,wrap(ptk[16:32],changed)),ptk)
        bad=bytearray(wrap(ptk[16:32],data));bad[5]^=1
        result[f'bad_wrap_{group}']=sign(eapol(0x13ca,8,an,16,bad),ptk)
    text='/* PUBLIC deterministic vectors: Python hashlib/HMAC + OpenSSL RFC3394. */\n'
    for name,data in result.items():text+=f'static const unsigned char vector_{name}[]={{'+','.join(str(v) for v in data)+'};\n'
    return text
if __name__=='__main__':(HERE/'fixtures/wifi-wpa-vectors.h').write_text(generate())
