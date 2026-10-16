use rbf::{
    devices::BfDevices,
    image::{StridedMemory, ORIGIN},
};
use rvm::{
    constants::*,
    devices::{
        GuestMemory, InputScript, ABI_VERSION, COMMAND_NEXT_TICK, REGISTER_COMMAND,
        REGISTER_DESCRIPTOR_OFFSET,
    },
};

fn store<M: GuestMemory + ?Sized>(
    host: &mut BfDevices,
    memory: &mut M,
    address: usize,
    value: u16,
) {
    memory.write(address, value);
    host.write_mmio(address, value, memory).unwrap();
}

#[test]
fn text_flush_publishes_only_enabled_completed_immutable_frames() {
    let mut host = BfDevices::new(2048, 35, 1, None).unwrap();
    let mut memory = vec![0; 2048];
    store(&mut host, &mut memory, HDR_DISP_MODE, DISP_TEXT40);
    store(&mut host, &mut memory, TEXT40_BASE_WORD, 0x1e41);
    store(&mut host, &mut memory, TEXT40_LAST_WORD, 0x4f5a);
    store(&mut host, &mut memory, HDR_DISP_FLUSH, 1);
    assert_eq!(host.presented_frames, 0);
    assert!(host.text_display.snapshot().is_none());
    store(&mut host, &mut memory, HDR_DISP_CTL, DISP_ENABLE);
    store(&mut host, &mut memory, HDR_DISP_FLUSH, 0);
    assert_eq!(host.presented_frames, 0);
    store(&mut host, &mut memory, HDR_DISP_FLUSH, 1);
    let first = host.text_display.snapshot().unwrap().clone();
    assert_eq!(first.sequence, 1);
    assert_eq!(first.cells[0], 0x1e41);
    assert_eq!(first.cells[999], 0x4f5a);
    assert!(first.cells[1..999].iter().all(|&cell| cell == 0));
    assert_eq!(
        first.text().lines().next().unwrap(),
        "A                                       "
    );
    assert!(host.display.snapshot().is_none());
    store(&mut host, &mut memory, TEXT40_BASE_WORD, 0x1242);
    assert_eq!(host.text_display.snapshot().unwrap(), &first);
    store(&mut host, &mut memory, HDR_DISP_FLUSH, 1);
    assert_eq!(host.presented_frames, 2);
    assert_eq!(host.text_display.snapshot().unwrap().cells[0], 0x1242);
    assert_eq!(first.cells[0], 0x1e41);
}

#[test]
fn text_clear_crosses_banks_without_touching_other_tape_lanes_or_guards() {
    let cells = 1152;
    let mut tape = vec![0xa55a; ORIGIN + cells * 8];
    {
        let mut memory = StridedMemory::new(&mut tape, cells);
        let mut host = BfDevices::new(64, 35, 1, None).unwrap();
        store(&mut host, &mut memory, HDR_DISP_MODE, DISP_TEXT40);
        store(
            &mut host,
            &mut memory,
            HDR_DISP_CTL,
            DISP_ENABLE | DISP_CLEAR,
        );
        assert_eq!(memory.read(TEXT40_BASE_WORD - 1), 0xa55a);
        assert_eq!(memory.read(TEXT40_LAST_WORD + 1), 0xa55a);
        assert!((TEXT40_BASE_WORD..=TEXT40_LAST_WORD).all(|address| memory.read(address) == 0));
        assert_eq!(
            host.read_mmio(HDR_DISP_CTL, &mut memory).unwrap(),
            DISP_ENABLE
        );
        store(&mut host, &mut memory, TEXT40_LAST_WORD, 0xab43);
        store(&mut host, &mut memory, HDR_DISP_FLUSH, 1);
        assert_eq!(host.text_display.snapshot().unwrap().cells[999], 0xab43);
    }
    assert!(tape[..ORIGIN].iter().all(|&value| value == 0xa55a));
    for (index, &value) in tape[ORIGIN..].iter().enumerate() {
        if index % 8 != 4 {
            assert_eq!(value, 0xa55a, "non-RAM lane changed at {index}");
        }
    }
}

#[test]
fn display_switches_drop_old_text_frames_and_preserve_guest_vram() {
    let mut host = BfDevices::new(2048, 35, 1, None).unwrap();
    let mut memory = vec![0; 2048];
    store(&mut host, &mut memory, HDR_DISP_MODE, DISP_TEXT40);
    store(&mut host, &mut memory, HDR_DISP_CTL, DISP_ENABLE);
    store(&mut host, &mut memory, TEXT40_BASE_WORD, 0x0741);
    store(&mut host, &mut memory, HDR_DISP_FLUSH, 1);
    store(&mut host, &mut memory, HDR_DISP_RESOLUTION, 0x0101);
    store(&mut host, &mut memory, HDR_DISP_MODE, DISP_RGB565);
    assert!(host.text_display.snapshot().is_none());
    store(&mut host, &mut memory, 33, 0xf800);
    store(&mut host, &mut memory, HDR_DISP_FLUSH, 1);
    assert_eq!(host.presented_frames, 2);
    assert_eq!(host.display.snapshot().unwrap().pixels, [0xf800]);
    store(&mut host, &mut memory, HDR_DISP_MODE, DISP_TEXT40);
    assert!(host.display.snapshot().is_none());
    assert!(host.text_display.snapshot().is_none());
    assert_eq!(
        host.read_mmio(TEXT40_BASE_WORD, &mut memory).unwrap(),
        0x0741
    );
    store(&mut host, &mut memory, HDR_DISP_FLUSH, 1);
    assert_eq!(host.text_display.snapshot().unwrap().sequence, 3);
    store(&mut host, &mut memory, HDR_DISP_MODE, DISP_OFF);
    assert!(host.text_display.snapshot().is_none());
    assert!(!host.window_closed());
}

#[test]
fn deterministic_text_input_uses_scripted_tick_state() {
    let mut host = BfDevices::new(2048, 35, 1, None).unwrap();
    host.host_input_enabled = false;
    host.devices
        .set_script(
            InputScript::from_json(
                r#"[{"tick":1,"key":"UP","down":true},{"tick":2,"key":"UP","down":false}]"#,
            )
            .unwrap(),
        )
        .unwrap();
    let mut memory = vec![0; 2048];
    store(&mut host, &mut memory, HDR_DISP_MODE, DISP_TEXT40);
    memory[1100] = ABI_VERSION;
    memory[1101] = 8;
    store(&mut host, &mut memory, REGISTER_DESCRIPTOR_OFFSET, 1100);
    assert_eq!(host.read_mmio(10, &mut memory).unwrap(), 0);
    for expected in [1, 0] {
        store(&mut host, &mut memory, REGISTER_COMMAND, COMMAND_NEXT_TICK);
        host.complete_tick(&mut memory, vec![]).unwrap();
        for _ in 0..12 {
            assert_eq!(host.read_mmio(10, &mut memory).unwrap(), 0);
            assert_eq!(host.devices.held(82), expected != 0);
        }
    }
}
