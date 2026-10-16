/* SPDX-License-Identifier: GPL-2.0-only */
/* RXE1 loader for Ripple's separate instruction and data memories. */
#include "platform.h"
#include <kdata.h>
#include <printf.h>
#include <vm_bulk.h>
/* One spare code slot lets exec replace an image with every task occupied. */
#define CODE_SLOTS (PTABSIZE + 1)
/* Keep the BF code reservation in ports/fuzix/Makefile in sync. */
#define USER_CODE_BANKS 12
static uint8_t code_refs[CODE_SLOTS];
static uint32_t code_size[CODE_SLOTS];
static int task_code[PTABSIZE];
void ripple_code_init(void) {
    unsigned i;
    for (i = 0; i < PTABSIZE; i++) task_code[i] = -1;
}
static uint8_t stream[BLKSIZE];
static uint32_t stream_base;
static uint16_t stream_count;
static inoptr stream_inode;
extern void imem_add_word2(unsigned, unsigned, unsigned);
int ripple_valid_code(unsigned long p) {
    int slot = task_code[(unsigned)(udata.u_ptab - ptab)];
    unsigned bank = p >> 16, off = (unsigned)p;
    unsigned base;
    if (slot < 0 || off >= BANK_CELLS)
        return 0;
#if RIPPLE_MMU
    base = 0;
#else
    base = 32 + slot * USER_CODE_BANKS;
#endif
    return bank >= base && bank < base + USER_CODE_BANKS &&
           (unsigned long)(bank - base) * BANK_CELLS + off < code_size[slot];
}
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
    unsigned slot, base, bank, addr, kind, chunk_off, cells[4];
    unsigned fast_code = RIPPLE_BULK_IO, fast_data = RIPPLE_BULK_IO;
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
    if (!nins || nins > (unsigned long)USER_CODE_BANKS * BANK_CELLS || entry >= nins || data > 60000UL ||
        nr > nins * 2) {
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
    for (slot = 0; slot < CODE_SLOTS && code_refs[slot]; slot++)
        ;
    if (slot == CODE_SLOTS) {
        udata.u_error = ENOMEM;
        goto bad;
    }
    abuf = tmpbuf();
    ebuf = tmpbuf();
    if (rargs((uint8_t **)udata.u_argn1, abuf) ||
        rargs((uint8_t **)udata.u_argn2, ebuf)) {
        tmpfree(abuf);
        tmpfree(ebuf);
        goto bad;
    }
    base = 32 + slot * USER_CODE_BANKS;
    /* Once STORC starts, read errors are fatal to this process. */
    stream_reset(ino);
    pos = 24;
    bank = base;
    addr = 0;
    /* RXE1 instructions are eight octets and start at aligned offset 24.
     * Decode directly from each cached filesystem block instead of making
     * eight byte-reader calls per instruction in large images such as AWK. */
    chunk_off = 24;
    (void)byteat(pos);
    i = 0;
    while (i < nins) {
        if (chunk_off == BLKSIZE) {
            chunk_off = 0;
            (void)byteat(pos);
        }
        if (chunk_off + 8 > stream_count) panic("short exec instruction read");
        if (fast_code) {
            unsigned n = (stream_count - chunk_off) / 8;
            unsigned status;
            unsigned long ptr = (unsigned long)(stream + chunk_off);
            irqflags_t flags;
            if (n > VM_BULK_MAX_INSTRUCTIONS) n = VM_BULK_MAX_INSTRUCTIONS;
            if ((unsigned long)n > nins - i) n = (unsigned)(nins - i);
            flags = di();
            status = vm_bulk_copy((unsigned)(ptr >> 16), (unsigned)ptr,
                bank, addr, n, VM_BULK_CODE_OCTETS);
            irqrestore(flags);
            if (status == VM_DEVICE_OK) {
                chunk_off += n * 8; pos += (unsigned long)n * 8; i += n;
                if (n >= BANK_CELLS - addr) { bank++; addr = n - (BANK_CELLS - addr); }
                else addr += n;
                continue;
            }
            if (status != VM_DEVICE_UNKNOWN_COMMAND && status != VM_DEVICE_UNSUPPORTED)
                panic("exec code transfer");
            fast_code = 0;
        }
        cells[0] = stream[chunk_off] & 255;
        cells[1] = (stream[chunk_off + 2] & 255) | ((stream[chunk_off + 3] & 255) << 8);
        cells[2] = (stream[chunk_off + 4] & 255) | ((stream[chunk_off + 5] & 255) << 8);
        cells[3] = (stream[chunk_off + 6] & 255) | ((stream[chunk_off + 7] & 255) << 8);
        chunk_off += 8;
        imem_store(bank, addr, cells);
        i++;
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
        if (kind == 0 && !RIPPLE_MMU)
            imem_add_word2(base + (unsigned)(idx / 64000UL),
                           (unsigned)(idx % 64000UL), base);
    }
#if RIPPLE_MMU
    if (ripple_mmu_exec((unsigned)(udata.u_ptab-ptab))) {
        tmpfree(abuf);
        tmpfree(ebuf);
        udata.u_error = ENOMEM;
        goto bad;
    }
    fast_data = 0; /* Device descriptors contain physical addresses. */
#endif
    /* Clear and populate data in block-sized transfers as well. */
    i = 0;
    while (fast_data && i < 64000UL) {
        unsigned n = (unsigned)(64000UL - i), status;
        irqflags_t flags;
        if (n > VM_BULK_MAX_CELLS) n = VM_BULK_MAX_CELLS;
        flags = di();
        status = vm_bulk_fill(USER_GP, (unsigned)i, n, 0);
        irqrestore(flags);
        if (status == VM_DEVICE_OK) { i += n; continue; }
        if (status != VM_DEVICE_UNKNOWN_COMMAND && status != VM_DEVICE_UNSUPPORTED)
            panic("exec data clear");
        fast_data = 0;
    }
    if (!fast_data) uzero((void *)PROGBASE, 64000);
    i = 0;
    while (i < data) {
        pos = 24 + nins * 8 + i;
        if (fast_data) {
            unsigned n, status;
            unsigned long ptr;
            irqflags_t flags;
            (void)byteat(pos);
            chunk_off = (unsigned)pos & BLKMASK;
            n = stream_count - chunk_off;
            if ((unsigned long)n > data - i) n = (unsigned)(data - i);
            if (!n) panic("short exec data read");
            ptr = (unsigned long)(stream + chunk_off);
            flags = di();
            status = vm_bulk_copy((unsigned)(ptr >> 16), (unsigned)ptr,
                USER_GP, (unsigned)i, n, VM_BULK_OCTETS);
            irqrestore(flags);
            if (status == VM_DEVICE_OK) { i += n; continue; }
            if (status != VM_DEVICE_UNKNOWN_COMMAND && status != VM_DEVICE_UNSUPPORTED)
                panic("exec data transfer");
            fast_data = 0;
        }
        dmem_store(USER_GP, (unsigned)i, byteat(pos));
        i++;
    }
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
    udata.u_sigtramp = 0;
    udata.u_cursig = 0;
    udata.u_error = 0;
    memset(udata.u_name, 0, sizeof(udata.u_name));
    if (argc) {
        uint8_t *name = (uint8_t *)ugetp(av);
        /* Short argv[0] can end at the very top of the GP bank. */
        for (i = 0; i < sizeof(udata.u_name); i++) {
            int c = ugetc(name + i);
            if (c < 0)
                break;
            udata.u_name[i] = c;
            if (!c)
                break;
        }
    }
    udata.u_ptab->p_flags |= PFL_EXECED;
    memcpy(udata.u_ptab->p_name, udata.u_name, sizeof(udata.u_name));
    udata.u_ptab->p_size = 250;
    tmpfree(abuf);
    tmpfree(ebuf);
    i_deref(ino);
    ripple_code_free(udata.u_ptab);
    code_refs[slot] = 1;
    code_size[slot] = nins;
    task_code[(unsigned)(udata.u_ptab - ptab)] = slot;
#if RIPPLE_MMU
    ripple_mmu_code((unsigned)(udata.u_ptab-ptab),base,nins);
    ripple_exec_bank = (unsigned)(entry / 64000UL);
#else
    ripple_exec_bank = base + (unsigned)(entry / 64000UL);
#endif
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
