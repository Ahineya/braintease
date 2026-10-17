use ripple_asm::{AssemblerOptions, Linker, RippleAssembler};
use rvm::{bulk::*, devices::*, Instr, VM};

fn submit(vm: &mut VM, command: u16, words: &[u16]) -> u16 {
    vm.memory[40..40 + words.len()].copy_from_slice(words);
    vm.write_cell(0, REGISTER_DESCRIPTOR_BANK as u16, 0)
        .unwrap();
    vm.write_cell(0, REGISTER_DESCRIPTOR_OFFSET as u16, 40)
        .unwrap();
    vm.write_cell(0, REGISTER_COMMAND as u16, command).unwrap();
    vm.read_cell(0, REGISTER_STATUS as u16).unwrap()
}

#[test]
fn binary_budget_validation_precedes_code_replacement() {
    let assembler = RippleAssembler::new(AssemblerOptions {
        bank_size: 8,
        ..Default::default()
    });
    let image = Linker::new(8)
        .link(vec![assembler.assemble("start:\nNOP\nHALT\n").unwrap()])
        .unwrap()
        .to_binary();
    let mut vm = VM::with_memory_size(65, 520);
    vm.instructions.push(Instr::new(0x42, 0x42, 0, 0, 0));
    vm.set_code_memory_limit(1).unwrap();
    assert!(vm
        .load_binary(&image)
        .unwrap_err()
        .contains("code-memory limit"));
    assert_eq!(vm.bank_size, 65);
    assert_eq!(vm.instructions.len(), 1);
    assert!(vm.set_code_memory_limit(0).is_err());
    vm.set_code_memory_limit(2).unwrap();
    vm.load_binary(&image).unwrap();
    assert_eq!(vm.instructions.len(), 2);
    assert!(vm.set_code_memory_limit(1).is_err());
}

#[test]
fn capacity_query_and_bulk_upload_obey_the_same_non_bank_aligned_budget() {
    let mut vm = VM::with_memory_size(65, 520);
    vm.set_code_memory_limit(131).unwrap();
    assert!(vm.instructions.is_empty());
    assert_eq!(
        submit(&mut vm, MEMORY_INFO, &[1, 8, 0, 0, 0, 0, 0, 0]),
        STATUS_OK
    );
    assert_eq!(&vm.memory[40..48], &[1, 8, 520, 0, 131, 0, 65, 0]);
    assert!(vm.instructions.is_empty());
    vm.memory[200..204].copy_from_slice(&[0x42, 0, 0, 0]);
    let transfer = [1, 12, 3, 5, 2, 0, 1, 0, CODE_WORDS, 0, 0, 0];
    assert_eq!(submit(&mut vm, MEMORY_COPY, &transfer), STATUS_OK);
    assert_eq!(vm.instructions.len(), 131);
    let mut outside = transfer;
    outside[5] = 1;
    assert_eq!(submit(&mut vm, MEMORY_COPY, &outside), STATUS_BOUNDS);
    assert_eq!(vm.instructions.len(), 131);
    let mut malformed = [1, 7, 0, 0, 0, 0, 0, 0];
    assert_eq!(
        submit(&mut vm, MEMORY_INFO, &malformed),
        STATUS_BAD_DESCRIPTOR
    );
    malformed[0] = 2;
    malformed[1] = 8;
    assert_eq!(submit(&mut vm, MEMORY_INFO, &malformed), STATUS_VERSION);
}
