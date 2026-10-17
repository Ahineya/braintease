//! Exercise host-clock mailbox commands through the exported BF CPU.
use bf_wasm::engine::Program;
use rbf::{
    devices::BfDevices,
    image::{Layout, ORIGIN},
    runner::{Host, RunOptions},
};
use ripple_asm::{AssemblerOptions, Linker, MacroFormatter, RippleAssembler};
use rvm::devices::{COMMAND_HOST_MONOTONIC, COMMAND_HOST_REALTIME};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

#[test]
fn real_bf_cpu_reads_host_clocks_without_logical_ticks() {
    let mut source = String::from("start:\n");
    for (descriptor, command, status) in [
        (63, COMMAND_HOST_MONOTONIC, 100),
        (83, COMMAND_HOST_REALTIME, 101),
    ] {
        for (i, value) in [1, 10, 0, 0, 0, 0, 0, 0, 0, 0].into_iter().enumerate() {
            source.push_str(&format!(
                "LI T0, {}\nLI T1, {value}\nSTORE T1, R0, T0\n",
                descriptor + i
            ));
        }
        source.push_str(&format!("LI T0, 28\nLI T1, {descriptor}\nSTORE T1, R0, T0\nLI T0, 29\nLI T1, {command}\nSTORE T1, R0, T0\nLI T0, 30\nLOAD T1, R0, T0\nLI T0, {status}\nSTORE T1, R0, T0\n"));
    }
    source.push_str("HALT\n");
    let object = RippleAssembler::new(AssemblerOptions {
        bank_size: 513,
        ..Default::default()
    })
    .assemble(&source)
    .unwrap();
    let image = Linker::new(513).link(vec![object]).unwrap();
    let mut bytes = Vec::new();
    MacroFormatter::new()
        .write_linked_program_host_bf(&image, 2, &mut bytes)
        .unwrap();
    let source = String::from_utf8(bytes).unwrap();
    let (layout, body) = Layout::parse(&source, 2_000_000).unwrap();
    let program = Program::parse(body, true, true).unwrap();
    let mut devices = BfDevices::new(513, 35, 1, None).unwrap();
    devices.host_input_enabled = false;
    let mut host = Host::new(layout, devices).unwrap();
    let before = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs();
    assert_eq!(
        host.run(
            &program,
            &RunOptions {
                jit: true,
                timeout: Some(Duration::from_secs(40)),
                ..Default::default()
            },
            &mut Vec::new()
        )
        .unwrap(),
        "halted"
    );
    let after = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs();
    let ram = |i: usize| host.machine.read_cell(ORIGIN + 8 * i + 4);
    assert_eq!(ram(100), 0);
    assert_eq!(ram(101), 0);
    let seconds = (0..4).fold(0u64, |n, i| n | u64::from(ram(85 + i)) << (i * 16));
    assert!((before..=after).contains(&seconds));
    for base in [63, 83] {
        let nanos = u32::from(ram(base + 6)) | u32::from(ram(base + 7)) << 16;
        assert!(nanos < 1_000_000_000);
        assert_eq!(nanos % 1_000_000, 0);
        assert_eq!(
            u32::from(ram(base + 8)) | u32::from(ram(base + 9)) << 16,
            1_000_000
        );
    }
    assert_eq!(host.devices.devices.tick(), 0);
}
