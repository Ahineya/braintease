//! Semantic floating intrinsics. Frontends keep arithmetic/conversions typed;
//! only the Ripple target legalizes them to software helper calls.
use crate::ir::{Function, Instruction, IrBuilder, IrType, Module, Value};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Precision {
    F32,
    F64,
}
impl Precision {
    pub fn ty(self) -> IrType {
        match self {
            Self::F32 => IrType::F32,
            Self::F64 => IrType::F64,
        }
    }
    pub fn bits(self) -> IrType {
        match self {
            Self::F32 => IrType::I32,
            Self::F64 => IrType::I64,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::F32 => "f32",
            Self::F64 => "f64",
        }
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FloatOp {
    Add,
    Sub,
    Mul,
    Div,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    FromSI,
    FromUI,
    FromI64,
    FromU64,
    ToSI,
    ToUI,
    ToI64,
    ToU64,
    FromF32,
    FromF64,
}
impl FloatOp {
    pub fn name(self) -> &'static str {
        match self {
            Self::Add => "add",
            Self::Sub => "sub",
            Self::Mul => "mul",
            Self::Div => "div",
            Self::Eq => "eq",
            Self::Ne => "ne",
            Self::Lt => "lt",
            Self::Le => "le",
            Self::Gt => "gt",
            Self::Ge => "ge",
            Self::FromSI => "from_si",
            Self::FromUI => "from_ui",
            Self::FromI64 => "from_i64",
            Self::FromU64 => "from_u64",
            Self::ToSI => "to_si",
            Self::ToUI => "to_ui",
            Self::ToI64 => "to_i64",
            Self::ToU64 => "to_u64",
            Self::FromF32 => "from_f32",
            Self::FromF64 => "from_f64",
        }
    }
    pub fn parse(name: &str) -> Option<Self> {
        [
            Self::Add,
            Self::Sub,
            Self::Mul,
            Self::Div,
            Self::Eq,
            Self::Ne,
            Self::Lt,
            Self::Le,
            Self::Gt,
            Self::Ge,
            Self::FromSI,
            Self::FromUI,
            Self::FromI64,
            Self::FromU64,
            Self::ToSI,
            Self::ToUI,
            Self::ToI64,
            Self::ToU64,
            Self::FromF32,
            Self::FromF64,
        ]
        .into_iter()
        .find(|op| op.name() == name)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FloatIntrinsic {
    pub precision: Precision,
    pub op: FloatOp,
}
impl FloatIntrinsic {
    pub fn from_helper(helper: &str) -> Option<Self> {
        let rest = helper.strip_prefix("__rcc_")?;
        let (width, op) = rest.split_once('_')?;
        Self::parse_parts(width, op)
    }
    pub fn parse(name: &str) -> Option<Self> {
        let rest = name.strip_prefix("float.")?;
        let (width, op) = rest.split_once('.')?;
        Self::parse_parts(width, op)
    }
    fn parse_parts(width: &str, op: &str) -> Option<Self> {
        let precision = match width {
            "f32" => Precision::F32,
            "f64" => Precision::F64,
            _ => return None,
        };
        let op = FloatOp::parse(op)?;
        if matches!(
            (precision, op),
            (Precision::F32, FloatOp::FromF32) | (Precision::F64, FloatOp::FromF64)
        ) {
            return None;
        }
        Some(Self { precision, op })
    }
    pub fn name(self) -> String {
        format!("float.{}.{}", self.precision.name(), self.op.name())
    }
    pub fn helper(self) -> String {
        format!("__rcc_{}_{}", self.precision.name(), self.op.name())
    }
    pub fn params(self) -> Vec<IrType> {
        use FloatOp::*;
        match self.op {
            FromSI | FromUI => vec![IrType::I32],
            FromI64 | FromU64 => vec![IrType::I64],
            FromF32 => vec![IrType::F32],
            FromF64 => vec![IrType::F64],
            ToSI | ToUI | ToI64 | ToU64 => vec![self.precision.ty()],
            _ => vec![self.precision.ty(); 2],
        }
    }
    pub fn result(self) -> IrType {
        use FloatOp::*;
        match self.op {
            Eq | Ne | Lt | Le | Gt | Ge => IrType::I16,
            ToSI | ToUI => IrType::I32,
            ToI64 | ToU64 => IrType::I64,
            _ => self.precision.ty(),
        }
    }
    pub fn validate(self, args: &[Value], result: &IrType) -> Result<(), String> {
        if args.len() != self.params().len() || *result != self.result() {
            return Err(format!(
                "invalid {} signature: expected {} operands and result {}",
                self.name(),
                self.params().len(),
                self.result()
            ));
        }
        Ok(())
    }
}

/// Bridge existing memory/ABI bit representations into semantic float values.
/// Reinterpretation is explicit in IR, separate from numeric conversion.
pub fn build_from_helper(
    builder: &mut IrBuilder,
    helper: &str,
    args: Vec<Value>,
    stored_result: IrType,
) -> Result<Option<u32>, String> {
    let spec = FloatIntrinsic::from_helper(helper)
        .ok_or_else(|| format!("unknown floating operation {helper}"))?;
    spec.validate(&args, &spec.result())?;
    let mut values = Vec::new();
    for (arg, ty) in args.into_iter().zip(spec.params()) {
        let value = if ty.is_fp() {
            let name = if ty.is_f32() {
                "float.value.f32"
            } else {
                "float.value.f64"
            };
            Value::Temp(builder.build_intrinsic(name, vec![arg], ty)?.unwrap())
        } else {
            arg
        };
        values.push(value);
    }
    let value = builder
        .build_intrinsic(&spec.name(), values, spec.result())?
        .unwrap();
    if stored_result == spec.result() {
        return Ok(Some(value));
    }
    if spec.result().is_fp() && stored_result == spec.precision.bits() {
        return builder.build_intrinsic(
            &format!("float.bits.{}", spec.precision.name()),
            vec![Value::Temp(value)],
            stored_result,
        );
    }
    Err(format!("{} cannot produce {stored_result}", spec.name()))
}

/// Target legalization happens before Ripple optimizations and ABI lowering.
pub fn lower_to_softfloat(module: &mut Module) -> Result<(), String> {
    let mut declarations = Vec::new();
    for function in &mut module.functions {
        for instruction in function.blocks.iter_mut().flat_map(|b| &mut b.instructions) {
            let Instruction::Intrinsic {
                result,
                intrinsic,
                args,
                result_type,
            } = instruction
            else {
                continue;
            };
            if let Some(spec) = FloatIntrinsic::parse(intrinsic) {
                spec.validate(args, result_type)?;
                let helper = spec.helper();
                declarations.push(spec);
                *instruction = Instruction::Call {
                    result: *result,
                    function: Value::Global(helper),
                    args: args.clone(),
                    result_type: result_type.clone(),
                    stack_args_from: None,
                };
            } else if let Some(bits) = reinterpret_type(intrinsic) {
                let expected = reinterpret_result(intrinsic).unwrap();
                if args.len() != 1 || result.is_none() || *result_type != expected {
                    return Err(format!("invalid {intrinsic} signature"));
                }
                // Floating values have identical storage bits at the Ripple ABI.
                *instruction = Instruction::Binary {
                    result: result.unwrap(),
                    op: crate::ir::IrBinaryOp::Add,
                    lhs: args[0].clone(),
                    rhs: Value::Constant(0),
                    result_type: bits,
                };
            } else if intrinsic.starts_with("float.") {
                return Err(format!("unknown floating intrinsic {intrinsic}"));
            }
        }
    }
    for spec in declarations {
        let name = spec.helper();
        if module.get_function(&name).is_none() {
            let mut declaration = Function::new(name, spec.result());
            declaration.is_external = true;
            declaration.parameters = spec
                .params()
                .into_iter()
                .enumerate()
                .map(|(i, t)| (i as u32, t))
                .collect();
            module.add_function(declaration);
        }
    }
    Ok(())
}

pub fn reinterpret_type(name: &str) -> Option<IrType> {
    match name {
        "float.value.f32" | "float.bits.f32" => Some(IrType::I32),
        "float.value.f64" | "float.bits.f64" => Some(IrType::I64),
        _ => None,
    }
}

pub fn reinterpret_result(name: &str) -> Option<IrType> {
    match name {
        "float.value.f32" => Some(IrType::F32),
        "float.value.f64" => Some(IrType::F64),
        "float.bits.f32" => Some(IrType::I32),
        "float.bits.f64" => Some(IrType::I64),
        _ => None,
    }
}
