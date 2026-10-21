#!/usr/bin/env python3
"""Create verified C16 working copies of wolfSSH, retaining original sources."""
import hashlib
import importlib.util
import json
import re
from pathlib import Path

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]


def prepare(destination):
    spec = importlib.util.spec_from_file_location('ssh_wolfcrypt_adapt', ROOT / 'ports/tls/adapt.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    destination = Path(destination)
    manifest = json.loads((PORT / 'source.json').read_text())['files']
    counts = {}
    for name, expected in manifest.items():
        data = (PORT / 'upstream' / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise RuntimeError(f'Pinned wolfSSH source changed: {name}')
        text = data.decode()
        if name.endswith(('.c', '.h')):
            # Both libraries export min when NO_INLINE is selected. Give the
            # SSH helper its own name without changing evaluation semantics.
            for function in ('min', 'ato32', 'c32toa', 'ConstantCompare'):
                text = re.sub(r'\b'+function+r'\b', 'riplix_ssh_'+function, text)
            text, count = helper.octet_casts(text)
            if count:
                counts[name] = count
        if name == 'wolfssh/port.h':
            # C99 variadics can forward the complete argument list. Avoid the
            # optional-comma GNU token-pasting extension, including zero extras.
            for macro, function in (('WSNPRINTF', 'snprintf'), ('WVSNPRINTF', 'vsnprintf')):
                old = f'#define {macro}(s,n,f,...) {function}((s),(n),(f),##__VA_ARGS__)'
                assert text.count(old) == 2
                text = text.replace(old, f'#define {macro}(...) {function}(__VA_ARGS__)')
            old='#define WTRUNCATE(fs,f,sz)   truncate((f),(off_t)(sz))'
            assert text.count(old)==1
            text=text.replace(old,'#define WTRUNCATE(fs,f,sz) riplix_ssh_truncate((f),(sz))')
            text += '''
extern int riplix_ssh_truncate(const char *, word64);
extern int32_t riplix_ssh_pread(int, byte *, word32, const word32 *);
extern int32_t riplix_ssh_pwrite(int, byte *, word32, const word32 *);
extern int32_t riplix_ssh_append(int, byte *, word32);
extern int riplix_ssh_settime(const char *, word32, word32);
extern int riplix_ssh_size_string(char *, unsigned, word32, word32);
#undef WPREAD
#undef WPWRITE
#undef WWRITE
#define WPREAD(fs,f,b,s,o) riplix_ssh_pread((f),(b),(s),(o))
#define WPWRITE(fs,f,b,s,o) riplix_ssh_pwrite((f),(b),(s),(o))
#define WWRITE(fs,f,b,s) riplix_ssh_append((f),(b),(s))
#undef WSETTIME
#define WSETTIME(fs,f,a,m) riplix_ssh_settime((f),(a),(m))
/* Riplix has no fchown syscall wrapper; report unsupported via upstream. */
#undef WFCHOWN
'''
        if name in ('wolfssh/port.h','src/port.c'):
            # SFTP offsets consist of two protocol uint32s, not two C ints.
            text=text.replace('const unsigned int* shortOffset','const word32* shortOffset')
        if name == 'wolfssh/wolfsftp.h':
            old='#define WOLFSSH_HANDLE_ID_SZ (sizeof(word32) * 2)'
            assert text.count(old)==1
            text=text.replace(old,'#define WOLFSSH_HANDLE_ID_SZ 8')
        if name in ('src/wolfsftp.c','wolfssh/wolfsftp.h'):
            # The SFTP implementation uses int-sized protocol lengths and
            # request IDs, including casts to word32*. Give that module the
            # original C32 arithmetic explicitly; libc/SSH core retain C16.
            text=re.sub(r'\blong long int\b','long long',text)
            text=re.sub(r'\bunsigned int\b','word32',text)
            text=re.sub(r'\bint\b','int32_t',text)
            text=text.replace('%10u','%10lu')
        if name == 'src/wolfsftp.c':
            # Preserve NO_SUCH_FILE separately from permission/protocol errors
            # so recursive uploads may create missing files without following links.
            begin=text.index('static int32_t SFTP_STAT(')
            end=text.index('\n}',begin)+2
            region=text[begin:end]
            old='                        return WS_SFTP_STATUS_NOT_OK;'
            assert region.count(old)==1
            region=region.replace(old, '                        ssh->error = ret == WOLFSSH_FTP_NOFILE ? WS_SFTP_FILE_DNE : WS_SFTP_STATUS_NOT_OK;\n'+old)
            text=text[:begin]+region+text[end:]

            # SFTP permits a short DATA response. OpenSSH requests 32 KiB by
            # default; cap the read rather than refusing a valid READ request.
            old='if (strSz > WOLFSSH_MAX_SFTP_RW) {\n            return WS_BUFFER_E;\n        }'
            assert text.count(old)==2
            text=text.replace(old,'if (strSz > WOLFSSH_MAX_SFTP_RW) {\n            strSz = WOLFSSH_MAX_SFTP_RW;\n        }')
            # SendPacket returns WANT_WRITE directly without setting error.
            # The SFTP caller retries according to ssh->error, so preserve the
            # reason rather than clearing the pending response as a fatal error.
            old='ret = wolfSSH_SendPacket(ssh);'
            assert text.count(old)==2
            text=text.replace(old,old+'\n        if (ret < 0) ssh->error = ret;')
            old='''WSNPRINTF(sizeStr, sizeof(sizeStr) - 1, "%8lld",
            ((long long)atr->sz[1] << 32) + (long long)(atr->sz[0]));'''
            assert text.count(old)==1
            text=text.replace(old,'''if (riplix_ssh_size_string(sizeStr, sizeof(sizeStr), atr->sz[0], atr->sz[1]) < 0)
        return WS_BUFFER_E;''')
            old='''WSNPRINTF(name->lName, totalSz + 1, "%s   ? %10lu %10lu %s %s%s",
            perm, atr->uid, atr->gid, sizeStr, date, name->fName);'''
            assert text.count(old)==1
            text=text.replace(old,'''{
        int32_t written = WSNPRINTF(name->lName, totalSz + 1, "%s   ? %10lu %10lu %s %s%s",
            perm, atr->uid, atr->gid, sizeStr, date, name->fName);
        if (written < 0 || written > totalSz) {
            WFREE(name->lName, name->heap, DYNTYPE_SFTP);
            name->lName = NULL;
            name->lSz = 0;
            return WS_BUFFER_E;
        }
        name->lSz = written;
    }''')
            old='ret = wolfSSH_stream_send(ssh, buffer->data + buffer->idx, sendSz);'
            assert text.count(old)==1
            text=text.replace(old,'''{
            word32 octet;
            for (octet=0; octet<sendSz; octet++) if (buffer->data[buffer->idx+octet]>255) {
                fprintf(stderr, "sshd: non-octet SFTP reply at %lu\\n", buffer->idx+octet);
                ssh->error=WS_BUFFER_E;
                return WS_BUFFER_E;
            }
        }
        '''+old)
            old='''ret = wolfSSH_SFTP_buffer_send(ssh, &state->buffer);
                if (ret < 0) {'''
            assert text.count(old)==5
            text=text.replace(old,old+'\n                    if (ssh->error == 0) ssh->error = ret;',1)
        output = destination / name
        output.parent.mkdir(parents=True, exist_ok=True)
        helper.write_changed(output, text)
    helper.write_changed(destination / 'ripple-adaptations.json',
                         json.dumps(dict(octet_casts=counts, changes=[
                             'Namespace shared NO_INLINE helpers',
                             'Forward snprintf/vsnprintf arguments using standard C99 macros',
                             'Use checked C16 filesystem, offset and timestamp wrappers',
                             'Represent SFTP handles as eight protocol octets',
                             'Preserve C32 SFTP length/request arithmetic using explicit int32_t/word32',
                             'Return bounded short SFTP READ responses to larger client requests',
                             'Preserve direct SSH flush errors for nonblocking SFTP retries',
                             'Format full 64-bit file sizes without %lld and check directory string lengths',
                             'Reject non-octet SFTP responses before encryption',
                         ]), indent=2) + '\n')
    return destination


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    print(prepare(parser.parse_args().destination))
