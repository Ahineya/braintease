//! Function/CFG translation. IR temporaries become Cranelift SSA variables;
//! explicit Phi nodes are assigned simultaneously on dedicated incoming edges.
use super::*;
use cranelift_codegen::ir::{Block, InstBuilder, Value as ClValue};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Variable};
use rcc_ir::ir::{Function, Instruction as I, IrType, Value};
use rcc_ir::BankTag;

pub(super) type PhiAssignments = BTreeMap<u32, Vec<(u32, Vec<(Value, u32)>)>>;

pub(super) struct Cg<'a, 'b> {
    pub b: FunctionBuilder<'a>,
    pub compiler: &'b mut Compiler,
    pub context: ClValue,
    pub vars: BTreeMap<u32, Variable>,
    pub types: BTreeMap<u32, IrType>,
    pub(super) blocks: BTreeMap<u32, Block>,
    pub(super) phis: PhiAssignments,
    pub(super) frame: ClValue,
    pub(super) mark: ClValue,
    pub(super) owns_stack: bool,
    pub(super) allocas: BTreeMap<u32, u64>,
    pub(super) edges: Vec<(Block, u32, u32)>,
    pub(super) va_input: Option<ClValue>,
    pub(super) var_area: u64,
    pub(super) bound_globals: BTreeMap<String, ClValue>,
    pub(super) read_view: Option<super::memory::ReadView>,
    pub(super) read_region: bool,
}
impl Cg<'_, '_> {
    pub(super) fn bind_globals(&mut self, function: &Function) -> Result<(), String> {
        if self.compiler.dynamic.is_none() {
            return Ok(());
        }
        // A process's module addresses cannot change during a continuation.
        // Resolve once at the step entry, outside application loops, and do
        // so again when a suspended frame resumes in its selected process.
        for name in rcc_ir::link::function_symbols(function) {
            if let Some(local) = self.compiler.globals.get(&name).copied() {
                let local = self.k(local as i64);
                let address = self.runtime("rcc_native_image_global", &[local])?;
                self.bound_globals.insert(name, address);
            }
        }
        Ok(())
    }
    pub fn k(&mut self, value: i64) -> ClValue {
        self.b.ins().iconst(types::I64, value)
    }
    pub fn value(&mut self, value: &Value) -> Result<ClValue, String> {
        Ok(match value {
            Value::Constant(value) => self.k(*value),
            Value::Temp(id) => {
                let var = *self
                    .vars
                    .get(id)
                    .ok_or_else(|| format!("undefined IR temporary %{id}"))?;
                self.b.use_var(var)
            }
            Value::Global(name) | Value::Function(name) => {
                if let Some(value) = self.bound_globals.get(name) {
                    return Ok(*value);
                }
                if let Some(n) = self.compiler.globals.get(name).copied() {
                    let n = self.k(n as i64);
                    if self.compiler.dynamic.is_some() {
                        self.runtime("rcc_native_image_global", &[n])?
                    } else {
                        n
                    }
                } else {
                    let code = self.compiler.function_token(name)?;
                    let value = self.k(code as i64);
                    if self.compiler.application {
                        self.runtime("rcc_native_image_token", &[value])?
                    } else {
                        value
                    }
                }
            }
            Value::FatPtr(pointer) => {
                let addr = self.value(&pointer.addr)?;
                if matches!(pointer.addr.as_ref(), Value::Constant(_)) {
                    if self.compiler.dynamic.is_some() && matches!(pointer.bank, BankTag::Global) {
                        let low = self.b.ins().band_imm_u(addr, 65535);
                        let local = self
                            .b
                            .ins()
                            .bor_imm_u(low, (self.compiler.global_bank << 16) as i64);
                        return self.runtime("rcc_native_image_global", &[local]);
                    }
                    let bank = match pointer.bank {
                        BankTag::Global => 1,
                        BankTag::Stack => 2,
                        BankTag::Heap(b) => u64::from(b),
                        BankTag::Null => 0,
                        _ => return Err("constant pointer has no bank".into()),
                    };
                    let low = self.b.ins().band_imm_u(addr, 65535);
                    self.b.ins().bor_imm_u(low, (bank << 16) as i64)
                } else {
                    addr
                }
            }
            Value::AddressOffset { base, cells } => {
                let base = self.value(base)?;
                let cells = self.k(*cells);
                self.logical_gep(base, cells)?
            }
            Value::Undef => self.k(0),
            _ => return Err(format!("aggregate used as native scalar: {value:?}")),
        })
    }
    pub fn runtime(&mut self, name: &str, args: &[ClValue]) -> Result<ClValue, String> {
        if name != "rcc_native_poll" {
            if self.read_view.is_some() {
                // End the grant, but remember this region already attempted
                // acquisition. Hidden helpers in pointer expressions must not
                // turn a denied region into one acquisition per scalar load.
                let zero = self.k(0);
                self.read_view = Some(super::memory::ReadView::from_values(&[zero; 4]));
            }
        }
        let id = self.compiler.import(name, args.len())?;
        let reference = self.compiler.object.declare_func_in_func(id, self.b.func);
        let mut values = vec![self.context];
        values.extend_from_slice(args);
        let inst = self.b.ins().call(reference, &values);
        Ok(self.b.inst_results(inst)[0])
    }
    pub(super) fn define(&mut self, result: u32, value: ClValue) -> Result<(), String> {
        let var = *self
            .vars
            .get(&result)
            .ok_or_else(|| format!("undeclared IR result %{result}"))?;
        self.b.def_var(var, value);
        Ok(())
    }
    pub(super) fn edge(&mut self, from: u32, to: u32) -> Result<Block, String> {
        let destination = *self
            .blocks
            .get(&to)
            .ok_or_else(|| format!("unknown native block {to}"))?;
        if !self.phis.contains_key(&to) {
            return Ok(destination);
        }
        let edge = self.b.create_block();
        self.edges.push((edge, from, to));
        Ok(edge)
    }
    pub(super) fn emit_edges(&mut self) -> Result<(), String> {
        for (edge, from, to) in std::mem::take(&mut self.edges) {
            self.b.switch_to_block(edge);
            let phis = self.phis[&to].clone();
            let mut assignments = Vec::new();
            for (result, incoming) in phis {
                let value = incoming
                    .iter()
                    .find(|(_, label)| *label == from)
                    .ok_or_else(|| format!("Phi %{result} has no incoming edge from {from}"))?;
                assignments.push((result, self.value(&value.0)?));
            }
            for (result, value) in assignments {
                self.define(result, value)?;
            }
            self.b.ins().jump(self.blocks[&to], &[]);
        }
        Ok(())
    }
}

pub(super) fn emit_function(compiler: &mut Compiler, function: &Function) -> Result<(), String> {
    if compiler.resumable {
        return super::process::emit_function(compiler, function);
    }
    emit_direct_function(compiler, function)
}

pub(super) fn emit_direct_function(
    compiler: &mut Compiler,
    function: &Function,
) -> Result<(), String> {
    let mut context = compiler.object.make_context();
    context.func.signature =
        compiler.signature(function.parameters.len() + usize::from(function.is_vararg));
    let mut frontend = FunctionBuilderContext::new();
    {
        let mut b = FunctionBuilder::new(&mut context.func, &mut frontend);
        let entry = b.create_block();
        b.append_block_params_for_function_params(entry);
        b.switch_to_block(entry);
        let runtime = b.block_params(entry)[0];
        let mut types_map = BTreeMap::new();
        for (id, ty) in &function.parameters {
            types_map.insert(*id, ty.clone());
        }
        let mut allocas = BTreeMap::new();
        let mut cells = 0u64;
        let mut phis = BTreeMap::new();
        for block in &function.blocks {
            for inst in &block.instructions {
                if let Some((id, ty)) = result_type(inst) {
                    types_map.insert(id, ty);
                }
                if let I::Alloca {
                    result,
                    alloc_type,
                    count,
                    ..
                } = inst
                {
                    let count = match count {
                        None => 1,
                        Some(Value::Constant(n)) if *n >= 0 => *n as u64,
                        _ => return Err("native v1 requires constant alloca sizes".into()),
                    };
                    allocas.insert(*result, cells);
                    cells = cells
                        .checked_add(
                            Target::Native
                                .layout()
                                .size_in_cells(alloc_type)?
                                .checked_mul(count)
                                .ok_or("native local size overflow")?,
                        )
                        .ok_or("native frame overflow")?;
                }
                if let I::Phi {
                    result, incoming, ..
                } = inst
                {
                    phis.entry(block.id)
                        .or_insert_with(Vec::new)
                        .push((*result, incoming.clone()));
                }
            }
        }
        let var_area = cells;
        let mut extra = 0;
        for inst in function.blocks.iter().flat_map(|b| &b.instructions) {
            if let I::Call {
                args,
                stack_args_from: Some(fixed),
                ..
            } = inst
            {
                if *fixed > args.len() {
                    return Err("variadic call has fewer arguments than named parameters".into());
                }
                let count: u64 = args[*fixed..]
                    .iter()
                    .map(|arg| argument_words(arg, &types_map))
                    .sum();
                extra = extra.max(count);
            }
        }
        cells = cells.checked_add(extra).ok_or("variadic area overflow")?;
        let va_input = function
            .is_vararg
            .then(|| b.block_params(entry)[function.parameters.len() + 1]);
        let vars = types_map
            .keys()
            .map(|&id| (id, b.declare_var(types::I64)))
            .collect();
        let blocks = function
            .blocks
            .iter()
            .map(|block| (block.id, b.create_block()))
            .collect();
        let zero = b.ins().iconst(types::I64, 0);
        let owns_stack = cells != 0 || !allocas.is_empty() || function.is_vararg;
        let mut cg = Cg {
            b,
            compiler,
            context: runtime,
            vars,
            types: types_map,
            blocks,
            phis,
            frame: zero,
            mark: zero,
            owns_stack,
            allocas,
            edges: Vec::new(),
            va_input,
            var_area,
            bound_globals: BTreeMap::new(),
            read_view: None,
            read_region: false,
        };
        cg.bind_globals(function)?;
        if owns_stack {
            cg.mark = cg.runtime("rcc_native_stack_mark", &[])?;
            let size = cg.k(i64::try_from(cells).map_err(|_| "native frame exceeds i64")?);
            cg.frame = cg.runtime("rcc_native_stack_alloc", &[size])?;
        }
        for (index, (id, ty)) in function.parameters.iter().enumerate() {
            let value = cg.b.block_params(entry)[index + 1];
            let value = cg.mask(value, width(ty));
            cg.define(*id, value)?;
        }
        let first = function
            .blocks
            .first()
            .ok_or_else(|| format!("native function {} has no body", function.name))?;
        cg.b.ins().jump(cg.blocks[&first.id], &[]);
        for block in &function.blocks {
            cg.b.switch_to_block(cg.blocks[&block.id]);
            cg.read_view = None;
            if !block.has_terminator() {
                return Err(format!(
                    "native function {} block {} has no terminator",
                    function.name, block.id
                ));
            }
            let regions = super::memory::read_regions(&block.instructions);
            for (instruction, region) in block.instructions.iter().zip(regions) {
                cg.read_region = region;
                if !region { cg.read_view = None; }
                cg.instruction(instruction, block.id)
                    .map_err(|e| format!("{} block {}: {e}", function.name, block.id))?;
            }
        }
        cg.emit_edges()?;
        cg.b.seal_all_blocks();
        cg.b.finalize(cg.compiler.object.target_config());
    }
    let id = compiler
        .direct_functions
        .get(&function.name)
        .copied()
        .unwrap_or(compiler.functions[&function.name]);
    compiler
        .object
        .define_function(id, &mut context)
        .map_err(|e| {
            format!(
                "native compilation {}: {e:?}\n{}",
                function.name,
                context.func.display()
            )
        })?;
    Ok(())
}

pub(super) fn result_type(inst: &I) -> Option<(u32, IrType)> {
    match inst {
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
        | I::Phi {
            result,
            result_type,
            ..
        }
        | I::Select {
            result,
            result_type,
            ..
        } => Some((*result, result_type.clone())),
        I::GetElementPtr {
            result,
            result_type,
            ..
        } => {
            // Field GEPs may carry the element type, but the SSA value is always
            // an address. Continuation spills must preserve its bank bits.
            let ty = match result_type {
                IrType::FatPtr(_) => result_type.clone(),
                _ => IrType::FatPtr(Box::new(result_type.clone())),
            };
            Some((*result, ty))
        }
        I::Call {
            result: Some(result),
            result_type,
            ..
        }
        | I::Intrinsic {
            result: Some(result),
            result_type,
            ..
        } => Some((*result, result_type.clone())),
        I::Cast {
            result,
            target_type,
            ..
        } => Some((*result, target_type.clone())),
        _ => None,
    }
}
pub(super) fn width(ty: &IrType) -> u32 {
    match ty {
        IrType::I1 => 1,
        IrType::I8 => 8,
        IrType::I16 => 16,
        IrType::I32 | IrType::F32 | IrType::FatPtr(_) => 32,
        _ => 64,
    }
}

pub(super) fn argument_words(value: &Value, types: &BTreeMap<u32, IrType>) -> u64 {
    match value {
        Value::Temp(id) => types.get(id).and_then(IrType::size_in_words).unwrap_or(1),
        Value::FatPtr(_) | Value::Global(_) | Value::Function(_) | Value::AddressOffset { .. } => 2,
        _ => 1,
    }
}
