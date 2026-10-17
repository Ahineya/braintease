use rcc_frontend::Frontend;
use rcc_ir::ir::{Instruction, IrType};

#[test]
fn void_conditionals_have_control_flow_without_void_storage() {
    let module = Frontend::compile_to_ir(
        "void yes(void); void no(void);
         void choose(int c, int d) {
             c ? yes() : no();
             c ? (void)0 : yes();
             c ? (d ? yes() : no()) : (void)0;
         }",
        "void_conditional.c",
    )
    .unwrap();
    for instruction in module
        .functions
        .iter()
        .flat_map(|f| &f.blocks)
        .flat_map(|b| &b.instructions)
    {
        match instruction {
            Instruction::Alloca { alloc_type, .. } => assert_ne!(*alloc_type, IrType::Void),
            Instruction::Load { result_type, .. } => assert_ne!(*result_type, IrType::Void),
            Instruction::Store { value_type, .. } => assert_ne!(*value_type, IrType::Void),
            _ => {}
        }
    }
}
