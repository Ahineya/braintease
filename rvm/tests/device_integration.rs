//! Device contract tests use the Rust VM exclusively, with private temporary disks.
use rvm::constants::{
    DISP_OFF, DISP_RGB565, DISP_RGB565_EXT, HDR_DISP_FLUSH, HDR_DISP_MODE, HDR_DISP_RESOLUTION,
};
use rvm::devices::InputTransition;
use rvm::VM;
use std::fs;
use std::ops::{Deref, DerefMut};
use std::path::PathBuf;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

const BANK_SIZE: u16 = 64000;
const DESCRIPTOR: usize = 4 * BANK_SIZE as usize + 100;
static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(0);

struct TestVm {
    vm: Option<VM>,
    directory: PathBuf,
}
impl TestVm {
    fn new() -> Self {
        let directory = std::env::temp_dir().join(format!(
            "rvm-device-integration-{}-{}",
            std::process::id(),
            NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&directory).unwrap();
        let mut vm = VM::with_options(BANK_SIZE, 300000, Some(directory.join("disk.img")));
        vm.host_input_enabled = false;
        Self {
            vm: Some(vm),
            directory,
        }
    }
    fn command(&mut self, command: u16, descriptor: &[u16]) -> u16 {
        self.memory[DESCRIPTOR..DESCRIPTOR + descriptor.len()].copy_from_slice(descriptor);
        self.write_cell(0, 27, 4).unwrap();
        self.write_cell(0, 28, 100).unwrap();
        self.write_cell(0, 29, command).unwrap();
        self.read_cell(0, 30).unwrap()
    }
    fn mode(&mut self, mode: u16) {
        self.write_cell(0, HDR_DISP_MODE as u16, mode).unwrap();
    }
}
impl Deref for TestVm {
    type Target = VM;
    fn deref(&self) -> &VM {
        self.vm.as_ref().unwrap()
    }
}
impl DerefMut for TestVm {
    fn deref_mut(&mut self) -> &mut VM {
        self.vm.as_mut().unwrap()
    }
}
impl Drop for TestVm {
    fn drop(&mut self) {
        drop(self.vm.take());
        let _ = fs::remove_dir_all(&self.directory);
    }
}

#[test]
fn tiny_legacy_frame_routes_below_text_vram_boundary_and_preserves_other_ram() {
    let mut vm = TestVm::new();
    vm.write_cell(0, HDR_DISP_RESOLUTION as u16, (3 << 8) | 2)
        .unwrap();
    vm.mode(DISP_RGB565);
    let colors = [0xf800, 0x07e0, 0x001f, 0xffff, 0, 0x8410];
    for (index, color) in colors.iter().enumerate() {
        vm.write_cell(0, 38 + index as u16, *color).unwrap();
    }
    vm.write_cell(0, 32, 0xffff).unwrap(); // front buffer remains read-only
    vm.write_cell(0, 44, 0xabcd).unwrap(); // first cell after both buffers
    vm.write_cell(0, 2000, 0x1234).unwrap();
    vm.write_cell(1, HDR_DISP_MODE as u16, 0x5678).unwrap();
    assert_eq!(vm.read_cell(0, 32).unwrap(), 0);
    assert_eq!(vm.read_cell(0, 38).unwrap(), colors[0]);
    assert_eq!(vm.read_cell(0, 44).unwrap(), 0xabcd);
    assert_eq!(vm.read_cell(0, 2000).unwrap(), 0x1234);
    assert_eq!(vm.read_cell(1, HDR_DISP_MODE as u16).unwrap(), 0x5678);
    assert_eq!(vm.read_cell(0, HDR_DISP_MODE as u16).unwrap(), DISP_RGB565);
    vm.write_cell(0, HDR_DISP_FLUSH as u16, 1).unwrap();
    assert_eq!(
        vm.rgb565_display
            .as_ref()
            .unwrap()
            .snapshot()
            .unwrap()
            .pixels,
        colors
    );
    assert_eq!(vm.presented_frames, 1);
    assert_eq!(vm.read_cell(0, 32).unwrap(), colors[0]);
    assert_eq!(vm.read_cell(0, 38).unwrap(), 0);
}

#[test]
fn mode_switches_preserve_window_connection_and_frame_sequence() {
    let mut vm = TestVm::new();
    vm.write_cell(0, HDR_DISP_RESOLUTION as u16, 0x0101)
        .unwrap();
    vm.mode(DISP_RGB565);
    let connection = vm.rgb565_display.as_ref().unwrap().get_state();
    vm.write_cell(0, 33, 0xf800).unwrap();
    vm.write_cell(0, HDR_DISP_FLUSH as u16, 1).unwrap();
    vm.mode(DISP_OFF);
    assert!(!connection.lock().unwrap().initialized);
    assert!(!connection.lock().unwrap().should_quit());
    vm.mode(DISP_RGB565_EXT);
    assert!(Arc::ptr_eq(
        &connection,
        &vm.rgb565_display.as_ref().unwrap().get_state()
    ));
    vm.memory[BANK_SIZE as usize] = 0x001f;
    assert_eq!(vm.command(1, &[1, 8, 1, 1, 1, 1, 0, 0]), 0);
    let snapshot = vm.rgb565_display.as_ref().unwrap().snapshot().unwrap();
    assert_eq!(snapshot.sequence, 2);
    assert_eq!(snapshot.pixels, [0x001f]);
    assert_eq!(vm.presented_frames, 2);
    assert!(!connection.lock().unwrap().should_quit());
}

#[test]
fn extended_frame_crosses_banks_and_publication_is_immutable() {
    let mut vm = TestVm::new();
    vm.mode(DISP_RGB565_EXT);
    let start = BANK_SIZE as usize + 63997;
    let expected: Vec<u16> = (0..64000u32)
        .map(|index| index.wrapping_mul(37) as u16)
        .collect();
    vm.memory[start..start + expected.len()].copy_from_slice(&expected);
    assert_eq!(vm.command(1, &[1, 8, 320, 200, 320, 1, 63997, 0]), 0);
    let snapshot = vm.rgb565_display.as_ref().unwrap().snapshot().unwrap();
    assert_eq!((snapshot.width, snapshot.height), (320, 200));
    assert_eq!(snapshot.pixels, expected);
    assert_eq!(snapshot.sequence, 1);
    vm.write_cell(1, 63997, 0xffff).unwrap();
    vm.write_cell(2, 0, 0xffff).unwrap();
    assert_eq!(
        vm.rgb565_display.as_ref().unwrap().snapshot().unwrap(),
        snapshot
    );
    assert_eq!(vm.command(1, &[1, 8, 320, 200, 320, 1, 63997, 0]), 0);
    let next = vm.rgb565_display.as_ref().unwrap().snapshot().unwrap();
    assert_eq!(next.sequence, 2);
    assert_eq!(next.pixels[0], 0xffff);
    assert_eq!(next.pixels[3], 0xffff);
    assert_eq!(snapshot.pixels[0], 0);
}

#[test]
fn extended_stride_skips_padding_and_does_not_reserve_guest_ram() {
    let mut vm = TestVm::new();
    vm.mode(DISP_RGB565_EXT);
    let start = 2 * BANK_SIZE as usize;
    vm.memory[start..start + 8].copy_from_slice(&[1, 2, 3, 99, 99, 4, 5, 6]);
    assert_eq!(vm.command(1, &[1, 8, 3, 2, 5, 2, 0, 0]), 0);
    assert_eq!(
        vm.rgb565_display
            .as_ref()
            .unwrap()
            .snapshot()
            .unwrap()
            .pixels,
        [1, 2, 3, 4, 5, 6]
    );
    vm.write_cell(0, 32, 0xabcd).unwrap();
    assert_eq!(vm.memory[32], 0xabcd);
    assert_eq!(vm.read_cell(0, 32).unwrap(), 0xabcd);
}

#[test]
fn malformed_present_descriptors_never_publish_or_replace_previous_frame() {
    let mut vm = TestVm::new();
    vm.mode(DISP_RGB565_EXT);
    vm.memory[BANK_SIZE as usize] = 0x07e0;
    assert_eq!(vm.command(1, &[1, 8, 1, 1, 1, 1, 0, 0]), 0);
    let expected = vm.rgb565_display.as_ref().unwrap().snapshot().unwrap();
    let bad = [
        [2, 8, 1, 1, 1, 1, 0, 0],           // unknown ABI version
        [1, 7, 1, 1, 1, 1, 0, 0],           // short descriptor
        [1, 8, 0, 1, 1, 1, 0, 0],           // zero dimension
        [1, 8, 3, 2, 2, 1, 0, 0],           // overlapping rows
        [1, 8, 1, 1, 1, 1, 0, 1],           // reserved flags
        [1, 8, 1, 1, 1, 1, 64000, 0],       // noncanonical bank offset
        [1, 8, 320, 200, 320, 4, 63000, 0], // outside allocated guest RAM
        [1, 8, 1, 1, 1, 0, 0, 0],           // MMIO is not ordinary pixel RAM
    ];
    for descriptor in bad {
        assert_ne!(vm.command(1, &descriptor), 0, "accepted {descriptor:?}");
        assert_eq!(
            vm.rgb565_display.as_ref().unwrap().snapshot().unwrap(),
            expected
        );
    }
    assert_eq!(vm.presented_frames, 1);
}

#[test]
fn tick_grants_deliver_controller_input_and_clock_reads_and_present_do_not_advance_time() {
    let mut vm = TestVm::new();
    vm.mode(DISP_RGB565_EXT);
    let clock = [1, 8, 0, 0, 0, 0, 0, 0];
    assert_eq!(vm.command(4, &clock), 0);
    assert_eq!(
        &vm.memory[DESCRIPTOR + 2..DESCRIPTOR + 8],
        &[0, 0, 0, 0, 35, 0]
    );
    assert_eq!(vm.command(3, &clock), 0x8000);
    assert_eq!(vm.devices.tick(), 0);
    vm.complete_tick(vec![InputTransition::Key {
        key: 82,
        down: true,
    }])
    .unwrap();
    assert_eq!(vm.read_cell(0, 30).unwrap(), 0);
    assert_eq!(vm.devices.tick(), 1);
    assert_eq!(vm.devices.millis(), 28);
    assert_eq!(vm.read_cell(0, 10).unwrap(), 0);
    for _ in 0..20 {
        assert_eq!(vm.command(4, &clock), 0);
    }
    assert_eq!(vm.devices.tick(), 1);
    assert_eq!(vm.devices.millis(), 28);
    for _ in 1..35 {
        assert_eq!(vm.command(3, &clock), 0x8000);
        vm.complete_tick(Vec::new()).unwrap();
    }
    assert_eq!(vm.devices.tick(), 35);
    assert_eq!(vm.devices.millis(), 1000);
    vm.memory[BANK_SIZE as usize] = 0xffff;
    assert_eq!(vm.command(1, &[1, 8, 1, 1, 1, 1, 0, 0]), 0);
    assert_eq!(vm.devices.tick(), 35);
    assert_eq!(vm.devices.millis(), 1000);
}

#[test]
fn live_keyboard_polling_works_before_any_tick_and_does_not_advance_replay() {
    let mut vm = TestVm::new();
    vm.host_input_enabled = true;
    vm.mode(DISP_RGB565_EXT);
    vm.devices
        .set_script(
            rvm::devices::InputScript::from_json(r#"[{"tick":1,"key":"A","down":true}]"#).unwrap(),
        )
        .unwrap();
    let display = vm.rgb565_display.as_ref().unwrap().get_state();
    display.lock().unwrap().set_key(82, true);
    let event = [1, 10, 1, 2, 0, 0, 0, 0, 0, 0];
    assert_eq!(vm.command(5, &event), 0);
    assert_eq!(
        &vm.memory[2 * BANK_SIZE as usize..2 * BANK_SIZE as usize + 4],
        &[1, 1, 82, 0]
    );
    assert_eq!(vm.devices.tick(), 0);
    assert!(!vm.devices.held(4));
    for _ in 0..3 {
        assert_eq!(vm.command(5, &event), 0);
        assert_eq!(vm.memory[DESCRIPTOR + 5], 0);
    }
    display.lock().unwrap().set_key(82, false);
    let mut state = [0; 28];
    state[0] = 1;
    state[1] = 28;
    assert_eq!(vm.command(6, &state), 0);
    assert!(!vm.devices.held(82));
    assert_eq!(vm.devices.tick(), 0);
    // A queued release at tick zero must survive checkpoint validation.
    rvm::devices::DeviceState::restore_checkpoint(vm.devices.checkpoint()).unwrap();
    assert_eq!(vm.command(5, &event), 0);
    assert_eq!(
        &vm.memory[2 * BANK_SIZE as usize..2 * BANK_SIZE as usize + 4],
        &[1, 0, 82, 0]
    );
    assert_eq!(vm.command(3, &[1, 8, 0, 0, 0, 0, 0, 0]), 0x8000);
    vm.complete_tick(Vec::new()).unwrap();
    assert_eq!(vm.command(5, &event), 0);
    assert_eq!(
        &vm.memory[2 * BANK_SIZE as usize..2 * BANK_SIZE as usize + 4],
        &[1, 1, 4, 0]
    );
}

#[test]
fn console_dma_preserves_transcript_order_and_rejects_bad_descriptors() {
    let mut vm = TestVm::new();
    vm.write_cell(0, 0, b'A' as u16).unwrap();
    let source = 2 * BANK_SIZE as usize - 2;
    vm.memory[source..source + 4].copy_from_slice(&[b'B' as u16, 0, 10, 0x1ff]);
    assert_eq!(vm.command(rvm::terminal::COMMAND, &[1,8,1,63998,4,0,0,0]),0);
    assert_eq!(vm.memory[DESCRIPTOR + 6],4);
    vm.write_cell(0, 0, b'C' as u16).unwrap();
    assert_eq!(vm.get_output(),b"AB\0\n\xffC");
    assert_eq!(vm.devices.tick(),0);
    for descriptor in [[2,8,1,63998,4,0,0,0], [1,7,1,63998,4,0,0,0],
                       [1,8,0,0,1,0,0,0], [1,8,4,100,8,0,0,0]] {
        let before = vm.memory.clone();
        assert_ne!(vm.command(rvm::terminal::COMMAND,&descriptor),0);
        assert!(vm.get_output().is_empty());
        // The caller placed the invalid descriptor; the device changes nothing.
        assert_eq!(vm.memory[0..DESCRIPTOR],before[0..DESCRIPTOR]);
        assert_eq!(vm.memory[DESCRIPTOR+8..],before[DESCRIPTOR+8..]);
        assert_eq!(vm.memory[DESCRIPTOR..DESCRIPTOR+8],descriptor);
    }
    assert_eq!(vm.command(rvm::terminal::COMMAND,&[1,8,1,0,0,0,0,0]),0);
    assert!(vm.get_output().is_empty());
}
