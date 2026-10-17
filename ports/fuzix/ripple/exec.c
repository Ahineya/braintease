/* SPDX-License-Identifier: GPL-2.0-only */
/* RXE1 loader for Ripple's separate instruction and data memories. */
#include "platform.h"
#include "graphics.h"
#include <kdata.h>
#include <printf.h>
#include <vm_bulk.h>
/* One spare code slot lets exec replace an image with every task occupied. */
#define CODE_SLOTS (PTABSIZE + 1)
static uint8_t code_refs[CODE_SLOTS];
static uint32_t code_size[CODE_SLOTS];
static unsigned code_base[CODE_SLOTS], code_banks[CODE_SLOTS];
static unsigned code_limit;
static int task_code[PTABSIZE];
void ripple_code_init(void) {
    unsigned i, status;
    unsigned long ram, capacity;
    irqflags_t flags;
    code_limit = RIPPLE_CODE_BANKS;
    flags = di();
    status = vm_memory_info(&ram, &capacity);
    irqrestore(flags);
    if (status == VM_DEVICE_OK) {
        capacity /= BANK_CELLS;
        if (capacity < code_limit) code_limit = (unsigned)capacity;
    } else if (code_limit > 256U) {
        /* Older hosts cannot report their cap. Retain a conservative pool. */
        code_limit = 256U;
    }
    if (code_limit < RIPPLE_CODE_FIRST) code_limit = RIPPLE_CODE_FIRST;
    for (i = 0; i < PTABSIZE; i++) task_code[i] = -1;
}
/* First-fit contiguous extents allocate physical instruction storage.
 * Metadata slots track images, not fixed-size regions. Reserve before loading
 * so a sleeping exec cannot have its destination claimed by another task. */
static int code_reserve(uint32_t count) {
    unsigned slot, i, base = RIPPLE_CODE_FIRST;
    unsigned banks = (unsigned)((count + BANK_CELLS - 1UL) / BANK_CELLS);
    for (slot = 0; slot < CODE_SLOTS && code_refs[slot]; slot++);
    if (slot == CODE_SLOTS) return -1;
    while (base <= code_limit && banks <= code_limit - base) {
        for (i = 0; i < CODE_SLOTS; i++) {
            if (code_refs[i] && base < code_base[i] + code_banks[i] &&
                code_base[i] < base + banks) break;
        }
        if (i == CODE_SLOTS) {
            code_refs[slot] = 1;
            code_base[slot] = base;
            code_banks[slot] = banks;
            code_size[slot] = count;
            return slot;
        }
        base = code_base[i] + code_banks[i];
    }
    return -1;
}
static void code_drop(unsigned slot) {
    if (!--code_refs[slot]) {
        code_size[slot] = 0;
        code_base[slot] = code_banks[slot] = 0;
    }
}
static uint8_t stream[BLKSIZE];
static uint32_t stream_base;
static uint16_t stream_count;
static inoptr stream_inode;
int ripple_valid_code(unsigned long p) {
    int slot = task_code[(unsigned)(udata.u_ptab - ptab)];
    unsigned bank = p >> 16, off = (unsigned)p;
    if (slot < 0 || off >= BANK_CELLS)
        return 0;
    return bank < code_banks[slot] &&
           (unsigned long)bank * BANK_CELLS + off < code_size[slot];
}
void ripple_code_free(ptptr p) {
    unsigned i = (unsigned)(p - ptab);
    if (task_code[i] >= 0) {
        code_drop(task_code[i]);
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
    unsigned base, bank, addr, kind, chunk_off, cells[4];
    int slot = -1;
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
    if (!nins || nins > (unsigned long)(RIPPLE_CODE_BANKS - RIPPLE_CODE_FIRST) * BANK_CELLS ||
        entry >= nins || data >= 16UL * BANK_CELLS) {
        udata.u_error = ENOEXEC;
        goto bad;
    }
    /* Subtract validated sections rather than overflowing RXE length sums. */
    expect = ino->c_node.i_size - 24;
    if (nins > expect / 8) {
        udata.u_error = ENOEXEC;
        goto bad;
    }
    expect -= nins * 8;
    if (data > expect || (expect - data) % 8 || nr != (expect - data) / 8) {
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
    slot = code_reserve(nins);
    if (slot < 0) {
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
    base = code_base[slot];
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
    if (ripple_mmu_exec_data((unsigned)(udata.u_ptab-ptab), data)) {
        tmpfree(abuf);
        tmpfree(ebuf);
        udata.u_error = ENOMEM;
        goto bad;
    }
    /* The new map consists of zeroed private pages. Data uploads below resolve
     * each virtual destination to a validated physical DMA span. */
    i = 0;
    while (i < data) {
        pos = 24 + nins * 8 + i;
        if (fast_data) {
            unsigned n, status, dest_bank, dest_off;
            unsigned run;
            unsigned long ptr;
            irqflags_t flags;
            (void)byteat(pos);
            chunk_off = (unsigned)pos & BLKMASK;
            n = stream_count - chunk_off;
            if ((unsigned long)n > data - i) n = (unsigned)(data - i);
            if (!n) panic("short exec data read");
            if(ripple_mmu_span(((USER_GP + i / BANK_CELLS) << 16) | (i % BANK_CELLS),1,
                               &dest_bank,&dest_off,&run))panic("exec destination");
            if(n>run)n=run;
            ptr = (unsigned long)(stream + chunk_off);
            flags = di();
            status = vm_bulk_copy((unsigned)(ptr >> 16), (unsigned)ptr,
                dest_bank, dest_off, n, VM_BULK_OCTETS);
            irqrestore(flags);
            if (status == VM_DEVICE_OK) { i += n; continue; }
            if (status != VM_DEVICE_UNKNOWN_COMMAND && status != VM_DEVICE_UNSUPPORTED)
                panic("exec data transfer");
            fast_data = 0;
        }
        dmem_store(USER_GP + (unsigned)(i / BANK_CELLS), (unsigned)(i % BANK_CELLS), byteat(pos));
        i++;
    }
    av = wargs((uint8_t *)PROGTOP, abuf, &argc);
    ev = wargs((uint8_t *)av, ebuf, NULL);
    udata.u_top = PROGTOP;
    udata.u_ptab->p_top = PROGTOP;
    udata.u_break = ((USER_GP + data / BANK_CELLS) << 16) | (data % BANK_CELLS);
    udata.u_codebase = PROGBASE;
    ripple_argc = argc;
    ripple_argv = (unsigned long)av;
    ripple_envp = (unsigned long)ev;
    ripple_gfx_exit(udata.u_ptab);
    for (i = 0; i < UFTSIZE; i++)
        if (udata.u_cloexec & (1UL << i))
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
    tmpfree(abuf);
    tmpfree(ebuf);
    i_deref(ino);
    ripple_code_free(udata.u_ptab);
    task_code[(unsigned)(udata.u_ptab - ptab)] = slot;
    ripple_mmu_code((unsigned)(udata.u_ptab-ptab),base,nins);
    ripple_exec_bank = (unsigned)(entry / 64000UL);
    ripple_exec_pc = (unsigned)(entry % 64000UL);
    __hard_di();
    udata.u_insys = 0;
    udata.u_ptab->p_status = P_RUNNING;
    ripple_enter_user();
    panic("exec returned");
bad:
    if (slot >= 0) code_drop((unsigned)slot);
    i_deref(ino);
    return -1;
}
void doexec(uaddr_t a) {
    used(a);
    panic("use RXE exec");
}
