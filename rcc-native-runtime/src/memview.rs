//! Versioned native read capability. This is never a guest C16 structure.
use super::{
    address_space::{Page, PAGE_CELLS},
    Runtime, BANK, BANKS,
};
use std::sync::Arc;

#[repr(C, align(8))]
#[derive(Default)]
pub(super) struct Record {
    fields: [u64; 5],
}

#[derive(Default)]
pub(super) struct ReadView {
    record: Record,
    // Derive the raw address from this shared Arc allocation, not from a
    // mutable Runtime/Memory borrow. Reborrowing Runtime during poll cannot
    // retag this separately allocated, shared immutable page as exclusive.
    // Retention also prevents reclamation across any accidentally stale native
    // address; correctness still requires compiler barriers before mutation.
    // One retained page per Runtime, never copied into process snapshots.
    page: Option<Arc<Page>>,
}

#[no_mangle]
unsafe extern "C" fn rcc_native_memview_v1(
    ctx: *mut Runtime,
    hint: u64,
    count: u64,
    flags: u64,
) -> u64 {
    let rt = unsafe { &mut *ctx };
    // Another acquisition ends the previous grant, even on denial.
    rt.read_view.page = None;
    rt.read_view.record.fields = [0; 5];
    let bank = hint >> 16;
    let offset = hint & 65535;
    if flags != 1
        || count == 0
        || bank == 0
        || bank >= BANKS as u64
        || offset >= BANK as u64
        || count > BANK as u64 - offset
    {
        return 0;
    }
    let index = bank as usize * BANK + offset as usize;
    let page_start = index / PAGE_CELLS * PAGE_CELLS;
    let start = page_start.max(bank as usize * BANK);
    let end = (page_start + PAGE_CELLS).min((bank as usize + 1) * BANK);
    if count > (end - index) as u64 {
        return 0;
    }
    let Some(page) = rt.memory.read_page(index) else {
        return 0;
    };
    // Arc::as_ptr gives shared allocation provenance. The stored clone forces
    // all writes through a COW detach; no &mut Page coexists with native reads.
    let base = unsafe { Arc::as_ptr(&page).cast::<u16>().add(start - page_start) };
    rt.read_view.record.fields = [1, 1, start as u64, (end - start) as u64, base as u64];
    rt.read_view.page = Some(page);
    std::ptr::addr_of!(rt.read_view.record) as u64
}

#[cfg(test)]
mod tests {
    use super::super::{address_space, memory::pointer};
    use super::*;

    #[test]
    fn slow_poll_with_a_live_grant_still_terminates_on_deadline() {
        if std::env::var_os("RCC_MEMVIEW_DEADLINE_CHILD").is_some() {
            let mut rt = Runtime::new();
            rt.enable_process_memory();
            assert_ne!(
                unsafe { rcc_native_memview_v1(&mut rt, pointer(BANK + 8), 1, 1) },
                0
            );
            rt.deadline = Some(std::time::Instant::now());
            rt.poll_count = 1023;
            rt.poll();
            unreachable!("expired poll returned");
        }
        let out = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "memview::tests::slow_poll_with_a_live_grant_still_terminates_on_deadline",
                "--nocapture",
            ])
            .env("RCC_MEMVIEW_DEADLINE_CHILD", "1")
            .output()
            .unwrap();
        assert_eq!(out.status.code(), Some(1));
        assert!(String::from_utf8_lossy(&out.stderr).contains("native wall-time limit exceeded"));
    }

    #[test]
    fn private_grants_cover_whole_requests_and_survive_slow_poll() {
        let mut rt = Runtime::new();
        unsafe {
            assert_eq!(rcc_native_memview_v1(&mut rt, pointer(BANK + 3), 1, 1), 0);
        }
        rt.enable_process_memory();
        for index in [
            BANK + 3,
            239 * BANK + 63998,
            256 * BANK + 3,
            32767 * BANK + 63998,
        ] {
            rt.memory.write(index, 0xbeef);
            let before = rt.poll_count;
            let p = unsafe { rcc_native_memview_v1(&mut rt, pointer(index), 1, 1) };
            assert_ne!(p, 0);
            assert_eq!(rt.poll_count, before);
            assert_eq!(p % 8, 0);
            let record = unsafe { *(p as *const [u64; 5]) };
            assert_eq!(&record[..2], &[1, 1]);
            assert!(record[2] <= index as u64 && index as u64 - record[2] < record[3]);
            let cell = unsafe { (record[4] as *const u16).add(index - record[2] as usize) };
            for _ in 0..2049 {
                rt.poll();
            }
            assert_eq!(unsafe { *cell }, 0xbeef);
            let mut child = rt.memory.clone();
            child.write(index, 0x1234);
            assert_eq!(rt.memory.read(index), 0xbeef);
            assert_eq!(unsafe { *cell }, 0xbeef);
            // A store is a compiler lifetime barrier. The old pin deliberately
            // remains owned until acquisition/drop, proving it cannot dangle.
            rt.memory.write(index, 0x5678);
            assert_eq!(unsafe { *cell }, 0xbeef);
            assert_eq!(rt.memory.read(index), 0x5678);
            let p = unsafe { rcc_native_memview_v1(&mut rt, pointer(index), 1, 1) };
            let record = unsafe { *(p as *const [u64; 5]) };
            assert_eq!(
                unsafe { *((record[4] as *const u16).add(index - record[2] as usize)) },
                0x5678
            );
        }
        assert_eq!(std::mem::size_of::<Record>(), 40);
        assert_eq!(std::mem::align_of::<Record>(), 8);
        assert_eq!(address_space::PAGE_CELLS, 512);
    }

    #[test]
    fn denial_never_faults_polls_or_accepts_partial_spans() {
        let mut rt = Runtime::new();
        rt.enable_process_memory();
        for (hint, count, flags) in [
            (0, 1, 1),
            (pointer(240 * BANK), 1, 1),
            (pointer(32768 * BANK), 1, 1),
            (1 << 32, 1, 1),
            ((1 << 16) | 65535, 1, 1),
            (pointer(BANK), 0, 1),
            (pointer(BANK), u64::MAX, 1),
            (pointer(BANK), 1, 0),
            (pointer(BANK), 1, 3),
            (pointer(BANK + 511), 2, 1),
            (pointer(2 * BANK - 1), 2, 1),
        ] {
            let polls = rt.poll_count;
            assert_eq!(
                unsafe { rcc_native_memview_v1(&mut rt, hint, count, flags) },
                0,
                "hint={hint:x} count={count} flags={flags}"
            );
            assert_eq!(rt.poll_count, polls);
        }
    }
}
