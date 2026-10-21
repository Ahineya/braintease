/* Polled-console US keyboard decoder. This is an independent terminal adapter,
 * not copied Linux/OpenBSD driver code. Protocol/key-map review references:
 * Linux v6.12 drivers/input/keyboard/atkbd.c, atkbd_receive_byte(),
 * https://github.com/torvalds/linux/blob/v6.12/drivers/input/keyboard/atkbd.c
 * OpenBSD pckbd.c rev 1.51 / wskbdmap_mfii.c rev 1.48, pinned at
 * dfb3b89c3c8f7be544280afe44a669f9e0012e27, sys/dev/pckbc/:
 * https://github.com/openbsd/src/blob/dfb3b89c3c8f7be544280afe44a669f9e0012e27/sys/dev/pckbc/pckbd.c
 * E0 keys have separate identities; E1 Pause bytes must not become Ctrl/Num.
 * E0 fake-shift bytes around PrintScreen must not alter real Shift state.
 * Terminal sequences match shared ripple/curses/input.c and mux/input.inc:
 * CSI navigation/tilde keys, SS3 F1-F4, CSI tilde F5-F12. Modified sequences
 * use xterm's 1+Shift+2*Alt+4*Ctrl parameter; curses currently discards it.
 * US layout only, no firmware hotkey/Fn, media, USB or I2C implementation.
 * Locks are software state; keyboard LED commands are deliberately separate.
 */
#include "keyboard.h"

enum { LSHIFT=1, RSHIFT=2, LCTRL=4, RCTRL=8, LALT=16, RALT=32 };
static const uint8_t normal[128] = {
    [0x01]=27,[0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',
    [0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0a]='9',[0x0b]='0',
    [0x0c]='-',[0x0d]='=',[0x0e]=127,[0x0f]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
    [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1a]='[',[0x1b]=']',[0x1c]='\r',
    [0x1e]='a',[0x1f]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
    [0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',[0x2b]='\\',
    [0x2c]='z',[0x2d]='x',[0x2e]='c',[0x2f]='v',[0x30]='b',[0x31]='n',
    [0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',[0x37]='*',[0x39]=' ',
    [0x4a]='-',[0x4e]='+'
};

void x64_keyboard_resync(X64Keyboard *k) {
    k->modifiers = k->locks_down = k->extended = k->pause = 0;
}
void x64_keyboard_init(X64Keyboard *k) {
    *k = (X64Keyboard){0};
}
int x64_keyboard_get(X64Keyboard *k) {
    if (!k->count) return -1;
    int result = k->output[k->head];
    k->head = (k->head + 1) % X64_KEYBOARD_QUEUE_CAPACITY;
    --k->count;
    return result;
}
static void queue(X64Keyboard *k, const uint8_t *octets, unsigned count) {
    if (count > X64_KEYBOARD_QUEUE_CAPACITY - k->count) {
        if (k->dropped != UINT64_MAX) ++k->dropped;
        return;
    }
    for (unsigned i=0; i<count; ++i)
        k->output[(k->head + k->count + i) % X64_KEYBOARD_QUEUE_CAPACITY] = octets[i];
    k->count += count;
}
static unsigned modifier(const X64Keyboard *k) {
    return 1 + !!(k->modifiers & (LSHIFT|RSHIFT)) +
        2 * !!(k->modifiers & (LALT|RALT)) + 4 * !!(k->modifiers & (LCTRL|RCTRL));
}
static void special(X64Keyboard *k, unsigned number, uint8_t last, int ss3) {
    uint8_t out[8]; unsigned used=0, mod=modifier(k);
    out[used++]=27; out[used++]=ss3 && mod==1 ? 'O' : '[';
    if (number) {
        if (number>=10) out[used++]='0'+number/10;
        out[used++]='0'+number%10;
    } else if (mod!=1) out[used++]='1';
    if (mod!=1) { out[used++]=';'; out[used++]='0'+mod; }
    out[used++]=last; queue(k,out,used);
}
static int navigation(X64Keyboard *k, unsigned key) {
    switch (key) {
    case 0x47: special(k,0,'H',0); break;
    case 0x48: special(k,0,'A',0); break;
    case 0x49: special(k,5,'~',0); break;
    case 0x4b: special(k,0,'D',0); break;
    case 0x4d: special(k,0,'C',0); break;
    case 0x4f: special(k,0,'F',0); break;
    case 0x50: special(k,0,'B',0); break;
    case 0x51: special(k,6,'~',0); break;
    case 0x52: special(k,2,'~',0); break;
    case 0x53: special(k,3,'~',0); break;
    default: return 0;
    }
    return 1;
}
static void character(X64Keyboard *k, unsigned c, unsigned key, int keypad) {
    unsigned shift=!!(k->modifiers&(LSHIFT|RSHIFT));
    if (!keypad) {
        if (c>='a' && c<='z') { if (shift!=!!k->caps) c-='a'-'A'; }
        else if (shift) {
            static const char punctuation[]="1234567890-=[];'`\\,./";
            static const char shifted[]=    "!@#$%^&*()_+{}:\"~|<>?";
            for (unsigned i=0; i<sizeof(punctuation)-1; ++i)
                if (c==(unsigned)punctuation[i]) { c=shifted[i]; break; }
        }
    }
    if (k->modifiers&(LCTRL|RCTRL)) {
        if ((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='['&&c<='_')) c&=31;
        else if (c==' ' || c=='@' || c=='2') c=0;
        else if (c=='6') c=30;
        else if (c=='-' || c=='/') c=31;
        else if (c=='?') c=127;
        else if (key==0x0e) c=8;
    }
    uint8_t out[2]; unsigned used=0;
    if (k->modifiers&(LALT|RALT)) out[used++]=27;
    out[used++]=(uint8_t)c; queue(k,out,used);
}
void x64_keyboard_feed(X64Keyboard *k, uint8_t scan) {
    /* Pause is E1 1D 45 E1 9D C5 in translated set 1. On malformed/truncated
     * input, discard the partial token and reconsider the mismatching byte. */
    if (k->pause) {
        static const uint8_t tail[]={0x1d,0x45,0xe1,0x9d,0xc5};
        if (scan==tail[k->pause-1]) {
            if (++k->pause==sizeof(tail)+1) k->pause=0;
            return;
        }
        k->pause=0;
    }
    if (scan==0xe1) { k->extended=0; k->pause=1; return; }
    if (scan==0xe0) { k->extended=1; return; }
    if (!scan || scan==0xff) { x64_keyboard_resync(k); return; }
    if (scan==0xfa || scan==0xfe || scan==0xfc || scan==0xfd || scan==0xf0) {
        k->extended=0; return; /* Unexpected protocol response, not a key. */
    }
    unsigned extended=k->extended, released=scan&0x80, key=scan&0x7f, bit=0;
    k->extended=0;
    if (key==0x1d) bit=extended?RCTRL:LCTRL;
    else if (key==0x38) bit=extended?RALT:LALT;
    else if (!extended && key==0x2a) bit=LSHIFT;
    else if (!extended && key==0x36) bit=RSHIFT;
    if (bit) {
        if (released) k->modifiers&=~bit; else k->modifiers|=bit;
        return;
    }
    if (!extended && (key==0x3a || key==0x45)) {
        bit=key==0x3a?1:2;
        if (released) k->locks_down&=~bit;
        else if (!(k->locks_down&bit)) {
            k->locks_down|=bit;
            if (key==0x3a) k->caps^=1; else k->num^=1;
        }
        return;
    }
    if (released) return;
    if (extended) {
        if (navigation(k,key)) return;
        if (key==0x1c) character(k,'\r',key,1);
        else if (key==0x35) character(k,'/',key,1);
        /* E0 fake shifts, PrintScreen, Windows/Menu and media keys are not
         * terminal characters. They never alter unprefixed modifier state. */
        return;
    }
    if (key>=0x3b && key<=0x3e) { special(k,0,'P'+key-0x3b,1); return; }
    if ((key>=0x3f && key<=0x44) || key==0x57 || key==0x58) {
        static const uint8_t numbers[]={15,17,18,19,20,21,23,24};
        unsigned at=key<=0x44?key-0x3f:key-0x57+6;
        special(k,numbers[at],'~',0); return;
    }
    if (key==0x0f && (k->modifiers&(LSHIFT|RSHIFT))) {
        /* The conventional Shift-Tab spelling is CSI Z, without ;2. */
        if (modifier(k)==2) { static const uint8_t backtab[]={27,'[','Z'}; queue(k,backtab,3); }
        else special(k,0,'Z',0);
        return;
    }
    if (key>=0x47 && key<=0x53 && key!=0x4a && key!=0x4e) {
        if (!!k->num != !!(k->modifiers&(LSHIFT|RSHIFT))) {
            static const uint8_t digits[]={'7','8','9',0,'4','5','6',0,'1','2','3','0','.'};
            character(k,digits[key-0x47],key,1);
        } else if (key==0x4c) special(k,0,'E',0);
        else (void)navigation(k,key);
        return;
    }
    if (normal[key]) character(k,normal[key],key,key==0x37||key==0x4a||key==0x4e);
}
