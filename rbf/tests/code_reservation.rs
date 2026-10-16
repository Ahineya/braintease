//! A tiny boot image must be able to load code well beyond its RAM footprint.
use bf_wasm::engine::Program;
use rbf::{
    devices::BfDevices,
    image::Layout,
    runner::{Host, RunOptions},
};
use ripple_asm::{AssemblerOptions, Linker, MacroFormatter, RippleAssembler};
use std::time::Duration;

#[test]
fn reserved_code_banks_load_and_execute_without_enlarging_guest_ram() {
    let object = RippleAssembler::new(AssemblerOptions {
        bank_size: 64,
        max_immediate: 65535,
        ..Default::default()
    }).assemble("start:\nLI T0, 67\nLI T1, 62\nLI X0, 14\nLI X1, 7\nLI X2, 65\nLI X3, 0\nSTORC R0, T0, T1\nLI T1, 63\nLI X0, 0\nSTORC R0, T0, T1\nLI T1, 62\nJALR R0, T0, T1\n").unwrap();
    let image = Linker::new(64).link(vec![object]).unwrap();
    for reserved in [0, 68] {
        let formatter = MacroFormatter::new()
            .with_irq(true)
            .with_code_banks(reserved);
        let mut bytes = Vec::new();
        formatter
            .write_linked_program_host_bf(&image, 8, &mut bytes)
            .unwrap();
        let source = String::from_utf8(bytes).unwrap();
        let macros = formatter.format_linked_program_host(&image, 8).unwrap();
        let header = source.split_once("END_BFHOST").unwrap().0;
        assert!(macros.contains(header.trim()));
        let (layout, body) = Layout::parse(&source, 200_000).unwrap();
        assert_eq!(layout.memory_banks, 8);
        if reserved != 0 {
            assert!(layout.tape_cells >= 8 * (76 + 4 * 68 * 64 + 33));
        }
        let program = Program::parse(body, true, true).unwrap();
        for jit in [false, true] {
            let mut host =
                Host::new(layout.clone(), BfDevices::new(64, 35, 1, None).unwrap()).unwrap();
            let result = host.run(
                &program,
                &RunOptions {
                    jit,
                    max_instructions: Some(100),
                    timeout: Some(Duration::from_secs(30)),
                    ..Default::default()
                },
                &mut Vec::new(),
            );
            if reserved == 0 {
                assert!(result.unwrap_err().contains("BF CPU execution fault"));
            } else {
                assert_eq!(result.unwrap(), "halted");
                assert_eq!(host.machine.read_cell(8 + 8 * 8 + 1), 65); // A0 = R7
                assert_eq!(host.machine.read_cell(8 + 8 * 81 + 1), 68);
                assert_eq!(host.machine.read_cell(8 + 8 * 82 + 1), 0);
            }
        }
    }
}
