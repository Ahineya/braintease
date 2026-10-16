/* SPDX-License-Identifier: GPL-2.0-only */
/* RXE1 loader for Ripple's separate instruction and data memories. */
#include "platform.h"
#include <kdata.h>
#include <printf.h>
static uint8_t code_refs[8];
static int task_code[PTABSIZE] = {-1, -1, -1, -1};
static uint8_t stream[BLKSIZE];
static uint32_t stream_base;
static uint16_t stream_count;
static inoptr stream_inode;
extern void imem_add_word2(unsigned, unsigned, unsigned);
void ripple_code_free(ptptr p) {
    unsigned i = (unsigned)(p - ptab);
    if (task_code[i] >= 0) {
        --code_refs[task_code[i]];
        task_code[i] = -1;
    }
}
void ripple_code_fork(ptptr p) {
    int slot = task_code[(unsigned)(udata.u_ptab - ptab)];
    task_code[(unsigned)(p - ptab)] = slot;
    if (slot >= 0)
        ++code_refs[slot];
}
static void stream_reset(inoptr ino) {
    stream_inode = ino;
    stream_base = 0xffffffffUL;
    stream_count = 0;
}
static unsigned byteat(uint32_t pos) {
    uint32_t b = pos & ~(uint32_t)BLKMASK;
    if (b != stream_base) {
        udata.u_offset = b;
        udata.u_count = BLKSIZE;
        udata.u_base = stream;
        udata.u_sysio = 1;
        readi(stream_inode, 0);
        stream_base = b;
        stream_count = (uint16_t)udata.u_done;
    }
    if ((pos & BLKMASK) >= stream_count)
        panic("short exec read");
    return stream[(unsigned)pos & BLKMASK] & 255;
}
static uint16_t wordat(uint32_t p) { return byteat(p) | (byteat(p + 1) << 8); }
static uint32_t longat(uint32_t p) {
    uint32_t v = wordat(p);
    return v | ((uint32_t)wordat(p + 2) << 16);
}
arg_t _execve(void) {
    inoptr ino;
    struct s_argblk *abuf, *ebuf;
    uint32_t entry, nins, data, nr, pos, idx, i, expect;
    unsigned slot, base, bank, addr, kind, cells[4];
    uint8_t **av, **ev;
    int argc;
    ino = n_open_argn();
    if (!ino)
        return -1;
    if (getmode(ino) != MODE_R(F_REG) || !(getperm(ino) & OTH_EX) ||
        (fs_tab[ino->c_super].m_flags & MS_NOEXEC)) {
        udata.u_error = EACCES;
        goto bad;
    }
    if (ino->c_node.i_size < 24) {
        udata.u_error = ENOEXEC;
        goto bad;
    }
    stream_reset(ino);
    if (wordat(0) != 0x5852 || wordat(2) != 0x3145 || wordat(4) != 64000) {
        udata.u_error = ENOEXEC;
        goto bad;
    }
    entry = longat(8);
    nins = longat(12);
    data = longat(16);
    nr = longat(20);
    if (!nins || nins > 256000UL || entry >= nins || data > 60000UL || nr > nins * 2) {
        udata.u_error = ENOEXEC;
        goto bad;
    }
    expect = 24 + nins * 8 + data + nr * 8;
    if (expect != ino->c_node.i_size) {
        udata.u_error = ENOEXEC;
        goto bad;
    }
    /* Check every relocation before replacing any mutable user memory. */
    for (i = 0; i < nr; i++) {
        pos = 24 + nins * 8 + data + i * 8;
        kind = wordat(pos);
        idx = longat(pos + 4);
        if (kind > 1 || idx >= nins) {
            udata.u_error = ENOEXEC;
            goto bad;
        }
    }
    for (slot = 0; slot < 8 && code_refs[slot]; slot++)
        ;
    if (slot == 8) {
        udata.u_error = ENOMEM;
        goto bad;
    }
    abuf = tmpbuf();
    ebuf = tmpbuf();
    if (rargs((uint8_t **)udata.u_argn1, abuf) || rargs((uint8_t **)udata.u_argn2, ebuf)) {
        tmpfree(abuf);
        tmpfree(ebuf);
        goto bad;
    }
    base = 32 + slot * 4;
    /* Once STORC starts, read errors are fatal to this process. */
    stream_reset(ino);
    pos = 24;
    bank = base;
    addr = 0;
    for (i = 0; i < nins; i++) {
        cells[0] = byteat(pos);
        cells[1] = wordat(pos + 2);
        cells[2] = wordat(pos + 4);
        cells[3] = wordat(pos + 6);
        imem_store(bank, addr, cells);
        pos += 8;
        if (++addr == 64000) {
            addr = 0;
            bank++;
        }
    }
    for (i = 0; i < nr; i++) {
        pos = 24 + nins * 8 + data + i * 8;
        kind = wordat(pos);
        idx = longat(pos + 4);
        if (kind == 0)
            imem_add_word2(base + (unsigned)(idx / 64000UL), (unsigned)(idx % 64000UL), base);
    }
    uzero((void *)PROGBASE, 64000);
    for (i = 0; i < data; i++)
        dmem_store(USER_GP, (unsigned)i, byteat(24 + nins * 8 + i));
    av = wargs((uint8_t *)PROGTOP, abuf, &argc);
    ev = wargs((uint8_t *)av, ebuf, NULL);
    udata.u_top = PROGTOP;
    udata.u_ptab->p_top = PROGTOP;
    udata.u_break = PROGBASE + data;
    udata.u_codebase = PROGBASE;
    ripple_argc = argc;
    ripple_argv = (unsigned long)av;
    ripple_envp = (unsigned long)ev;
    for (i = 0; i < UFTSIZE; i++)
        if (udata.u_cloexec & (1U << i))
            doclose(i);
    udata.u_cloexec = 0;
    memset(udata.u_sigvec, 0, sizeof(udata.u_sigvec));
    udata.u_cursig = 0;
    udata.u_error = 0;
    if (argc)
        uget((void *)ugetp(av), udata.u_name, 8);
    memcpy(udata.u_ptab->p_name, udata.u_name, 8);
    tmpfree(abuf);
    tmpfree(ebuf);
    i_deref(ino);
    ripple_code_free(udata.u_ptab);
    code_refs[slot] = 1;
    task_code[(unsigned)(udata.u_ptab - ptab)] = slot;
    ripple_exec_bank = base + (unsigned)(entry / 64000UL);
    ripple_exec_pc = (unsigned)(entry % 64000UL);
    __hard_di();
    udata.u_insys = 0;
    udata.u_ptab->p_status = P_RUNNING;
    ripple_enter_user();
    panic("exec returned");
bad:
    i_deref(ino);
    return -1;
}
void doexec(uaddr_t a) {
    used(a);
    panic("use RXE exec");
}
