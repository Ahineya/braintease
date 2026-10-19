//! Scalar carriers and function-local state. CFG/phi lowering lives in cfg.rs.
use rcc_ir::ir::{Function, Instruction as I, IrType, IrUnaryOp, Value};
use std::collections::BTreeMap;
use wasm_encoder::{Function as Body, Instruction as W, ValType};

pub(crate) fn width(ty: &IrType) -> Result<u32, String> {
    Ok(match ty {
        IrType::I1 => 1,
        IrType::I8 => 8,
        IrType::I16 => 16,
        IrType::I32 | IrType::F32 | IrType::FatPtr(_) => 32,
        IrType::I64 | IrType::F64 => 64,
        _ => return Err(format!("Wasm scalar type {ty} is not implemented yet")),
    })
}
pub(crate) fn result_type(i: &I) -> Option<(u32, &IrType)> {
    match i {
        I::Binary {
            result,
            result_type,
            ..
        }
        | I::Unary {
            result,
            result_type,
            ..
        }
        | I::Load {
            result,
            result_type,
            ..
        }
        | I::Alloca {
            result,
            result_type,
            ..
        }
        | I::GetElementPtr {
            result,
            result_type,
            ..
        }
        | I::Phi {
            result,
            result_type,
            ..
        }
        | I::Select {
            result,
            result_type,
            ..
        } => Some((*result, result_type)),
        I::Cast {
            result,
            target_type,
            ..
        } => Some((*result, target_type)),
        I::Call {
            result: Some(result),
            result_type,
            ..
        }
        | I::Intrinsic {
            result: Some(result),
            result_type,
            ..
        } => Some((*result, result_type)),
        _ => None,
    }
}

pub(crate) struct Context<'a> {
    pub functions: &'a BTreeMap<String, (u32, &'a Function)>,
    pub codes: &'a BTreeMap<String, u64>,
    pub layout: &'a crate::globals::Layout,
    pub address: u32,
    pub gep: u32,
    pub signatures: &'a BTreeMap<usize, u32>,
}
pub(crate) struct Cg<'a> {
    pub body: Body,
    pub function: &'a Function,
    pub context: &'a Context<'a>,
    pub frame: u32,
    pub mark: u32,
    pub allocas: BTreeMap<u32, u64>,
    pub locals: BTreeMap<u32, (u32, IrType)>,
    // Arithmetic scratch values; all operations are i64 carriers.
    pub scratch: u32,
    pub pc: u32,
    pub labels: Vec<crate::cfg::Label>,
    pub block_indices: BTreeMap<u32, usize>,
    pub dispatch: bool,
}
impl Cg<'_> {
    pub fn ins(&mut self, i: W<'_>) {
        self.body.instruction(&i);
    }
    pub fn mask(&mut self, bits: u32) {
        if bits < 64 {
            self.ins(W::I64Const(((1u64 << bits) - 1) as i64));
            self.ins(W::I64And);
        }
    }
    pub fn signed(&mut self, bits: u32) {
        if bits < 64 {
            self.ins(W::I64Const(i64::from(64 - bits)));
            self.ins(W::I64Shl);
            self.ins(W::I64Const(i64::from(64 - bits)));
            self.ins(W::I64ShrS);
        }
    }
    pub fn width_of(&self, v: &Value) -> Result<u32, String> {
        match v {
            Value::Temp(id) => width(
                &self
                    .locals
                    .get(id)
                    .ok_or_else(|| format!("undefined IR temporary %{id}"))?
                    .1,
            ),
            Value::Constant(_) | Value::Undef => Ok(16),
            Value::FatPtr(_)
            | Value::Global(_)
            | Value::Function(_)
            | Value::AddressOffset { .. } => Ok(32),
            _ => Err(format!("Wasm scalar value is not implemented: {v}")),
        }
    }
    pub fn value(&mut self, v: &Value) -> Result<(), String> {
        match v {
            Value::Temp(id) => {
                let local = self
                    .locals
                    .get(id)
                    .ok_or_else(|| format!("undefined IR temporary %{id}"))?
                    .0;
                self.ins(W::LocalGet(local));
            }
            Value::Constant(n) => self.ins(W::I64Const(*n)),
            Value::Undef => self.ins(W::I64Const(0)),
            Value::Global(_)
            | Value::Function(_)
            | Value::FatPtr(_)
            | Value::AddressOffset { .. } => {
                if let Value::FatPtr(p) = v {
                    if !matches!(p.addr.as_ref(), Value::Constant(_)) {
                        return self.value(&p.addr);
                    }
                }
                if let Value::AddressOffset { base, cells } = v {
                    self.value(base)?;
                    self.ins(W::I64Const(*cells));
                    self.ins(W::Call(self.context.gep));
                } else {
                    let constant = self.context.layout.constant(v, self.context.codes)?;
                    self.ins(W::I64Const(constant as i64));
                }
            }
            _ => return Err(format!("Wasm scalar value is not implemented: {v}")),
        }
        Ok(())
    }
    pub fn define(&mut self, id: u32) -> Result<(), String> {
        let (local, ty) = self
            .locals
            .get(&id)
            .ok_or_else(|| format!("undeclared IR result %{id}"))?;
        let local = *local;
        let bits = width(ty)?;
        self.mask(bits);
        self.ins(W::LocalSet(local));
        Ok(())
    }
    pub fn instruction(&mut self, i: &I) -> Result<(), String> {
        match i {
            I::Binary {
                result,
                op,
                lhs,
                rhs,
                result_type,
            } => {
                self.binary(*op, lhs, rhs, result_type)?;
                self.define(*result)?;
            }
            I::Unary {
                result,
                op,
                operand,
                result_type,
            } => {
                if result_type.is_fp() {
                    if *op != IrUnaryOp::Neg {
                        return Err("floating unary operations require numeric negation or float intrinsics".into());
                    }
                    if let Value::Temp(id) = operand {
                        if self.locals.get(id).map(|(_, ty)| ty) != Some(result_type) {
                            return Err("floating negation operand type mismatch".into());
                        }
                    }
                    self.value(operand)?;
                    if *result_type == IrType::F32 {
                        self.ins(W::I32WrapI64);
                        self.ins(W::F32ReinterpretI32);
                        self.ins(W::F32Neg);
                        self.ins(W::I32ReinterpretF32);
                        self.ins(W::I64ExtendI32U);
                    } else {
                        self.ins(W::F64ReinterpretI64);
                        self.ins(W::F64Neg);
                        self.ins(W::I64ReinterpretF64);
                    }
                    self.define(*result)?;
                    return Ok(());
                }
                self.value(operand)?;
                match op {
                    IrUnaryOp::Not => {
                        self.ins(W::I64Const(-1));
                        self.ins(W::I64Xor);
                    }
                    IrUnaryOp::Neg => {
                        self.ins(W::I64Const(-1));
                        self.ins(W::I64Mul);
                    }
                    IrUnaryOp::SExt => self.signed(self.width_of(operand)?),
                    IrUnaryOp::ZExt => self.mask(self.width_of(operand)?),
                    IrUnaryOp::Trunc | IrUnaryOp::IntToPtr | IrUnaryOp::PtrToInt => {}
                    IrUnaryOp::PtrToLinear => {
                        self.ins(W::I64Const(0));
                        self.ins(W::Call(self.context.address));
                        self.ins(W::I64Const(2));
                        self.ins(W::I64DivU);
                    }
                }
                self.define(*result)?;
            }
            I::Cast { result, value, .. } => {
                self.value(value)?;
                self.define(*result)?;
            }
            I::Select {
                result,
                condition,
                true_value,
                false_value,
                ..
            } => {
                self.value(true_value)?;
                self.value(false_value)?;
                self.value(condition)?;
                self.ins(W::I64Eqz);
                self.ins(W::I32Eqz);
                self.ins(W::Select);
                self.define(*result)?;
            }
            I::Call { .. } => self.call(i)?,
            I::Load { .. } | I::Store { .. } | I::GetElementPtr { .. } | I::Alloca { .. } => {
                self.memory(i)?
            }
            I::Intrinsic {
                result,
                intrinsic,
                args,
                result_type,
            } => {
                self.floating(intrinsic, args, result_type)?;
                self.define(result.ok_or("floating intrinsic requires a result")?)?;
            }
            I::Phi { .. } | I::DebugLoc { .. } | I::Comment(_) => {}
            _ => return Err(format!("Wasm instruction is not implemented yet: {i}")),
        }
        Ok(())
    }
}

pub(crate) fn emit<'a>(
    function: &'a Function,
    context: &'a Context<'a>,
) -> Result<(Body, bool), String> {
    let mut locals = BTreeMap::new();
    for (id, ty) in &function.parameters {
        width(ty)?;
        if locals.contains_key(id) {
            return Err(format!("duplicate parameter %{id}"));
        }
        locals.insert(*id, (locals.len() as u32, ty.clone()));
    }
    // Keep arbitrary IR temp IDs compact in the Wasm local index space.
    for i in function.blocks.iter().flat_map(|b| &b.instructions) {
        if let Some((id, ty)) = result_type(i) {
            width(ty).map_err(|e| format!("{}: {e}", function.name))?;
            if locals.contains_key(&id) {
                return Err(format!("{}: duplicate IR temporary %{id}", function.name));
            }
            let ty = if matches!(i, I::GetElementPtr { .. }) && !ty.is_pointer() {
                IrType::FatPtr(Box::new(ty.clone()))
            } else {
                ty.clone()
            };
            locals.insert(id, (locals.len() as u32, ty));
        }
    }
    let scratch = locals.len() as u32;
    let pc = scratch + 4;
    let mut cg = Cg {
        body: Body::new([
            (scratch - function.parameters.len() as u32 + 4, ValType::I64),
            (1, ValType::I32),
        ]),
        function,
        context,
        frame: scratch + 2,
        mark: scratch + 3,
        allocas: BTreeMap::new(),
        locals,
        scratch,
        pc,
        labels: Vec::new(),
        block_indices: BTreeMap::new(),
        dispatch: false,
    };
    for (id, _) in &function.parameters {
        cg.value(&Value::Temp(*id))?;
        cg.define(*id)?;
    }
    cg.prepare_frame()?;
    let structured = crate::cfg::emit(&mut cg).map_err(|e| format!("{}: {e}", function.name))?;
    cg.ins(W::Unreachable);
    cg.ins(W::End);
    Ok((cg.body, structured))
}
