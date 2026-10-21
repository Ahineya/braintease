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
// Preserve kernel globals and old private heaps. The extension stays below the
// signed 32-bit pointer boundary; high banks remain shared supervisor storage.
pub(super) const PRIVATE_START: usize = 256 * BANK;
pub(super) const PRIVATE_END: usize = 32768 * BANK;
pub(super) fn private_span(start: usize, count: usize) -> bool {
    (start < KERNEL_START && count <= KERNEL_START - start)
        || ((PRIVATE_START..PRIVATE_END).contains(&start) && count <= PRIVATE_END - start)
}
pub(super) fn shared_span(start: usize, count: usize) -> bool {
    ((KERNEL_START..PRIVATE_START).contains(&start) && count <= PRIVATE_START - start)
        || ((PRIVATE_END..CELLS).contains(&start) && count <= CELLS - start)
}
pub(super) fn private_free() -> BTreeMap<usize, usize> {
    BTreeMap::from([
        (5 * BANK + 1, KERNEL_START - 5 * BANK - 1),
        (PRIVATE_START + 1, PRIVATE_END - PRIVATE_START - 1),
    ])
}
fn kernel_index(index: usize) -> usize {
    if index < PRIVATE_START {
        index - KERNEL_START
    } else {
        PRIVATE_START - KERNEL_START + index - PRIVATE_END
    }
}
pub(super) type Page = [u16; PAGE_CELLS];
const LEAF_PAGES: usize = 128;
type Leaf = Vec<Arc<Page>>;

/// Fork shares one root. A write detaches only the root, its leaf, and the page.
/// Unchanged writes never detach any level, including the shared zero page.
#[derive(Clone)]
pub(super) struct Pages {
    root: Arc<Vec<Arc<Leaf>>>,
}
impl Pages {
    fn new(pages: Vec<Arc<Page>>) -> Self {
        Self {
            root: Arc::new(
                pages
                    .chunks(LEAF_PAGES)
                    .map(|leaf| Arc::new(leaf.to_vec()))
                    .collect(),
            ),
        }
    }
    /// A large zero address range costs one directory and one shared zero leaf,
    /// rather than one allocation (or even one page handle) per logical page.
    fn zeroed(count: usize) -> Self {
        assert_eq!(count % LEAF_PAGES, 0);
        let zero = Arc::new([0; PAGE_CELLS]);
        let leaf = Arc::new(vec![zero; LEAF_PAGES]);
        Self {
            root: Arc::new(vec![leaf; count / LEAF_PAGES]),
        }
    }
    #[cfg(test)]
    fn len(&self) -> usize {
        self.root.iter().map(|leaf| leaf.len()).sum()
    }
    #[cfg(test)]
    fn iter(&self) -> impl Iterator<Item = &Arc<Page>> {
        self.root.iter().flat_map(|leaf| leaf.iter())
    }
}
impl std::ops::Index<usize> for Pages {
    type Output = Arc<Page>;
    fn index(&self, index: usize) -> &Self::Output {
        &self.root[index / LEAF_PAGES][index % LEAF_PAGES]
    }
}
impl std::ops::IndexMut<usize> for Pages {
    fn index_mut(&mut self, index: usize) -> &mut Self::Output {
        let root = Arc::make_mut(&mut self.root);
        &mut Arc::make_mut(&mut root[index / LEAF_PAGES])[index % LEAF_PAGES]
    }
}

// A third directory level bounds fork detachment to a few KiB even for the
// multi-GiB private extension. The original low-memory fast path is unchanged.
const REGION_PAGES: usize = LEAF_PAGES * LEAF_PAGES;
const REGION_CELLS: usize = REGION_PAGES * PAGE_CELLS;
#[derive(Clone)]
pub(super) struct HeapPages {
    regions: Arc<Vec<Pages>>,
}
impl HeapPages {
    fn from_leaves(leaves: Vec<Arc<Leaf>>) -> Self {
        Self {
            regions: Arc::new(
                leaves
                    .chunks(LEAF_PAGES)
                    .map(|region| Pages {
                        root: Arc::new(region.to_vec()),
                    })
                    .collect(),
            ),
        }
    }
    fn zeroed() -> Self {
        let leaf = Arc::new(vec![Arc::new([0; PAGE_CELLS]); LEAF_PAGES]);
        let full = Pages {
            root: Arc::new(vec![leaf.clone(); LEAF_PAGES]),
        };
        let leaves = (PRIVATE_END - PRIVATE_START) / PAGE_CELLS / LEAF_PAGES;
        let mut regions = vec![full; leaves / LEAF_PAGES];
        if leaves % LEAF_PAGES != 0 {
            regions.push(Pages {
                root: Arc::new(vec![leaf; leaves % LEAF_PAGES]),
            });
        }
        Self {
            regions: Arc::new(regions),
        }
    }
    fn read(&self, index: usize) -> u16 {
        self.regions[index / REGION_CELLS][index % REGION_CELLS / PAGE_CELLS][index % PAGE_CELLS]
    }
    fn write(&mut self, index: usize, value: u16) {
        if self.read(index) != value {
            let region = &mut Arc::make_mut(&mut self.regions)[index / REGION_CELLS];
            Arc::make_mut(&mut region[index % REGION_CELLS / PAGE_CELLS])[index % PAGE_CELLS] =
                value;
        }
    }
    fn read_into(&self, mut start: usize, mut dest: &mut [u16]) {
        while !dest.is_empty() {
            let offset = start % PAGE_CELLS;
            let n = dest.len().min(PAGE_CELLS - offset);
            dest[..n].copy_from_slice(
                &self.regions[start / REGION_CELLS][start % REGION_CELLS / PAGE_CELLS]
                    [offset..offset + n],
            );
            start += n;
            dest = &mut dest[n..];
        }
    }
    fn write_from(&mut self, mut start: usize, mut values: &[u16]) {
        while !values.is_empty() {
            let offset = start % PAGE_CELLS;
            let n = values.len().min(PAGE_CELLS - offset);
            if self.regions[start / REGION_CELLS][start % REGION_CELLS / PAGE_CELLS]
                [offset..offset + n]
                != values[..n]
            {
                let region = &mut Arc::make_mut(&mut self.regions)[start / REGION_CELLS];
                Arc::make_mut(&mut region[start % REGION_CELLS / PAGE_CELLS])[offset..offset + n]
                    .copy_from_slice(&values[..n]);
            }
            start += n;
            values = &values[n..];
        }
    }
}

/// Keep the original 32 MiB region contiguous for ordinary native programs.
/// Higher banks are demand-allocated pages, so extending the address space does
/// not zero/copy gigabytes at startup or fork. Kernel storage uses the same tail.
#[derive(Clone)]
pub(super) struct Ram {
    base: Vec<u16>,
    tail: Pages,
}
const BASE_CELLS: usize = 256 * BANK;
impl Ram {
    fn new() -> Self {
        Self {
            base: vec![0; BASE_CELLS],
            tail: Pages::zeroed((CELLS - BASE_CELLS) / PAGE_CELLS),
        }
    }
    fn len(&self) -> usize {
        self.base.len() + self.tail.root.len() * LEAF_PAGES * PAGE_CELLS
    }
    fn read(&self, index: usize) -> u16 {
        if index < self.base.len() {
            self.base[index]
        } else {
            let at = index - self.base.len();
            self.tail[at / PAGE_CELLS][at % PAGE_CELLS]
        }
    }
    fn write(&mut self, index: usize, value: u16) {
        if index < self.base.len() {
            self.base[index] = value;
        } else {
            let at = index - self.base.len();
            if self.tail[at / PAGE_CELLS][at % PAGE_CELLS] != value {
                Arc::make_mut(&mut self.tail[at / PAGE_CELLS])[at % PAGE_CELLS] = value;
            }
        }
    }
    fn read_into(&self, mut start: usize, mut dest: &mut [u16]) {
        if start < self.base.len() {
            let n = dest.len().min(self.base.len() - start);
            dest[..n].copy_from_slice(&self.base[start..start + n]);
            start += n;
            dest = &mut dest[n..];
        }
        while !dest.is_empty() {
            let at = start - self.base.len();
            let offset = at % PAGE_CELLS;
            let n = dest.len().min(PAGE_CELLS - offset);
            dest[..n].copy_from_slice(&self.tail[at / PAGE_CELLS][offset..offset + n]);
            start += n;
            dest = &mut dest[n..];
        }
    }
    fn write_from(&mut self, mut start: usize, mut values: &[u16]) {
        if start < self.base.len() {
            let n = values.len().min(self.base.len() - start);
            self.base[start..start + n].copy_from_slice(&values[..n]);
            start += n;
            values = &values[n..];
        }
        while !values.is_empty() {
            let at = start - self.base.len();
            let offset = at % PAGE_CELLS;
            let n = values.len().min(PAGE_CELLS - offset);
            if self.tail[at / PAGE_CELLS][offset..offset + n] != values[..n] {
                Arc::make_mut(&mut self.tail[at / PAGE_CELLS])[offset..offset + n]
                    .copy_from_slice(&values[..n]);
            }
            start += n;
            values = &values[n..];
        }
    }
}

pub(super) struct Kernel {
    cells: Ram,
    free: BTreeMap<usize, usize>,
    allocated: BTreeMap<usize, usize>,
}

#[derive(Clone)]
pub(super) enum Memory {
    Flat(Ram),
    Paged {
        pages: Pages,
        heap: HeapPages,
        kernel: Arc<Mutex<Kernel>>,
    },
}
impl Default for Memory {
    fn default() -> Self {
        Self::Flat(Ram {
            base: Vec::new(),
            tail: Pages {
                root: Arc::new(Vec::new()),
            },
        })
    }
}
impl Memory {
    /// Retain shared ownership of an existing private page. No guest allocation,
    /// COW detachment or reference into a uniquely owned Vec is exposed. The Arc
    /// allocation stays immutable while retained: future writes use make_mut.
    pub(super) fn read_page(&self, index: usize) -> Option<Arc<Page>> {
        let Self::Paged { pages, heap, .. } = self else { return None };
        if (BANK..KERNEL_START).contains(&index) {
            Some(pages[index / PAGE_CELLS].clone())
        } else if (PRIVATE_START..PRIVATE_END).contains(&index) {
            let index = index - PRIVATE_START;
            Some(heap.regions[index / REGION_CELLS][index % REGION_CELLS / PAGE_CELLS].clone())
        } else {
            None
        }
    }
    pub fn new() -> Self {
        Self::Flat(Ram::new())
    }
    pub fn enable_processes(&mut self) {
        if matches!(self, Self::Paged { .. }) {
            return;
        }
        let Self::Flat(mut flat) = std::mem::take(self) else {
            unreachable!()
        };
        assert_eq!(flat.len(), CELLS);
        let zero = Arc::new([0; PAGE_CELLS]);
        let pages = flat.base[..KERNEL_START]
            .chunks_exact(PAGE_CELLS)
            .map(|cells| {
                if cells.iter().all(|v| *v == 0) {
                    zero.clone()
                } else {
                    Arc::new(cells.try_into().unwrap())
                }
            })
            .collect();
        let mut leaves = Arc::unwrap_or_clone(flat.tail.root);
        let shared = leaves.split_off((PRIVATE_END - PRIVATE_START) / PAGE_CELLS / LEAF_PAGES);
        let heap = HeapPages::from_leaves(leaves);
        let kernel = Kernel {
            cells: Ram {
                base: flat.base.split_off(KERNEL_START),
                tail: Pages {
                    root: Arc::new(shared),
                },
            },
            free: BTreeMap::from([
                (KERNEL_START + 1, PRIVATE_START - KERNEL_START - 1),
                (PRIVATE_END + 1, CELLS - PRIVATE_END - 1),
            ]),
            allocated: BTreeMap::new(),
        };
        *self = Self::Paged {
            pages: Pages::new(pages),
            heap,
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
            Self::Flat(cells) => cells.read(index),
            Self::Paged {
                pages,
                heap,
                kernel,
            } => {
                if index < KERNEL_START {
                    pages[index / PAGE_CELLS][index % PAGE_CELLS]
                } else if (PRIVATE_START..PRIVATE_END).contains(&index) {
                    heap.read(index - PRIVATE_START)
                } else {
                    kernel.lock().unwrap().cells.read(kernel_index(index))
                }
            }
        }
    }
    pub fn write(&mut self, index: usize, value: u16) {
        match self {
            Self::Flat(cells) => cells.write(index, value),
            Self::Paged {
                pages,
                heap,
                kernel,
            } => {
                if index < KERNEL_START {
                    if pages[index / PAGE_CELLS][index % PAGE_CELLS] != value {
                        Arc::make_mut(&mut pages[index / PAGE_CELLS])[index % PAGE_CELLS] = value;
                    }
                } else if (PRIVATE_START..PRIVATE_END).contains(&index) {
                    heap.write(index - PRIVATE_START, value);
                } else {
                    kernel
                        .lock()
                        .unwrap()
                        .cells
                        .write(kernel_index(index), value);
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
            Self::Flat(cells) => cells.read_into(start, dest),
            Self::Paged {
                pages,
                heap,
                kernel,
            } => {
                while !dest.is_empty() {
                    let n;
                    if start < KERNEL_START {
                        let offset = start % PAGE_CELLS;
                        n = dest.len().min(PAGE_CELLS - offset);
                        dest[..n].copy_from_slice(&pages[start / PAGE_CELLS][offset..offset + n]);
                    } else if start < PRIVATE_START {
                        n = dest.len().min(PRIVATE_START - start);
                        kernel
                            .lock()
                            .unwrap()
                            .cells
                            .read_into(kernel_index(start), &mut dest[..n]);
                    } else if start < PRIVATE_END {
                        n = dest.len().min(PRIVATE_END - start);
                        heap.read_into(start - PRIVATE_START, &mut dest[..n]);
                    } else {
                        n = dest.len();
                        kernel
                            .lock()
                            .unwrap()
                            .cells
                            .read_into(kernel_index(start), dest);
                    }
                    start += n;
                    dest = &mut dest[n..];
                }
            }
        }
    }
    pub fn write_from(&mut self, mut start: usize, mut values: &[u16]) {
        assert!(start <= self.len() && values.len() <= self.len() - start);
        match self {
            Self::Flat(cells) => cells.write_from(start, values),
            Self::Paged {
                pages,
                heap,
                kernel,
            } => {
                while !values.is_empty() {
                    let n;
                    if start < KERNEL_START {
                        let offset = start % PAGE_CELLS;
                        n = values.len().min(PAGE_CELLS - offset);
                        if pages[start / PAGE_CELLS][offset..offset + n] != values[..n] {
                            Arc::make_mut(&mut pages[start / PAGE_CELLS])[offset..offset + n]
                                .copy_from_slice(&values[..n]);
                        }
                    } else if start < PRIVATE_START {
                        n = values.len().min(PRIVATE_START - start);
                        kernel
                            .lock()
                            .unwrap()
                            .cells
                            .write_from(kernel_index(start), &values[..n]);
                    } else if start < PRIVATE_END {
                        n = values.len().min(PRIVATE_END - start);
                        heap.write_from(start - PRIVATE_START, &values[..n]);
                    } else {
                        n = values.len();
                        kernel
                            .lock()
                            .unwrap()
                            .cells
                            .write_from(kernel_index(start), values);
                    }
                    start += n;
                    values = &values[n..];
                }
            }
        }
    }
    pub fn reset_private_heap(&mut self) {
        let Self::Paged { heap, .. } = self else {
            unreachable!()
        };
        *heap = HeapPages::zeroed();
    }
    pub fn fill(&mut self, range: Range<usize>, value: u16) {
        assert!(range.start <= range.end && range.end <= self.len());
        if let Self::Flat(cells) = self {
            if range.end <= cells.base.len() {
                cells.base[range].fill(value);
                return;
            }
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
            if from.end <= cells.base.len() && dest + from.len() <= cells.base.len() {
                cells.base.copy_within(from, dest);
                return;
            }
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
    fn read_cells(&self, start: usize, dest: &mut [u16]) {
        self.read_into(start, dest);
    }
    fn write_cells(&mut self, start: usize, values: &[u16]) {
        self.write_from(start, values);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn extended_banks_are_sparse_and_preserved_when_enabling_processes() {
        let mut memory = Memory::new();
        let Memory::Flat(ram) = &memory else { panic!() };
        assert_eq!(ram.base.len(), BASE_CELLS);
        assert!(ram
            .tail
            .root
            .iter()
            .all(|leaf| Arc::ptr_eq(leaf, &ram.tail.root[0])));
        let data = [0xffff, 0x1234, 0xabcd, 7, 0, 0x8000];
        memory.write_from(BASE_CELLS - 3, &data);
        memory.write(CELLS - 1, 0x4321);
        memory.enable_processes();
        let mut child = memory.clone();
        let mut actual = [0; 6];
        child.read_into(BASE_CELLS - 3, &mut actual);
        assert_eq!(actual, data);
        assert_eq!(child.read(CELLS - 1), 0x4321);
        child.write(CELLS - 1, 0xbeef);
        assert_eq!(memory.read(CELLS - 1), 0xbeef);
        child.write(BANK + 1, 42);
        assert_eq!(memory.read(BANK + 1), 0);
        child.copy_within(BASE_CELLS - 3..BASE_CELLS + 3, BASE_CELLS - 1);
        // The low side is shared; the new high side is private to the child.
        child.read_into(BASE_CELLS - 1, &mut actual);
        assert_eq!(actual, data);
        assert_eq!(memory.read(BASE_CELLS), 7);
        memory.fill(CELLS - 3..CELLS, 0xcafe);
        assert!(child.cells(CELLS - 3..CELLS).all(|v| v == 0xcafe));
    }
    #[test]
    fn kernel_can_allocate_far_beyond_the_old_shared_arena() {
        let mut memory = Memory::new();
        memory.enable_processes();
        // 64 MiB physical if touched, above the old 2 MiB shared ceiling.
        let large = memory.kernel_allocate(32 * 1024 * 1024);
        assert_ne!(large, 0);
        let last = super::super::memory::linear(large) + 32 * 1024 * 1024 - 1;
        assert!(last > BASE_CELLS);
        memory.write(last, 0xabcd);
        let next = memory.kernel_allocate(4096);
        assert_ne!(next, 0);
        assert!(shared_span(super::super::memory::linear(next), 4096));
        memory.kernel_release(large);
        assert_eq!(memory.kernel_allocate(32 * 1024 * 1024), large);
    }
    #[test]
    fn sparse_flat_fork_detaches_only_the_written_tail_page() {
        let mut parent = Memory::new();
        let at = CELLS - PAGE_CELLS;
        parent.write(at, 11);
        let mut child = parent.clone();
        child.write(at + 1, 22);
        assert_eq!(parent.read(at + 1), 0);
        assert_eq!(child.read(at), 11);
        assert_eq!(child.read(at + 1), 22);
    }
    #[test]
    fn fork_shares_directory_and_detaches_only_the_modified_leaf() {
        let mut parent = Memory::new();
        parent.enable_processes();
        let mut child = parent.clone();
        child.write(LEAF_PAGES * PAGE_CELLS + 3, 0);
        child.write_from(LEAF_PAGES * PAGE_CELLS - 1, &[0; 4]);
        let (Memory::Paged { pages: a, .. }, Memory::Paged { pages: b, .. }) = (&parent, &child)
        else {
            panic!()
        };
        assert!(Arc::ptr_eq(&a.root, &b.root));
        let page_refs = Arc::strong_count(&a[0]);
        let mut grandchild = child.clone();
        let Memory::Paged { pages, .. } = &grandchild else {
            panic!()
        };
        assert_eq!(Arc::strong_count(&pages[0]), page_refs); // no 30,000 handle clones
        grandchild.write(LEAF_PAGES * PAGE_CELLS + 3, 99);
        let (Memory::Paged { pages: a, .. }, Memory::Paged { pages: b, .. }) =
            (&parent, &grandchild)
        else {
            panic!()
        };
        assert!(!Arc::ptr_eq(&a.root, &b.root));
        assert!(Arc::ptr_eq(&a.root[0], &b.root[0]));
        assert!(!Arc::ptr_eq(&a.root[1], &b.root[1]));
        assert!(Arc::ptr_eq(&a[LEAF_PAGES + 1], &b[LEAF_PAGES + 1]));
        child.write(LEAF_PAGES * PAGE_CELLS + 3, 7);
        assert_eq!(parent.read(LEAF_PAGES * PAGE_CELLS + 3), 0);
        assert_eq!(grandchild.read(LEAF_PAGES * PAGE_CELLS + 3), 99);
        child.write(KERNEL_START - 1, 0xffff);
        assert_eq!(parent.read(KERNEL_START - 1), 0);
        drop(parent);
        drop(grandchild);
        assert_eq!(child.read(LEAF_PAGES * PAGE_CELLS + 3), 7);
    }
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
        assert!(a.iter().zip(b.iter()).all(|(a, b)| Arc::ptr_eq(a, b)));
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
        let low = rt.allocate((KERNEL_START - (5 * BANK + 1)) as u64);
        let high = rt.allocate((PRIVATE_END - PRIVATE_START - 1) as u64);
        assert_ne!(low, 0);
        assert_ne!(high, 0);
        assert_eq!(rt.allocate(1), 0);
        let shared_low = rt
            .memory
            .kernel_allocate((PRIVATE_START - KERNEL_START - 1) as u64);
        let shared_high = rt.memory.kernel_allocate((CELLS - PRIVATE_END - 1) as u64);
        assert_ne!(shared_low, 0);
        assert_ne!(shared_high, 0);
        assert_eq!(rt.memory.kernel_allocate(1), 0);
        for (ptr, count) in [
            (low, KERNEL_START - 5 * BANK - 1),
            (high, PRIVATE_END - PRIVATE_START - 1),
        ] {
            let at = super::super::memory::linear(ptr);
            assert!(private_span(at, count));
            assert!(!shared_span(at, count));
            rt.release(ptr);
        }
        rt.memory.kernel_release(shared_low);
        rt.memory.kernel_release(shared_high);
        assert_ne!(rt.memory.kernel_allocate(100), 0);
        assert_ne!(rt.allocate(100), 0);
    }
    #[test]
    fn sparse_heap_cow_crosses_regions_without_copying_the_whole_directory() {
        let mut parent = Memory::new();
        parent.enable_processes();
        let at = PRIVATE_START + REGION_CELLS - 2;
        parent.write_from(at, &[1, 2, 3, 4]);
        let mut child = parent.clone();
        child.write(at, 1);
        let (Memory::Paged { heap: a, .. }, Memory::Paged { heap: b, .. }) = (&parent, &child)
        else {
            panic!()
        };
        assert!(Arc::ptr_eq(&a.regions, &b.regions));
        child.write_from(at, &[0xffff, 0xabcd, 0x1234, 0xbeef]);
        let (Memory::Paged { heap: a, .. }, Memory::Paged { heap: b, .. }) = (&parent, &child)
        else {
            panic!()
        };
        assert!(!Arc::ptr_eq(&a.regions, &b.regions));
        assert!(!Arc::ptr_eq(&a.regions[0].root, &b.regions[0].root));
        assert!(!Arc::ptr_eq(&a.regions[1].root, &b.regions[1].root));
        assert!(Arc::ptr_eq(&a.regions[2].root, &b.regions[2].root));
        assert_eq!(
            parent.cells(at..at + 4).collect::<Vec<_>>(),
            vec![1, 2, 3, 4]
        );
        child.reset_private_heap();
        assert!(child.cells(at..at + 4).all(|n| n == 0));
        assert_eq!(parent.read(at), 1);
    }
    #[test]
    fn shared_span_rejects_private_regions_and_boundary_crossings() {
        for (start, count) in [
            (KERNEL_START, 1),
            (PRIVATE_START - 2, 2),
            (PRIVATE_END, 1),
            (CELLS - 1, 1),
        ] {
            assert!(shared_span(start, count));
            assert!(!private_span(start, count));
        }
        for (start, count) in [(PRIVATE_START, 1), (PRIVATE_END - 2, 2), (BANK, 1)] {
            assert!(private_span(start, count));
            assert!(!shared_span(start, count));
        }
        for (start, count) in [
            (PRIVATE_START - 1, 2),
            (PRIVATE_END - 1, 2),
            (CELLS - 1, 2),
            (usize::MAX, 1),
        ] {
            assert!(!private_span(start, count));
            assert!(!shared_span(start, count));
        }
    }
    #[test]
    fn page_crossings_overlapping_moves_and_shared_boundary_match_flat_ram() {
        let mut paged = Memory::new();
        paged.enable_processes();
        let mut flat = Memory::new();
        for start in [
            PAGE_CELLS - 3,
            BANK - 3,
            KERNEL_START - 3,
            PRIVATE_START - 3,
            PRIVATE_END - 3,
        ] {
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
