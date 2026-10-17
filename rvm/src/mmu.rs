//! Optional CPU address translation. Tables are physical RAM, never MMIO.
//! Supervisor accesses are physical except for an explicitly selected user
//! window. User accesses always translate and can never target physical bank 0.
pub const PAGE_CELLS: usize = 512;
pub const PRESENT: u16 = 1;
pub const READ: u16 = 2;
pub const WRITE: u16 = 4;
pub const EXECUTE: u16 = 8;
pub const USER: u16 = 16;
pub const PAGED: u16 = 32;
/// Software-owned PTE marker. Hardware still faults because WRITE is clear.
pub const COW: u16 = 64;
const FLAGS: u16 = PRESENT | READ | WRITE | EXECUTE | USER | PAGED | COW;
pub const CAUSE_SYSCALL: u16 = 1;
pub const CAUSE_READ: u16 = 2;
pub const CAUSE_WRITE: u16 = 3;
pub const CAUSE_EXECUTE: u16 = 4;
pub const CAUSE_PRIVILEGE: u16 = 5;
pub const CAUSE_ILLEGAL: u16 = 6;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Access {
    Read,
    Write,
    Execute,
    CodeRead,
}
impl Access {
    fn flag(self) -> u16 {
        match self {
            Self::Write => WRITE,
            Self::Execute => EXECUTE,
            _ => READ,
        }
    }
    pub fn cause(self) -> u16 {
        match self {
            Self::Write => CAUSE_WRITE,
            Self::Execute => CAUSE_EXECUTE,
            _ => CAUSE_READ,
        }
    }
    fn code(self) -> bool {
        matches!(self, Self::Execute | Self::CodeRead)
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fault {
    pub cause: u16,
    pub bank: u16,
    pub offset: u16,
}
#[derive(Clone, Copy, Debug)]
struct Translation {
    physical: usize,
    flags: u16,
}
pub(crate) const TLB_SIZE: usize = 128;
/// Stable layout shared with generated checked user-memory accesses. A miss
/// goes through translate(), which validates PTEs and fills one cache entry.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct TlbEntry {
    pub tag: u32,
    pub generation: u32,
    pub physical: usize,
    pub flags: u32,
    pub span: u32,
}
#[derive(Debug)]
pub(crate) struct Tlb(pub [TlbEntry; TLB_SIZE]);
impl Default for Tlb {
    fn default() -> Self {
        Self([TlbEntry::default(); TLB_SIZE])
    }
}
#[derive(Debug, Default)]
pub struct Mmu {
    pub enabled: bool,
    pub user: bool,
    pub window: bool,
    pub window_lo: u16,
    pub window_hi: u16,
    pub data_root: (u16, u16),
    pub code_root: (u16, u16),
    pub vector: (u16, u16),
    pub cause: u16,
    pub address: (u16, u16),
    pub(crate) tlb: Tlb,
    pub(crate) tlb_generation: u32,
}
impl Mmu {
    pub fn flush(&mut self) {
        self.tlb_generation = self.tlb_generation.wrapping_add(1);
        if self.tlb_generation == 0 {
            self.tlb.0.fill(TlbEntry::default());
        }
    }
    pub fn mapped(&self, bank: u16, access: Access) -> bool {
        self.enabled
            && (self.user
                || (!access.code()
                    && self.window
                    && bank >= self.window_lo
                    && bank <= self.window_hi))
    }
    fn table(
        memory: &[u16],
        size: u16,
        pointer: (u16, u16),
        index: usize,
        words: usize,
    ) -> Option<&[u16]> {
        if pointer.0 == 0 || pointer.1 >= size {
            return None;
        }
        let base = usize::from(pointer.0) * usize::from(size) + usize::from(pointer.1);
        memory.get(base.checked_add(index)?..base.checked_add(index)?.checked_add(words)?)
    }
    fn walk(
        &self,
        memory: &[u16],
        size: u16,
        bank: u16,
        offset: u16,
        code: bool,
    ) -> Option<Translation> {
        let root = if code { self.code_root } else { self.data_root };
        let directory = Self::table(memory, size, root, usize::from(bank >> 8) * 2, 2)?;
        let leaf = Self::table(
            memory,
            size,
            (directory[0], directory[1]),
            usize::from(bank & 255) * 4,
            4,
        )?;
        let mut flags = leaf[2];
        if leaf[3] != 0 || flags & !FLAGS != 0 || flags & PRESENT == 0 {
            return None;
        }
        let (target_bank, target_offset) = if flags & PAGED != 0 {
            let page = Self::table(
                memory,
                size,
                (leaf[0], leaf[1]),
                usize::from(offset) / PAGE_CELLS * 4,
                4,
            )?;
            if page[3] != 0
                || page[2] & !FLAGS != 0
                || page[2] & (PRESENT | PAGED) != PRESENT
                || usize::from(page[1]) % PAGE_CELLS != 0
            {
                return None;
            }
            flags &= page[2];
            (page[0], usize::from(page[1]))
        } else {
            if leaf[1] != 0 {
                return None;
            }
            (leaf[0], usize::from(offset) / PAGE_CELLS * PAGE_CELLS)
        };
        if target_offset >= usize::from(size) {
            return None;
        }
        // MMIO, legacy framebuffer routing and privileged DMA have no user alias.
        if target_bank == 0 {
            return None;
        }
        Some(Translation {
            physical: usize::from(target_bank) * usize::from(size) + target_offset,
            flags,
        })
    }
    fn cache_key(bank: u16, offset: u16, code: bool) -> (usize, u32) {
        let page = usize::from(offset) / PAGE_CELLS;
        (
            (usize::from(bank) ^ page ^ if code { 64 } else { 0 }) & (TLB_SIZE - 1),
            ((u32::from(bank) << 8) | ((page as u32) << 1) | u32::from(code)) + 1,
        )
    }
    fn cached(&self, bank: u16, offset: u16, code: bool) -> Option<TlbEntry> {
        let (index, tag) = Self::cache_key(bank, offset, code);
        let entry = self.tlb.0[index];
        (entry.tag == tag && entry.generation == self.tlb_generation).then_some(entry)
    }
    pub fn peek(
        &self,
        memory: &[u16],
        size: u16,
        bank: u16,
        offset: u16,
        access: Access,
        limit: usize,
    ) -> Option<usize> {
        if !self.mapped(bank, access) {
            return (usize::from(bank) * usize::from(size))
                .checked_add(usize::from(offset))
                .filter(|&i| i < limit);
        }
        if size == 0 || offset >= size {
            return None;
        }
        let t = self
            .cached(bank, offset, access.code())
            .map(|e| Translation {
                physical: e.physical,
                flags: e.flags as u16,
            })
            .or_else(|| self.walk(memory, size, bank, offset, access.code()))?;
        if t.flags & access.flag() == 0 || (self.user && t.flags & USER == 0) {
            return None;
        }
        let within = usize::from(offset) % PAGE_CELLS;
        (t.physical % usize::from(size) + within < usize::from(size))
            .then_some(t.physical + within)
            .filter(|&i| i < limit)
    }
    pub fn translate(
        &mut self,
        memory: &[u16],
        size: u16,
        bank: u16,
        offset: u16,
        access: Access,
        limit: usize,
    ) -> Result<usize, Fault> {
        if !self.mapped(bank, access) {
            return Ok(usize::from(bank) * usize::from(size) + usize::from(offset));
        }
        let fail = Fault {
            cause: access.cause(),
            bank,
            offset,
        };
        if size == 0 || offset >= size {
            return Err(fail);
        }
        let t = if let Some(e) = self.cached(bank, offset, access.code()) {
            Translation {
                physical: e.physical,
                flags: e.flags as u16,
            }
        } else {
            let t = self
                .walk(memory, size, bank, offset, access.code())
                .ok_or(fail)?;
            let (index, tag) = Self::cache_key(bank, offset, access.code());
            self.tlb.0[index] = TlbEntry {
                tag,
                generation: self.tlb_generation,
                physical: t.physical,
                flags: u32::from(t.flags),
                span: PAGE_CELLS.min(usize::from(size) - t.physical % usize::from(size)) as u32,
            };
            t
        };
        if t.flags & access.flag() == 0 || (self.user && t.flags & USER == 0) {
            return Err(fail);
        }
        let within = usize::from(offset) % PAGE_CELLS;
        let address = t.physical + within;
        if address >= limit || t.physical % usize::from(size) + within >= usize::from(size) {
            return Err(fail);
        }
        Ok(address)
    }
    /// Upper virtual bound for JIT tables, bounded by the configured code budget.
    /// Sparse mappings above that bound continue through reference execution.
    pub fn code_limit(&self, memory: &[u16], size: u16, capacity: usize) -> usize {
        if size == 0 {
            return 0;
        }
        let banks = capacity.div_ceil(usize::from(size)).min(65536);
        for high in (0..banks.div_ceil(256)).rev() {
            let Some(dir) = Self::table(memory, size, self.code_root, high * 2, 2) else {
                continue;
            };
            let Some(leaf) = Self::table(memory, size, (dir[0], dir[1]), 0, 1024) else {
                continue;
            };
            for low in (0..256.min(banks - high * 256)).rev() {
                if leaf[low * 4 + 2] & (PRESENT | EXECUTE | USER) == PRESENT | EXECUTE | USER {
                    return ((high * 256 + low + 1) * usize::from(size)).min(capacity);
                }
            }
        }
        0
    }
}
