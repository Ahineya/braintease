use super::{emit_module, parse_module};
use crate::ir::{
    AsmOperandIR, BasicBlock, FatPointer, Function, GlobalVariable, Instruction, IrBinaryOp,
    IrType, IrUnaryOp, Linkage, Module, Value,
};
use crate::BankTag;
use rcc_common::SourceLocation;

fn round_trip(module: &Module) {
    let text = emit_module(module);
    let parsed = parse_module(&text).unwrap_or_else(|err| panic!("{err}\n{text}"));
    assert_eq!(&parsed, module, "{text}");
    assert_eq!(emit_module(&parsed), text);
}

fn small_module() -> Module {
    let mut module = Module::new("demo".into());
    module.type_definitions.insert(
        "Point".into(),
        IrType::Struct {
            name: Some("Point".into()),
            fields: vec![IrType::I16, IrType::I16],
            packed: false,
        },
    );
    module.internal_functions.push("hidden".into());
    module.globals.push(GlobalVariable {
        name: "g".into(),
        var_type: IrType::I16,
        is_constant: true,
        initializer: Some(Value::Constant(7)),
        linkage: Linkage::External,
        symbol_id: Some(3),
    });
    module.globals.push(GlobalVariable {
        name: "e".into(),
        var_type: IrType::FatPtr(Box::new(IrType::I16)),
        is_constant: false,
        initializer: None,
        linkage: Linkage::Declaration,
        symbol_id: None,
    });
    let mut function = Function::new("main".into(), IrType::I16);
    function.parameters.push((0, IrType::I16));
    function.symbol_id = Some(0);
    function.source_location = Some(SourceLocation::new("demo.c", 4, 1));
    let mut entry = BasicBlock::new(0);
    entry.is_loop_condition = true;
    entry.predecessors = vec![2, 0];
    entry.successors = vec![1];
    entry.instructions.push(Instruction::Binary {
        result: 1,
        op: IrBinaryOp::Add,
        lhs: Value::Temp(0),
        rhs: Value::Constant(-1),
        result_type: IrType::I16,
    });
    entry.instructions.push(Instruction::Return(Some(Value::Temp(1))));
    let mut back = BasicBlock::new(1);
    back.predecessors = vec![0];
    back.instructions.push(Instruction::Branch(0));
    function.blocks.push(entry);
    function.blocks.push(back);
    module.add_function(function);
    module
}

#[test]
fn printed_module_matches_the_text_format() {
    let expected = "\
module demo {
  type Point = struct Point { i16, i16 }
  internal function @hidden
  @g = external constant i16 = 7 symbol 3
  @e = declaration i16*
  function i16 @main(i16 %0) symbol 0 loc demo.c 4 1 {
    block 0 loop preds 2, 0 succs 1:
      %1 = add i16 %0, -1
      ret %1
    block 1 preds 0:
      br label 0
  }
}
";
    assert_eq!(emit_module(&small_module()), expected);
    round_trip(&small_module());
}

#[test]
fn comments_and_blank_lines_do_not_change_the_module() {
    let text = "\
; leading comment
module demo {

  type Point = struct Point { i16, i16 } ; field layout
  internal function @hidden
  @g = external constant i16 = 7 symbol 3
  @e = declaration i16*

  function i16 @main(i16 %0) symbol 0 loc demo.c 4 1 {
    block 0 loop preds 2, 0 succs 1:
      %1 = add i16 %0, -1
      ret %1
    block 1 preds 0:
      br label 0
  }
}
";
    assert_eq!(parse_module(text).unwrap(), small_module());
}

#[test]
fn every_ir_construct_round_trips() {
    let mut module = Module::new("has space".into());
    module.type_definitions.insert(
        "i16".into(),
        IrType::Struct {
            name: Some("i16".into()),
            fields: vec![IrType::I8],
            packed: true,
        },
    );
    module.type_definitions.insert(
        "Nested".into(),
        IrType::Function {
            return_type: Box::new(IrType::FatPtr(Box::new(IrType::I8))),
            param_types: vec![
                IrType::Array { size: 3, element_type: Box::new(IrType::I32) },
                IrType::Label,
                IrType::Void,
                IrType::I1,
                IrType::I64,
                IrType::F32,
                IrType::F64,
                IrType::Struct { name: None, fields: vec![IrType::I16], packed: false },
            ],
            is_vararg: true,
        },
    );
    module.internal_functions = vec!["hidden".into(), "also space".into()];
    for (name, linkage, constant, init, symbol) in [
        ("g", Linkage::External, true, Some(Value::Constant(i64::MAX)), Some(0)),
        ("u", Linkage::Internal, false, Some(Value::Undef), None),
        ("p", Linkage::Private, false, None, Some(u32::MAX)),
        ("d", Linkage::Declaration, false, None, None),
    ] {
        module.globals.push(GlobalVariable {
            name: name.into(),
            var_type: IrType::FatPtr(Box::new(IrType::I16)),
            is_constant: constant,
            initializer: init,
            linkage,
            symbol_id: symbol,
        });
    }

    let mut external = Function::new("puts".into(), IrType::I16);
    external.is_external = true;
    external.is_vararg = true;
    external.symbol_id = Some(1);
    external.source_location = Some(SourceLocation::new("dir/libc.h", 1, 1));
    module.functions.push(external);

    let mut body = Function::new("work".into(), IrType::I32);
    body.parameters.push((0, IrType::I16));
    body.parameters.push((7, IrType::FatPtr(Box::new(IrType::I8))));
    body.source_location = Some(SourceLocation::new("a\"b\n", 9, 2));

    let banks = [
        BankTag::Global,
        BankTag::Stack,
        BankTag::Heap(0),
        BankTag::Heap(u16::MAX),
        BankTag::Unknown,
        BankTag::Mixed,
        BankTag::Null,
    ];
    let mut values = vec![
        Value::Temp(0),
        Value::Temp(u32::MAX),
        Value::Constant(0),
        Value::Constant(-1),
        Value::Constant(i64::MIN),
        Value::Constant(i64::MAX),
        Value::Global("g".into()),
        Value::Global("has space".into()),
        Value::Function("puts".into()),
        Value::Function("x".into()),
        Value::Undef,
        Value::AddressOffset { base: Box::new(Value::Global("g".into())), cells: -4 },
        Value::ConstantArray(vec![1, -2, i64::MIN]),
        Value::ConstantAggregate(vec![]),
        Value::ConstantArray(vec![]),
    ];
    for bank in banks {
        values.push(Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(1)),
            bank,
        }));
    }
    values.push(Value::ConstantAggregate(values.clone()));

    let mut instructions = Vec::new();
    let mut next = 1u32;
    for op in [
        IrBinaryOp::Add, IrBinaryOp::Sub, IrBinaryOp::Mul, IrBinaryOp::SDiv, IrBinaryOp::UDiv,
        IrBinaryOp::SRem, IrBinaryOp::URem, IrBinaryOp::And, IrBinaryOp::Or, IrBinaryOp::Xor,
        IrBinaryOp::Shl, IrBinaryOp::LShr, IrBinaryOp::AShr, IrBinaryOp::Eq, IrBinaryOp::Ne,
        IrBinaryOp::Slt, IrBinaryOp::Sle, IrBinaryOp::Sgt, IrBinaryOp::Sge, IrBinaryOp::Ult,
        IrBinaryOp::Ule, IrBinaryOp::Ugt, IrBinaryOp::Uge,
    ] {
        instructions.push(Instruction::Binary {
            result: next,
            op,
            lhs: Value::Temp(0),
            rhs: Value::Constant(1),
            result_type: IrType::I16,
        });
        next += 1;
    }
    for op in [
        IrUnaryOp::Not, IrUnaryOp::Neg, IrUnaryOp::ZExt, IrUnaryOp::SExt, IrUnaryOp::Trunc,
        IrUnaryOp::PtrToLinear, IrUnaryOp::PtrToInt, IrUnaryOp::IntToPtr,
    ] {
        instructions.push(Instruction::Unary {
            result: next,
            op,
            operand: Value::Temp(0),
            result_type: IrType::I32,
        });
        next += 1;
    }
    instructions.push(Instruction::Load {
        result: next,
        ptr: values[0].clone(),
        result_type: IrType::I16,
        volatile: false,
    });
    next += 1;
    instructions.push(Instruction::Load {
        result: next,
        ptr: Value::Temp(1),
        result_type: IrType::I8,
        volatile: true,
    });
    next += 1;
    instructions.push(Instruction::Store {
        value: Value::Constant(1),
        ptr: Value::Temp(1),
        value_type: IrType::I16,
        volatile: false,
    });
    instructions.push(Instruction::Store {
        value: Value::Constant(2),
        ptr: Value::Temp(1),
        value_type: IrType::I32,
        volatile: true,
    });
    instructions.push(Instruction::GetElementPtr {
        result: next,
        ptr: Value::FatPtr(FatPointer { addr: Box::new(Value::Temp(1)), bank: BankTag::Stack }),
        indices: vec![Value::Constant(0), Value::Temp(2)],
        result_type: IrType::FatPtr(Box::new(IrType::I16)),
        source_element_type: IrType::Struct {
            name: Some("Point".into()),
            fields: vec![IrType::I16, IrType::I16],
            packed: false,
        },
        is_struct_field: true,
    });
    next += 1;
    instructions.push(Instruction::GetElementPtr {
        result: next,
        ptr: Value::Temp(1),
        indices: vec![],
        result_type: IrType::I16,
        source_element_type: IrType::I16,
        is_struct_field: false,
    });
    next += 1;
    instructions.push(Instruction::Alloca {
        result: next,
        alloc_type: IrType::I16,
        count: Some(Value::Constant(4)),
        result_type: IrType::FatPtr(Box::new(IrType::I16)),
    });
    next += 1;
    instructions.push(Instruction::Alloca {
        result: next,
        alloc_type: IrType::I8,
        count: None,
        result_type: IrType::FatPtr(Box::new(IrType::I8)),
    });
    next += 1;
    instructions.push(Instruction::Call {
        result: Some(next),
        function: Value::Function("puts".into()),
        args: values.clone(),
        result_type: IrType::I16,
        stack_args_from: Some(1),
    });
    instructions.push(Instruction::Call {
        result: None,
        function: Value::Global("fp".into()),
        args: vec![],
        result_type: IrType::Void,
        stack_args_from: None,
    });
    instructions.push(Instruction::Intrinsic {
        result: None,
        intrinsic: "llvm.trap".into(),
        args: vec![],
        result_type: IrType::Void,
    });
    instructions.push(Instruction::Intrinsic {
        result: Some(next),
        intrinsic: "needs quotes".into(),
        args: vec![Value::Undef],
        result_type: IrType::I32,
    });
    instructions.push(Instruction::Phi {
        result: next,
        incoming: vec![(Value::Temp(0), 2), (Value::Constant(1), 0)],
        result_type: IrType::I16,
    });
    instructions.push(Instruction::Phi {
        result: next,
        incoming: vec![],
        result_type: IrType::I1,
    });
    instructions.push(Instruction::Cast {
        result: next,
        value: Value::Temp(0),
        target_type: IrType::I64,
    });
    instructions.push(Instruction::Select {
        result: next,
        condition: Value::Temp(1),
        true_value: Value::Constant(1),
        false_value: Value::Constant(0),
        result_type: IrType::I16,
    });
    instructions.push(Instruction::DebugLoc {
        location: SourceLocation::new("dir/a.c", 10, 2),
    });
    instructions.push(Instruction::InlineAsm {
        assembly: "say \"hi\"".into(),
        outputs: vec![AsmOperandIR {
            constraint: "=r".into(),
            value: Value::Temp(0),
            tied_to: Some(0),
        }],
        inputs: vec![
            AsmOperandIR { constraint: "r".into(), value: Value::Temp(1), tied_to: None },
            AsmOperandIR { constraint: "m".into(), value: Value::Global("g".into()), tied_to: Some(1) },
        ],
        clobbers: vec!["r1".into(), "memory".into()],
    });
    instructions.push(Instruction::InlineAsm {
        assembly: "nop".into(),
        outputs: vec![],
        inputs: vec![],
        clobbers: vec![],
    });
    instructions.push(Instruction::Comment("hello\n\"world\"".into()));
    instructions.push(Instruction::Comment(String::new()));
    instructions.push(Instruction::BranchCond {
        condition: Value::Temp(1),
        true_label: 1,
        false_label: 0,
    });

    let mut entry = BasicBlock::new(0);
    entry.instructions = instructions;
    entry.predecessors = vec![1, 0];
    entry.successors = vec![1];
    entry.is_loop_condition = true;
    let mut exit = BasicBlock::new(u32::MAX);
    exit.instructions.push(Instruction::Return(None));
    exit.instructions.push(Instruction::Return(Some(Value::Constant(i64::MIN))));
    body.blocks.push(entry);
    body.blocks.push(exit);
    module.functions.push(body);

    let mut with_block = Function::new("ext".into(), IrType::Void);
    with_block.is_external = true;
    let mut only = BasicBlock::new(3);
    only.instructions.push(Instruction::Comment("kept".into()));
    with_block.blocks.push(only);
    module.functions.push(with_block);

    round_trip(&module);
}

#[test]
fn parse_errors_name_the_location() {
    let err = parse_module("module demo {\n  function i16 @main() {\n    block 0:\n      %1 = nope i16 1\n  }\n}\n").unwrap_err();
    assert_eq!(err.line, 4);
    assert!(err.message.contains("nope"), "{err}");

    let err = parse_module("module \"unterminated\n").unwrap_err();
    assert!(err.message.contains("unterminated string"), "{err}");

    let err = parse_module("define main {\n").unwrap_err();
    assert_eq!(err.line, 1);
    assert!(err.message.contains("module"), "{err}");
}

#[test]
fn missing_initializer_is_distinct_from_undef() {
    let mut module = Module::new("init".into());
    module.globals.push(GlobalVariable {
        name: "bare".into(),
        var_type: IrType::I16,
        is_constant: false,
        initializer: None,
        linkage: Linkage::External,
        symbol_id: None,
    });
    module.globals.push(GlobalVariable {
        name: "undefd".into(),
        var_type: IrType::I16,
        is_constant: false,
        initializer: Some(Value::Undef),
        linkage: Linkage::External,
        symbol_id: None,
    });
    round_trip(&module);
    let text = emit_module(&module);
    assert!(text.contains("@bare = external i16\n"), "{text}");
    assert!(text.contains("@undefd = external i16 = undef\n"), "{text}");
}
