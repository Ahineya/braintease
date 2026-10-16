//! Flat RAM for ordinary programs, copy-on-write pages for hosted processes.
//! The kernel arena and its allocator are shared explicitly, outside private
//! process pages. This is a trusted address-space model, not a privilege boundary.
use super::{
    memory::{allocate_from, release_from},
    BANK, CELLS,
};
use std::{
    collections::BTreeMap,
    ops::Range,
    sync::{Arc, Mutex},
};

pub(super) const PAGE_CELLS: usize = 512;
pub(super) const KERNEL_START: usize = 240 * BANK;
type Page = [u16; PAGE_CELLS];

pub(super) struct Kernel {
    cells: Vec<u16>,
    free: BTreeMap<usize, usize>,
    allocated: BTreeMap<usize, usize>,
}

#[derive(Clone)]
pub(super) enum Memory {
    Flat(Vec<u16>),
    Paged {
        pages: Vec<Arc<Page>>,
        kernel: Arc<Mutex<Kernel>>,
    },
}
impl Default for Memory {
    fn default() -> Self {
        Self::Flat(Vec::new())
    }
}
impl Memory {
    pub fn new() -> Self {
        Self::Flat(vec![0; CELLS])
    }
    pub fn enable_processes(&mut self) {
        if matches!(self, Self::Paged { .. }) {
            return;
        }
        let Self::Flat(flat) = std::mem::take(self) else {
            unreachable!()
        };
        assert_eq!(flat.len(), CELLS);
        let zero = Arc::new([0; PAGE_CELLS]);
        let pages = flat[..KERNEL_START]
            .chunks_exact(PAGE_CELLS)
            .map(|cells| {
                if cells.iter().all(|v| *v == 0) {
                    zero.clone()
                } else {
                    Arc::new(cells.try_into().unwrap())
                }
            })
            .collect();
        let kernel = Kernel {
            cells: flat[KERNEL_START..].to_vec(),
            free: BTreeMap::from([(KERNEL_START + 1, CELLS - KERNEL_START - 1)]),
            allocated: BTreeMap::new(),
        };
        *self = Self::Paged {
            pages,
            kernel: Arc::new(Mutex::new(kernel)),
        };
    }
    pub fn len(&self) -> usize {
        match self {
            Self::Flat(cells) => cells.len(),
            Self::Paged { .. } => CELLS,
        }
    }
    pub fn read(&self, index: usize) -> u16 {
        match self {
            Self::Flat(cells) => cells[index],
            Self::Paged { pages, kernel } => {
                if index < KERNEL_START {
                    pages[index / PAGE_CELLS][index % PAGE_CELLS]
                } else {
                    kernel.lock().unwrap().cells[index - KERNEL_START]
                }
            }
        }
    }
    pub fn write(&mut self, index: usize, value: u16) {
        match self {
            Self::Flat(cells) => cells[index] = value,
            Self::Paged { pages, kernel } => {
                if index < KERNEL_START {
                    let page = &mut pages[index / PAGE_CELLS];
                    if page[index % PAGE_CELLS] != value {
                        Arc::make_mut(page)[index % PAGE_CELLS] = value;
                    }
                } else {
                    kernel.lock().unwrap().cells[index - KERNEL_START] = value;
                }
            }
        }
    }
    pub fn cells(
        &self,
        range: Range<usize>,
    ) -> impl DoubleEndedIterator<Item = u16> + ExactSizeIterator + '_ {
        assert!(range.start <= range.end && range.end <= self.len());
        range.map(|at| self.read(at))
    }
    pub fn read_into(&self, mut start: usize, mut dest: &mut [u16]) {
        assert!(start <= self.len() && dest.len() <= self.len() - start);
        match self {
            Self::Flat(cells) => dest.copy_from_slice(&cells[start..start + dest.len()]),
            Self::Paged { pages, kernel } => {
                while start < KERNEL_START && !dest.is_empty() {
                    let offset = start % PAGE_CELLS;
                    let count = dest.len().min(PAGE_CELLS - offset);
                    dest[..count]
                        .copy_from_slice(&pages[start / PAGE_CELLS][offset..offset + count]);
                    start += count;
                    dest = &mut dest[count..];
                }
                if !dest.is_empty() {
                    let start = start - KERNEL_START;
                    dest.copy_from_slice(&kernel.lock().unwrap().cells[start..start + dest.len()]);
                }
            }
        }
    }
    pub fn write_from(&mut self, mut start: usize, mut values: &[u16]) {
        assert!(start <= self.len() && values.len() <= self.len() - start);
        match self {
            Self::Flat(cells) => cells[start..start + values.len()].copy_from_slice(values),
            Self::Paged { pages, kernel } => {
                while start < KERNEL_START && !values.is_empty() {
                    let offset = start % PAGE_CELLS;
                    let count = values.len().min(PAGE_CELLS - offset);
                    let page = &mut pages[start / PAGE_CELLS];
                    if page[offset..offset + count] != values[..count] {
                        Arc::make_mut(page)[offset..offset + count]
                            .copy_from_slice(&values[..count]);
                    }
                    start += count;
                    values = &values[count..];
                }
                if !values.is_empty() {
                    let start = start - KERNEL_START;
                    kernel.lock().unwrap().cells[start..start + values.len()]
                        .copy_from_slice(values);
                }
            }
        }
    }
    pub fn fill(&mut self, range: Range<usize>, value: u16) {
        assert!(range.start <= range.end && range.end <= self.len());
        if let Self::Flat(cells) = self {
            cells[range].fill(value);
            return;
        }
        let values = [value; PAGE_CELLS];
        let mut start = range.start;
        while start < range.end {
            let count = (range.end - start).min(PAGE_CELLS);
            self.write_from(start, &values[..count]);
            start += count;
        }
    }
    pub fn copy_within(&mut self, from: Range<usize>, dest: usize) {
        assert!(from.start <= from.end && from.end <= self.len());
        assert!(dest <= self.len() && from.len() <= self.len() - dest);
        if let Self::Flat(cells) = self {
            cells.copy_within(from, dest);
            return;
        }
        let mut scratch = [0; PAGE_CELLS];
        let mut remaining = from.len();
        while remaining != 0 {
            let count = remaining.min(PAGE_CELLS);
            let offset = if dest > from.start {
                remaining - count
            } else {
                from.len() - remaining
            };
            self.read_into(from.start + offset, &mut scratch[..count]);
            self.write_from(dest + offset, &scratch[..count]);
            remaining -= count;
        }
    }
    pub fn kernel_allocate(&mut self, cells: u64) -> u64 {
        let Self::Paged { kernel, .. } = self else {
            unreachable!("kernel allocation requires process memory")
        };
        let mut kernel = kernel.lock().unwrap();
        let Kernel {
            free, allocated, ..
        } = &mut *kernel;
        allocate_from(free, allocated, cells)
    }
    pub fn kernel_release(&mut self, ptr: u64) {
        let Self::Paged { kernel, .. } = self else {
            unreachable!("kernel free requires process memory")
        };
        let mut kernel = kernel.lock().unwrap();
        let Kernel {
            free, allocated, ..
        } = &mut *kernel;
        release_from(free, allocated, ptr);
    }
}

impl rvm::devices::GuestMemory for Memory {
    fn len(&self) -> usize {
        self.len()
    }
    fn read(&self, index: usize) -> u16 {
        self.read(index)
    }
    fn write(&mut self, index: usize, value: u16) {
        self.write(index, value);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn shared(a: &Memory, b: &Memory, index: usize) -> bool {
        let (Memory::Paged { pages: a, .. }, Memory::Paged { pages: b, .. }) = (a, b) else {
            panic!()
        };
        Arc::ptr_eq(&a[index / PAGE_CELLS], &b[index / PAGE_CELLS])
    }
    #[test]
    fn fork_shares_pages_and_only_writes_detach_touched_private_pages() {
        let mut parent = Memory::new();
        parent.write(BANK + 1, 42);
        parent.enable_processes();
        let mut child = parent.clone();
        let (Memory::Paged { pages: a, .. }, Memory::Paged { pages: b, .. }) = (&parent, &child)
        else {
            panic!()
        };
        assert_eq!(a.len(), 30_000);
        assert!(a.iter().zip(b).all(|(a, b)| Arc::ptr_eq(a, b)));
        assert!(shared(&parent, &child, BANK + 1));
        child.write(BANK + 1, 43);
        assert_eq!(parent.read(BANK + 1), 42);
        assert!(!shared(&parent, &child, BANK + 1));
        assert!(shared(&parent, &child, BANK + PAGE_CELLS));
        child.write(BANK + PAGE_CELLS, 0); // A no-op does not break sharing.
        assert!(shared(&parent, &child, BANK + PAGE_CELLS));
        parent.write(BANK + PAGE_CELLS, 11);
        assert_eq!(child.read(BANK + PAGE_CELLS), 0);
    }
    #[test]
    fn shared_kernel_data_and_allocator_survive_switches_and_forks() {
        let mut parent = Memory::new();
        parent.enable_processes();
        let a = parent.kernel_allocate(20);
        let at = super::super::memory::linear(a);
        let mut child = parent.clone();
        child.write(at, 42);
        assert_eq!(parent.read(at), 42);
        let b = child.kernel_allocate(20);
        assert_ne!(a, b);
        parent.kernel_release(a);
        assert_eq!(child.kernel_allocate(20), a);
        drop(parent);
        assert_eq!(child.read(at), 42);
    }
    #[test]
    fn kernel_allocator_exhaustion_and_private_heap_do_not_overlap() {
        let mut rt = super::super::Runtime::new();
        rt.enable_process_memory();
        let private = rt.allocate((KERNEL_START - (5 * BANK + 1)) as u64);
        assert_ne!(private, 0);
        assert_eq!(rt.allocate(1), 0);
        let kernel = rt.memory.kernel_allocate((CELLS - KERNEL_START - 1) as u64);
        assert_ne!(kernel, 0);
        assert_eq!(rt.memory.kernel_allocate(1), 0);
        rt.memory.kernel_release(kernel);
        assert_ne!(rt.memory.kernel_allocate(100), 0);
        rt.release(private);
        assert_ne!(rt.allocate(100), 0);
    }
    #[test]
    fn page_crossings_overlapping_moves_and_shared_boundary_match_flat_ram() {
        let mut paged = Memory::new();
        paged.enable_processes();
        let mut flat = Memory::new();
        for start in [PAGE_CELLS - 3, BANK - 3, KERNEL_START - 3] {
            let data: Vec<_> = (0..1300).map(|i| i as u16).collect();
            for memory in [&mut flat, &mut paged] {
                memory.write_from(start, &data);
                memory.copy_within(start..start + 1200, start + 1);
                memory.copy_within(start + 3..start + 1200, start);
                memory.fill(start + 400..start + 600, 0xabcd);
            }
            assert!(flat
                .cells(start..start + 1300)
                .eq(paged.cells(start..start + 1300)));
        }
    }
}
