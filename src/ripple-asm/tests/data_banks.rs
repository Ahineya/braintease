use ripple_asm::{AssemblerOptions, Linker, RippleAssembler};
use ripple_asm::types::{CompilerMetadata, Opcode};

fn data(cells: usize) -> ripple_asm::ObjectFile {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    let mut object = assembler.assemble(&format!(".data\n__gp_big:\nbig:\n.space {cells}\n")).unwrap();
    object.rcc = Some(CompilerMetadata { banked_data: true, ..Default::default() });
    object
}

#[test]
fn data_projections_normalize_both_components_after_archive_selection() {
    let source = "start:\nLI T0, %data_offset(big+16)\nLI T1, %data_bank(big+16)\nLI T2, %data_offset(big+-1)\nLI T3, %data_bank(big+-1)\nHALT\n";
    let entry = RippleAssembler::new(AssemblerOptions::default()).assemble(source).unwrap();
    let archive = Linker::create_archive(vec![("storage".into(), data(34))]);
    let image = Linker::new(16).link_archives(vec![entry], vec![archive]).unwrap();
    assert_eq!(image.data_labels["big"], 2);
    assert_eq!(image.instructions[..4].iter().map(|i| i.word2).collect::<Vec<_>>(), [2, 1, 1, 0]);
    assert_eq!(image.data.len(), 36);
}

#[test]
fn banked_data_capacity_includes_origin_and_legacy_objects_cannot_silently_wrap() {
    assert_eq!(Linker::new(16).link(vec![data(254)]).unwrap().data.len(), 256);
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    let mut object = assembler.assemble(".data\n__gp_big:\nbig:\n.space 254\n.code\n__rcc_init_big:\nRET\n").unwrap();
    object.rcc = Some(CompilerMetadata { banked_data: true, ..Default::default() });
    assert_eq!(Linker::new(16).link(vec![object.clone()]).unwrap().data.len(), 256);
    object.data.push(0);
    assert!(Linker::new(16).link(vec![object.clone()]).is_err());
    object.data.pop();
    object.rcc.as_mut().unwrap().banked_data = false;
    assert!(Linker::new(16).link(vec![object]).is_err());
}

#[test]
fn malformed_data_projections_fail_instead_of_truncating() {
    for expression in ["big+-3", "big+1048576", "big+9223372036854775807", "big+oops", "missing+0"] {
        let source = format!("start:\nLI T0, %data_offset({expression})\nHALT\n");
        let entry = RippleAssembler::new(AssemblerOptions::default()).assemble(&source).unwrap();
        assert!(Linker::new(16).link(vec![entry, data(34)]).is_err(), "{expression}");
    }
    let mut entry = RippleAssembler::new(AssemblerOptions::default()).assemble("start:\nLI T0, %data_bank(big+0)\nHALT\n").unwrap();
    entry.instructions[0].opcode = Opcode::Add as u8;
    assert!(Linker::new(16).link(vec![entry, data(34)]).is_err());
}

#[test]
fn space_accepts_wide_counts_without_wrapping_or_unbounded_allocation() {
    let mut options = AssemblerOptions::default();
    options.bank_size = 64000;
    let assembler = RippleAssembler::new(options);
    let object = assembler.assemble(".data\nlarge:\n.space 140003\ntail:\n.space 1\n").unwrap();
    assert_eq!(object.data.len(), 140004);
    assert_eq!(object.data_labels["tail"], 140005);
    for size in ["-1", "4294967295", "999999999999999999999"] {
        assert!(assembler.assemble(&format!(".data\n.space {size}\n")).is_err());
    }
}

#[test]
fn legacy_data_stays_first_and_standalone_stack_and_heap_follow_all_globals() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    let entry = assembler.assemble("start:\nLI T0, __rcc_layout_stack_bank\nLI T1, __rcc_layout_heap_bank\nLI T2, legacy\nLI T3, %data_offset(big+16)\nLI T4, %data_bank(big+16)\nLI T5, %offset(__rcc_stack_bank)\nHALT\n").unwrap();
    let legacy = assembler.assemble(".data\n__gp_legacy:\nlegacy:\n.space 4\n").unwrap();
    let stack_helper = assembler.assemble("__rcc_stack_bank:\nRET\n").unwrap();
    // Put banked data first in the input to exercise deliberate data reordering.
    let image = Linker::new(16).link(vec![entry, data(110), legacy, stack_helper]).unwrap();
    assert_eq!(image.data_labels["legacy"], 2);
    assert_eq!(image.data_labels["big"], 6);
    assert_eq!(image.instructions[..6].iter().map(|i| i.word2).collect::<Vec<_>>(), [9, 10, 2, 6, 1, 7]);
}

#[test]
fn sys_cli_rejects_multi_bank_data_before_writing_an_unloadable_image() {
    let directory = std::env::temp_dir().join(format!("ripple-sys-data-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let input = directory.join("large.pobj");
    let output = directory.join("large.sys");
    std::fs::write(&input, serde_json::to_vec(&data(34)).unwrap()).unwrap();
    let result = std::process::Command::new(env!("CARGO_BIN_EXE_rlink"))
        .args(["-f", "sys", "--bank-size", "16", "-o"])
        .arg(&output).arg(&input).output().unwrap();
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("one global data bank"));
    assert!(!output.exists());
    std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn cli_symbol_map_uses_instruction_indices_and_dense_data_cells() {
    let directory = std::env::temp_dir().join(format!("ripple-symbols-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let input = directory.join("small.pobj");
    let output = directory.join("small.rxe");
    let map = directory.join("symbols.json");
    let object = RippleAssembler::new(AssemblerOptions::default())
        .assemble("start:\nHALT\n.data\nslot:\n.space 3\n").unwrap();
    std::fs::write(&input, serde_json::to_vec(&object).unwrap()).unwrap();
    let result = std::process::Command::new(env!("CARGO_BIN_EXE_rlink"))
        .args(["-f", "rxe", "--bank-size", "16", "--map"])
        .arg(&map).arg("-o").arg(&output).arg(&input).output().unwrap();
    assert!(result.status.success(), "{}", String::from_utf8_lossy(&result.stderr));
    let symbols: serde_json::Value = serde_json::from_slice(&std::fs::read(&map).unwrap()).unwrap();
    assert_eq!(symbols["bank_size"], 16);
    assert_eq!(symbols["code"]["start"], 0);
    assert_eq!(symbols["data"]["slot"], 2);
    std::fs::remove_dir_all(directory).unwrap();
}
