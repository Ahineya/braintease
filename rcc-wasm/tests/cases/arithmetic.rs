use super::*;

#[test]
fn integer_widths_and_trapping_edges_match_wrapping_reference() {
    use IrBinaryOp::*;
    let ops = [
        Add, Sub, Mul, UDiv, SDiv, URem, SRem, And, Or, Xor, Shl, LShr, AShr, Eq, Ne, Slt, Sle,
        Sgt, Sge, Ult, Ule, Ugt, Uge,
    ];
    let mut functions = Vec::new();
    let mut cases = Vec::new();
    let mut random = 0xabcde123456789u64;
    for (bits, ty) in [
        (1, IrType::I1),
        (8, IrType::I8),
        (16, IrType::I16),
        (32, IrType::I32),
        (64, IrType::I64),
    ] {
        let mask = u64::MAX >> (64 - bits);
        let sign = |v: u64| ((v << (64 - bits)) as i64) >> (64 - bits);
        for op in ops {
            let name = format!("op{bits}_{op}");
            let comparison = matches!(op, Eq | Ne | Slt | Sle | Sgt | Sge | Ult | Ule | Ugt | Uge);
            let result_type = if comparison { IrType::I1 } else { ty.clone() };
            let mut f = Function::new(name.clone(), result_type.clone());
            f.add_parameter(0, ty.clone());
            f.add_parameter(1, ty.clone());
            f.add_block(block(
                0,
                vec![
                    Instruction::Binary {
                        result: 20,
                        op,
                        lhs: Value::Temp(0),
                        rhs: Value::Temp(1),
                        result_type,
                    },
                    Instruction::Return(Some(Value::Temp(20))),
                ],
            ));
            functions.push(f);
            let mut pairs = vec![
                (0, 0),
                (1, 0),
                (mask, 0),
                (mask, 1),
                (1u64 << (bits - 1), mask),
                (mask, mask),
                (mask, bits as u64),
                (mask, 64),
                (1, u64::MAX),
            ];
            for _ in 0..24 {
                random ^= random << 13;
                random ^= random >> 7;
                random ^= random << 17;
                let a = random;
                random = random.wrapping_mul(6364136223846793005).wrapping_add(1);
                pairs.push((a, random));
            }
            for (input_a, input_b) in pairs {
                let a = input_a & mask;
                let b = input_b & mask;
                let sa = sign(a);
                let sb = sign(b);
                let shift = (b & (bits - 1) as u64) as u32;
                let expected = match op {
                    Add => a.wrapping_add(b),
                    Sub => a.wrapping_sub(b),
                    Mul => a.wrapping_mul(b),
                    UDiv => {
                        if b == 0 {
                            0
                        } else {
                            a / b
                        }
                    }
                    URem => {
                        if b == 0 {
                            0
                        } else {
                            a % b
                        }
                    }
                    SDiv => {
                        if sb == 0 {
                            0
                        } else {
                            sa.wrapping_div(sb) as u64
                        }
                    }
                    SRem => {
                        if sb == 0 {
                            0
                        } else {
                            sa.wrapping_rem(sb) as u64
                        }
                    }
                    And => a & b,
                    Or => a | b,
                    Xor => a ^ b,
                    Shl => a.wrapping_shl(shift),
                    LShr => a >> shift,
                    AShr => (sa >> shift) as u64,
                    Eq => (a == b) as u64,
                    Ne => (a != b) as u64,
                    Slt => (sa < sb) as u64,
                    Sle => (sa <= sb) as u64,
                    Sgt => (sa > sb) as u64,
                    Sge => (sa >= sb) as u64,
                    Ult => (a < b) as u64,
                    Ule => (a <= b) as u64,
                    Ugt => (a > b) as u64,
                    Uge => (a >= b) as u64,
                } & if comparison { 1 } else { mask };
                cases.push(case(&name, &[input_a, input_b], expected));
            }
        }
    }
    let exports = functions.iter().map(|f| f.name.clone()).collect();
    let artifact = compile(&[module(functions)], &Options { exports }).unwrap();
    assert_eq!(artifact.statistics.dispatch_functions, 0);
    run(&artifact, cases);
}

#[test]
fn unary_narrowing_void_calls_and_non_boolean_conditions() {
    let m = parse(
        r#"
function void @noop() { block 0: ret void }
function i64 @extend(i8 %0) {
 block 0:
  %1 = zext i64 %0
  ret %1
}
function i8 @negate(i8 %0) {
 block 0:
  %1 = neg i8 %0
  ret %1
}
function i8 @invert(i8 %0) {
 block 0:
  %1 = not i8 %0
  ret %1
}
function i8 @truncate(i64 %0) {
 block 0:
  %1 = trunc i8 %0
  ret %1
}
function i8 @cast(i64 %0) {
 block 0:
  %1 = cast %0 to i8
  ret %1
}
function i16 @truth(i64 %0) {
 block 0:
  call void func @noop()
  br %0, label 1, label 2
 block 1: ret 1
 block 2: ret 0
}
"#,
    );
    let artifact = compile(
        &[m],
        &options(&[
            "extend", "negate", "invert", "truncate", "cast", "truth", "noop",
        ]),
    )
    .unwrap();
    run(
        &artifact,
        vec![
            case("extend", &[255], 255),
            case("negate", &[1], 255),
            case("invert", &[0], 255),
            case("truncate", &[0x1234], 0x34),
            case("cast", &[0x1234], 0x34),
            case("truth", &[1u64 << 32], 1),
            case("truth", &[0], 0),
            case("noop", &[], 0),
        ],
    );
}
