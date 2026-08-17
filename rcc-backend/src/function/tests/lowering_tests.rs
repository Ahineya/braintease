use crate::function::internal::FunctionLowering;
use rcc_codegen::{AsmInst, Reg};

#[test]
fn test_prologue_initializes_r13() {
    let mut func = FunctionLowering::new();
    let insts = func.emit_prologue(5);
    
    // Should have LI R13, 1 near the beginning
    // Stack bank is initialized in crt0.asm, not in function prologue
    
    // Should save RA and FP
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::Ra, _, _))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::Fp, _, _))));
    
    // Should allocate space
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sp, Reg::Sp, 5))));
}

#[test]
fn test_epilogue_restores_state() {
    let mut func = FunctionLowering::new();
    func.pressure_manager.init(); // Initialize pressure manager
    let insts = func.emit_epilogue();
    
    // Should restore SP, FP, RA
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Load(Reg::Fp, _, _))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Load(Reg::Ra, _, _))));
    
    // Should return with JALR R0, RAB, RA (restores PCB from RAB)
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Jalr(Reg::R0, Reg::Rab, Reg::Ra))));
    assert!(!insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::Pcb, Reg::Rab, Reg::R0))));
}

#[test]
fn test_fat_pointer_return() {
    let mut func = FunctionLowering::new();
    func.pressure_manager.init();
    
    // Return fat pointer with addr in R5, bank in R6
    let insts = func.emit_return(Some((Reg::A0, Some(Reg::A1))));
    
    // Should move to R3 and R4
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::Rv0, Reg::A0, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::Rv1, Reg::A1, Reg::R0))));
}

#[test]
fn test_scalar_return() {
    let mut func = FunctionLowering::new();
    func.pressure_manager.init();
    
    // Return scalar value in R7
    let insts = func.emit_return(Some((Reg::A2, None)));
    
    // Should move to R3 only
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::Rv0, Reg::A2, Reg::R0))));
    // Should NOT touch R4
    assert!(!insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::Rv1, _, _))));
}

#[test]
fn test_local_access() {
    let mut func = FunctionLowering::new();
    func.pressure_manager.init();
    
    // Test load from local
    let load_insts = func.load_local(3, Reg::A0);
    assert!(load_insts.iter().any(|i| matches!(i, AsmInst::Load(Reg::A0, Reg::Sb, _))));
    
    // Test store to local
    let store_insts = func.store_local(3, Reg::A0);
    assert!(store_insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::A0, Reg::Sb, _))));
}

#[test]
fn test_prologue_with_no_locals() {
    let mut func = FunctionLowering::new();
    let insts = func.emit_prologue(0);
    
    // Should still initialize R13
    // Stack bank is initialized in crt0.asm, not in function prologue
    
    // Should save RA and FP
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::Ra, _, _))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::Fp, _, _))));
    
    // Should NOT allocate space
    assert!(!insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sp, Reg::Sp, 0))));
}

#[test]
fn test_callee_saved_registers_are_preserved() {
    let mut func = FunctionLowering::new();
    
    // Generate prologue
    let prologue = func.emit_prologue(2);
    
    // Check that S0-S3 are saved in prologue
    assert!(prologue.iter().any(|i| matches!(i, AsmInst::Store(Reg::S0, Reg::Sb, Reg::Sp))),
            "S0 should be saved in prologue");
    assert!(prologue.iter().any(|i| matches!(i, AsmInst::Store(Reg::S1, Reg::Sb, Reg::Sp))),
            "S1 should be saved in prologue");
    assert!(prologue.iter().any(|i| matches!(i, AsmInst::Store(Reg::S2, Reg::Sb, Reg::Sp))),
            "S2 should be saved in prologue");
    assert!(prologue.iter().any(|i| matches!(i, AsmInst::Store(Reg::S3, Reg::Sb, Reg::Sp))),
            "S3 should be saved in prologue");
    
    // Generate epilogue
    let epilogue = func.emit_epilogue();
    
    // Check that S0-S3 are restored in epilogue
    assert!(epilogue.iter().any(|i| matches!(i, AsmInst::Load(Reg::S0, Reg::Sb, _))),
            "S0 should be restored in epilogue");
    assert!(epilogue.iter().any(|i| matches!(i, AsmInst::Load(Reg::S1, Reg::Sb, _))),
            "S1 should be restored in epilogue");
    assert!(epilogue.iter().any(|i| matches!(i, AsmInst::Load(Reg::S2, Reg::Sb, _))),
            "S2 should be restored in epilogue");
    assert!(epilogue.iter().any(|i| matches!(i, AsmInst::Load(Reg::S3, Reg::Sb, _))),
            "S3 should be restored in epilogue");
}

#[test]
fn test_callee_saved_registers_restore_order() {
    let mut func = FunctionLowering::new();
    func.emit_prologue(0);
    let epilogue = func.emit_epilogue();
    
    // Find the positions of the restore instructions
    let mut s0_pos = None;
    let mut s1_pos = None;
    let mut s2_pos = None;
    let mut s3_pos = None;
    
    for (i, inst) in epilogue.iter().enumerate() {
        match inst {
            AsmInst::Load(Reg::S0, _, _) => s0_pos = Some(i),
            AsmInst::Load(Reg::S1, _, _) => s1_pos = Some(i),
            AsmInst::Load(Reg::S2, _, _) => s2_pos = Some(i),
            AsmInst::Load(Reg::S3, _, _) => s3_pos = Some(i),
            _ => {}
        }
    }
    
    // All should be found
    assert!(s0_pos.is_some(), "S0 restore not found");
    assert!(s1_pos.is_some(), "S1 restore not found");
    assert!(s2_pos.is_some(), "S2 restore not found");
    assert!(s3_pos.is_some(), "S3 restore not found");
    
    // S3 should be restored before S2, S2 before S1, S1 before S0 (reverse order)
    assert!(s3_pos.unwrap() < s2_pos.unwrap(), "S3 should be restored before S2");
    assert!(s2_pos.unwrap() < s1_pos.unwrap(), "S2 should be restored before S1");
    assert!(s1_pos.unwrap() < s0_pos.unwrap(), "S1 should be restored before S0");
}