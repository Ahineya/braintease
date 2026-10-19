use super::*;
use rcc_ir::float::{FloatIntrinsic, FloatOp as Op, Precision};
#[test]
fn typed_float_operations_conversions_and_ieee_edges() {
    let mut functions = Vec::new();
    let mut cases = Vec::new();
    for precision in [Precision::F32, Precision::F64] {
        let bits = |n: f64| {
            if precision == Precision::F32 {
                u64::from((n as f32).to_bits())
            } else {
                n.to_bits()
            }
        };
        for (op, args, expected) in [
            (Op::Add, vec![bits(1.5), bits(2.0)], bits(3.5)),
            (Op::Sub, vec![bits(1.0), bits(3.0)], bits(-2.0)),
            (Op::Mul, vec![bits(-2.0), bits(4.0)], bits(-8.0)),
            (Op::Div, vec![bits(1.0), bits(0.0)], bits(f64::INFINITY)),
            (Op::Eq, vec![bits(-0.0), bits(0.0)], 1),
            (Op::Ne, vec![bits(f64::NAN), bits(f64::NAN)], 1),
            (Op::Lt, vec![bits(f64::NAN), bits(1.0)], 0),
            (Op::Le, vec![bits(-1.0), bits(-1.0)], 1),
            (Op::Gt, vec![bits(3.0), bits(2.0)], 1),
            (Op::Ge, vec![bits(f64::NAN), bits(0.0)], 0),
            (
                Op::FromSI,
                vec![u64::from((-12345i32) as u32)],
                bits(-12345.0),
            ),
            (Op::FromUI, vec![u32::MAX as u64], bits(u32::MAX as f64)),
            (Op::FromI64, vec![i64::MIN as u64], bits(i64::MIN as f64)),
            (Op::FromU64, vec![u64::MAX], bits(u64::MAX as f64)),
            (Op::ToSI, vec![bits(f64::INFINITY)], i32::MAX as u64),
            (Op::ToUI, vec![bits(-1.0)], 0),
            (Op::ToI64, vec![bits(f64::NEG_INFINITY)], i64::MIN as u64),
            (Op::ToU64, vec![bits(f64::INFINITY)], u64::MAX),
        ] {
            let spec = FloatIntrinsic { precision, op };
            let name = format!("{}_{}", precision.name(), op.name());
            let mut f = Function::new(name.clone(), spec.result());
            for (id, ty) in spec.params().into_iter().enumerate() {
                f.add_parameter(id as u32, ty);
            }
            f.add_block(block(
                0,
                vec![
                    Instruction::Intrinsic {
                        result: Some(20),
                        intrinsic: spec.name(),
                        args: (0..args.len() as u32).map(Value::Temp).collect(),
                        result_type: spec.result(),
                    },
                    Instruction::Return(Some(Value::Temp(20))),
                ],
            ));
            cases.push(case(&name, &args, expected));
            if matches!(op, Op::ToSI | Op::ToUI | Op::ToI64 | Op::ToU64) {
                cases.push(case(&name, &[bits(f64::NAN)], 0));
            }
            functions.push(f);
        }
    }
    for (name, ty, input, expected) in [
        ("neg32", IrType::F32, 0u64, u64::from((-0.0f32).to_bits())),
        ("neg64", IrType::F64, 1.5f64.to_bits(), (-1.5f64).to_bits()),
    ] {
        let mut f = Function::new(name.into(), ty.clone());
        f.add_block(block(
            0,
            vec![
                Instruction::Unary {
                    result: 0,
                    op: IrUnaryOp::Neg,
                    operand: Value::Constant(input as i64),
                    result_type: ty,
                },
                Instruction::Return(Some(Value::Temp(0))),
            ],
        ));
        functions.push(f);
        cases.push(case(name, &[], expected));
    }
    let exports = functions.iter().map(|f| f.name.clone()).collect();
    run(
        &compile(&[module(functions)], &Options { exports }).unwrap(),
        cases,
    );
}
#[test]
fn real_c_float_memory_rounding_and_integer_casts_use_wasm_arithmetic() {
    let m = c(r#"
        double twice(double x){return x*2.0;}
        int main(void){
            volatile float a=16777216.0f,b=1.0f;
            double values[2]={1.25,2.5};
            if(a+b!=a)return 1;
            if(twice(values[0])+values[1]!=5.0)return 2;
            if((long)twice(21.9)!=43L)return 3;
            return 42;
        }
    "#);
    run(
        &compile(&[m], &Options::default()).unwrap(),
        vec![case("main", &[], 42)],
    );
}
