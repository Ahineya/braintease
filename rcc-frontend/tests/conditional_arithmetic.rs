use rcc_frontend::Frontend;
use rcc_ir::ir::{Instruction, IrType};

#[test]
fn conditional_arithmetic_converts_both_operands_to_the_common_type() {
    for (left, right, result, ir_type) in [
        ("int", "long", "long", IrType::I32),
        ("unsigned int", "long", "long", IrType::I32),
        ("long", "unsigned long", "unsigned long", IrType::I32),
        ("long", "long long", "long long", IrType::I64),
        ("int", "float", "float", IrType::F32),
        ("float", "double", "double", IrType::F64),
        ("char", "char", "int", IrType::I16),
    ] {
        for (a, b) in [(left, right), (right, left)] {
            let source = format!("{result} choose(int c, {a} a, {b} b) {{ return c ? a : b; }}");
            let module = Frontend::compile_to_ir(&source, "conditional_arithmetic.c").unwrap();
            let allocations: Vec<_> = module.functions[0]
                .blocks
                .iter()
                .flat_map(|b| &b.instructions)
                .filter_map(|i| match i {
                    Instruction::Alloca { alloc_type, .. } => Some(alloc_type.clone()),
                    _ => None,
                })
                .collect();
            // Parameters precede the conditional's result slot. The slot must
            // have the common width regardless of which operand appears first.
            assert_eq!(allocations.last(), Some(&ir_type), "{source}");
        }
    }
}
