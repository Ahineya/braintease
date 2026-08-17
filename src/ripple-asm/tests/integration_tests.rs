use ripple_asm::{RippleAssembler, AssemblerOptions, Linker, MacroFormatter, Opcode, Register};
use pretty_assertions::assert_eq;

#[test]
fn test_basic_instructions() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    // Test NOP
    let result = assembler.assemble("NOP").unwrap();
    assert_eq!(result.instructions.len(), 1);
    assert_eq!(result.instructions[0].opcode, Opcode::Nop as u8);
    
    // Test ADD
    let result = assembler.assemble("ADD R3, R4, R5").unwrap();
    assert_eq!(result.instructions.len(), 1);
    assert_eq!(result.instructions[0].opcode, Opcode::Add as u8);
    assert_eq!(result.instructions[0].word1, Register::Ra as u16);
    assert_eq!(result.instructions[0].word2, Register::Rab as u16);
    assert_eq!(result.instructions[0].word3, Register::Rv0 as u16);
    
    // Test LI
    let result = assembler.assemble("LI R3, 42").unwrap();
    assert_eq!(result.instructions.len(), 1);
    assert_eq!(result.instructions[0].opcode, Opcode::Li as u8);
    assert_eq!(result.instructions[0].word1, Register::Ra as u16);
    assert_eq!(result.instructions[0].word2, 42);
    
    // Test HALT
    let result = assembler.assemble("HALT").unwrap();
    assert_eq!(result.instructions.len(), 1);
    assert!(result.instructions[0].is_halt());
}

#[test]
fn test_register_parsing() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    let source = "ADD PC, RA, R0";
    let result = assembler.assemble(source).unwrap();
    
    assert_eq!(result.instructions[0].word1, Register::Pc as u16);
    assert_eq!(result.instructions[0].word2, Register::Ra as u16);
    assert_eq!(result.instructions[0].word3, Register::R0 as u16);
}

#[test]
fn test_labels_and_jumps() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    let source = r#"
        LI R3, 5
        JAL R0, R0, loop
        HALT
    loop:
        ADDI R3, R3, -1
        BNE R3, R0, loop
        JALR R0, R0, RA
    "#;
    
    let result = assembler.assemble(source).unwrap();
    
    assert_eq!(result.instructions.len(), 6);
    assert!(result.labels.contains_key("loop"));
    assert_eq!(result.labels.get("loop").unwrap().offset, 12); // 3 instructions * 4 bytes
}

#[test]
fn test_data_section() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    let source = r#"
.data
hello_msg: .asciiz "Hello"
count: .byte 10, 20, 30
words: .word 0x1234, 0x5678

.code
start:
    LI R3, 0
    HALT
    "#;
    
    let result = assembler.assemble(source).unwrap();
    
    assert_eq!(result.instructions.len(), 2);
    assert!(result.data_labels.contains_key("hello_msg"));
    assert!(result.data_labels.contains_key("count"));
    assert!(result.data_labels.contains_key("words"));
    
    // Check data content
    assert_eq!(&result.data[0..6], b"Hello\0");
}

#[test]
fn test_virtual_instructions() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    // Test MOVE
    let result = assembler.assemble("MOVE R3, R4").unwrap();
    assert_eq!(result.instructions.len(), 1);
    assert_eq!(result.instructions[0].opcode, Opcode::Add as u8);
    assert_eq!(result.instructions[0].word3, Register::R0 as u16);
    
    // Test INC
    let result = assembler.assemble("INC R5").unwrap();
    assert_eq!(result.instructions.len(), 1);
    assert_eq!(result.instructions[0].opcode, Opcode::Addi as u8);
    assert_eq!(result.instructions[0].word3, 1);
    
    // Test DEC
    let result = assembler.assemble("DEC R5").unwrap();
    assert_eq!(result.instructions.len(), 1);
    assert_eq!(result.instructions[0].opcode, Opcode::Addi as u8);
    
    // Test PUSH (expands to 2 instructions)
    let result = assembler.assemble("PUSH R3").unwrap();
    assert_eq!(result.instructions.len(), 2);
    
    // Test POP (expands to 2 instructions)
    let result = assembler.assemble("POP R3").unwrap();
    assert_eq!(result.instructions.len(), 2);

    // RET restores PCB from RAB: JALR R0, RAB, RA
    let result = assembler.assemble("RET").unwrap();
    assert_eq!(result.instructions.len(), 1);
    assert_eq!(result.instructions[0].opcode, Opcode::Jalr as u8);
    assert_eq!(result.instructions[0].word1, Register::R0 as u16);
    assert_eq!(result.instructions[0].word2, Register::Rab as u16);
    assert_eq!(result.instructions[0].word3, Register::Ra as u16);
}

#[test]
fn test_comments() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    let source = r#"
    ; This is a comment
    ADD R3, R4, R5  ; inline comment
    # Hash comment
    SUB R6, R7, R8  // C-style comment
    "#;
    
    let result = assembler.assemble(source).unwrap();
    assert_eq!(result.instructions.len(), 2);
}

#[test]
fn test_case_insensitive() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    let source = "add r3, R4, r5";
    let result = assembler.assemble(source).unwrap();
    
    assert_eq!(result.instructions[0].opcode, Opcode::Add as u8);
    assert_eq!(result.instructions[0].word1, Register::Ra as u16);
    assert_eq!(result.instructions[0].word2, Register::Rab as u16);
    assert_eq!(result.instructions[0].word3, Register::Rv0 as u16);
}

#[test]
fn test_immediate_formats() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    // Decimal
    let result = assembler.assemble("LI R3, 42").unwrap();
    assert_eq!(result.instructions[0].word2, 42);
    
    // Hexadecimal
    let result = assembler.assemble("LI R3, 0xFF").unwrap();
    assert_eq!(result.instructions[0].word2, 0xFF);
    
    // Binary
    let result = assembler.assemble("LI R3, 0b1010").unwrap();
    assert_eq!(result.instructions[0].word2, 0b1010);
}

#[test]
fn test_linking() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    // File 1
    let obj1 = assembler.assemble(r#"
start:
    CALL func
    HALT
    "#).unwrap();
    
    // File 2
    let obj2 = assembler.assemble(r#"
func:
    LI R3, 42
    RET
    "#).unwrap();
    
    let linker = Linker::new(16);
    let linked = linker.link(vec![obj1, obj2]).unwrap();
    
    assert_eq!(linked.instructions.len(), 4);
    assert!(linked.labels.contains_key("start"));
    assert!(linked.labels.contains_key("func"));
    assert_eq!(linked.entry_point, 0);
}

#[test]
fn test_macro_formatter() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    let formatter = MacroFormatter::new();
    
    let source = r#"
.data
msg: .asciiz "Test"

.code
start:
    LI R3, 42
    ADD R3, R3, R4
    HALT
    "#;
    
    let obj = assembler.assemble(source).unwrap();
    let formatted = formatter.format_full_program(
        &obj.instructions,
        Some(&obj.data),
        None,
        Some("Test Program"),
    );
    
    assert!(formatted.contains("// Test Program"));
    assert!(formatted.contains("@prg("));
    assert!(formatted.contains("@OP_LI"));
    assert!(formatted.contains("@OP_ADD"));
    assert!(formatted.contains("@OP_HALT"));
    assert!(formatted.contains("'T'"));
    assert!(formatted.contains("'e'"));
    assert!(formatted.contains("'s'"));
    assert!(formatted.contains("'t'"));
}

#[test]
fn test_hello_world_example() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    let source = r#"
; Hello World Example for Ripple VM
.data
hello_msg:  .asciiz "Hello, Ripple!\n"

.code
start:
    LI R3, 0
    LI R5, 2

print_loop:
    LOAD  R3, R5, 0
    BNE   R3, R0, 2
    HALT
    ADDI  R5, R5, 1  ; inc
    STORE R3, R0, 0  ; print character (I/O at address 0)
    JAL  R0, R0, print_loop
    "#;
    
    let result = assembler.assemble(source);
    assert!(result.is_ok(), "Failed to assemble hello world: {:?}", result);
    
    let obj = result.unwrap();
    assert!(obj.data_labels.contains_key("hello_msg"));
    assert!(obj.labels.contains_key("start"));
    assert!(obj.labels.contains_key("print_loop"));
}

#[test]
fn test_error_handling() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    
    // Unknown instruction
    let result = assembler.assemble("UNKNOWN R3, R4");
    assert!(result.is_err());
    
    // Wrong number of operands
    let result = assembler.assemble("ADD R3");
    assert!(result.is_err());
    
    // Invalid register
    let result = assembler.assemble("ADD R99, R3, R4");
    assert!(result.is_err());
}

#[test]
fn test_jal_label_resolution() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    let linker = Linker::new(16);
    
    let source = r#"
start:
    LI R3, 10
    JAL R0, R0, print_loop
    HALT
print_loop:
    ADDI R3, R3, -1
    BNE R3, R0, print_loop
    JALR R0, R0, RA
    "#;
    
    // Assemble the code
    let obj = assembler.assemble(source).unwrap();
    
    // Link the object file (local JAL targets are remapped to bank+offset)
    let linked = linker.link(vec![obj]).unwrap();
    
    // Verify the JAL instruction (at index 1) has been resolved correctly
    // print_loop is at instruction index 3 (0: LI, 1: JAL, 2: HALT, 3: print_loop)
    let jal_instruction = &linked.instructions[1];
    assert_eq!(jal_instruction.opcode, Opcode::Jal as u8);
    assert_eq!(jal_instruction.word1, 0); // R0
    assert_eq!(jal_instruction.word2, 0); // bank
    assert_eq!(jal_instruction.word3, 3); // in-bank instruction offset
    
    // Also check the BNE instruction (at index 4) has been resolved correctly
    // It should branch back to print_loop (relative offset)
    let bne_instruction = &linked.instructions[4];
    assert_eq!(bne_instruction.opcode, Opcode::Bne as u8);
    // BNE uses relative offset, so it should be -1 (go back 1 instruction from 4 to 3)
    // -1 as u16 in two's complement is 0xFFFF
    assert_eq!(bne_instruction.word3, 0xFFFF);
}

#[test]
fn test_hello_world_with_linking() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    let linker = Linker::new(16);
    
    let source = r#"
; Hello World Example with proper linking
.data
hello_msg:  .asciiz "Hello, Ripple!\n"

.code
start:
    LI R3, 0
    LI R5, 2

print_loop:
    LOAD  R3, R5, 0
    BNE   R3, R0, continue
    HALT
continue:
    ADDI  R5, R5, 1
    STORE R3, R0, 0
    JAL   R0, R0, print_loop
    "#;
    
    // Assemble
    let obj = assembler.assemble(source).unwrap();
    
    // Link
    let linked = linker.link(vec![obj]).unwrap();
    
    // Verify JAL instruction is properly resolved
    // JAL is at instruction index 6 (0: LI, 1: LI, 2: LOAD, 3: BNE, 4: HALT, 5: ADDI, 6: STORE, 7: JAL)
    let jal_instruction = &linked.instructions[7];
    assert_eq!(jal_instruction.opcode, Opcode::Jal as u8);
    assert_eq!(jal_instruction.word3, 2); // Should jump to print_loop at instruction 2
    
    // The BNE instruction at index 3 should have the correct relative offset
    // From instruction 3 to instruction 5 (continue label) is +2 instructions
    let bne_instruction = &linked.instructions[3];
    assert_eq!(bne_instruction.opcode, Opcode::Bne as u8);
    assert_eq!(bne_instruction.word3, 2); // Should branch forward 2 instructions to 'continue'
}

#[test]
fn test_link_packs_objects_into_banks() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    let linker = Linker::new(8);

    let mut src1 = String::from("start:\n");
    for _ in 0..6 {
        src1.push_str("    NOP\n");
    }
    src1.push_str("    CALL func\n    HALT\n");
    let obj1 = assembler.assemble(&src1).unwrap();
    assert_eq!(obj1.instructions.len(), 8);

    let src2 = "func:\n    LI R3, 42\n    RET\n";
    let obj2 = assembler.assemble(src2).unwrap();
    assert_eq!(obj2.instructions.len(), 2);

    // obj1 fills bank 0 exactly; obj2 starts at bank 1 offset 0 (no pad needed).
    let linked = linker.link(vec![obj1, obj2]).unwrap();
    assert_eq!(linked.code_bank_count(), 2);
    assert_eq!(linked.instructions.len(), 10);

    let jal = &linked.instructions[6];
    assert_eq!(jal.opcode, Opcode::Jal as u8);
    assert_eq!(jal.word2, 1);
    assert_eq!(jal.word3, 0);

    let func = linked.labels.get("func").unwrap();
    assert_eq!(func.bank, 1);
    assert_eq!(func.offset, 0);
}

#[test]
fn test_bank_overflow() {
    let mut options = AssemblerOptions::default();
    options.bank_size = 8; // Small bank size to test overflow
    
    let assembler = RippleAssembler::new(options);
    
    let source = r#"
    NOP
    NOP
    NOP
label1:
    NOP
    "#;
    
    let result = assembler.assemble(source).unwrap();
    // After 3 NOPs (3 * 4 = 12 bytes), with bank_size=8, we should be at bank 1, offset 4
    // Actually the label is at the position where the 4th NOP will be placed
    // 1st NOP: bank 0, offset 0-3
    // 2nd NOP: bank 0, offset 4-7  
    // 3rd NOP: bank 1, offset 0-3 (bank overflow)
    // label1 points to 4th NOP: bank 1, offset 4
    assert_eq!(result.labels.get("label1").unwrap().bank, 1);
    assert_eq!(result.labels.get("label1").unwrap().offset, 4);
}