//! Lower checked RFL to Ripple IR.
//!
//! Each RFL function is `i16* rfl_fn_*(i16* frame)`. It returns the next
//! activation record, or null when `main` is finished. Records live on the
//! heap except the root `main` record, which the driver allocates once.
//! `rfl_frame_alloc` bumps a chunk from `malloc`. Returning rewinds that bump,
//! so a call reclaims its record before the caller continues. The hardware
//! stack only holds the driver and the function that is running right now.

mod arrays;
mod buffer_ownership;
mod build;
mod closures;
mod constants;
mod dispatch;
mod emit;
mod erasure;
mod faults;
mod ffi;
mod frames;
mod liveness;
mod managed_runtime;
mod memory;
mod numeric;
mod objects;
mod optimize;
mod ownership;
mod places;
mod print;
mod resources;
mod string_runtime;
mod strings;
mod temporaries;
mod vector_fill;
mod vectors;

use arrays::*;
use buffer_ownership::*;
use build::*;
use closures::*;
use constants::*;
use dispatch::*;
use emit::*;
use erasure::*;
use faults::*;
use ffi::*;
use frames::*;
use managed_runtime::*;
use memory::*;
use numeric::*;
use objects::*;
use optimize::*;
use ownership::*;
use places::*;
use print::*;
use resources::*;
use string_runtime::*;
use strings::*;
use temporaries::*;
use vector_fill::*;
use vectors::*;

use rcc_ir::ir::{
    FatPointer, Function, GlobalVariable, IrBinaryOp, IrBuilder, IrType, IrUnaryOp, Linkage,
    Module, Value,
};
use rcc_ir::BankTag;
use std::sync::Arc;

use crate::ast::{BinOp, UnaryOp};
use crate::error::CompileError;
use crate::ty::Ty;
use crate::typed::{AdtDef, CtorDef, TBlock, TExpr, TExprKind, TFn, TModule, TPlaceStep, TStmt};

const RESUME: i64 = 0;
const FUNC_ID: i64 = 1;
const CALLER: i64 = 2;
const DEST: i64 = 4;
const PAYLOAD: i64 = 5;

struct Slot {
    offset: i64,
    ty: Ty,
    /// The slot holds a box pointer. Loads and stores use the value inside it.
    shared: bool,
}

struct Proc {
    name: String,
    ret: Ty,
    func_id: usize,
    frame_cells: i64,
    slots: Vec<Slot>,
    param_slots: Vec<usize>,
    /// Slot index of each binding, in binding order.
    binding_slots: Vec<usize>,
    /// Frame offset of the environment pointer. Present on every function.
    env_at: Option<i64>,
    captures: Vec<LowerCap>,
    result_slot: Option<usize>,
    segs: Vec<Seg>,
    structs: Arc<Vec<Vec<Ty>>>,
    tuples: Arc<Vec<Vec<Ty>>>,
    collections: Arc<crate::typed::CollectionTypes>,
    resources: Arc<crate::resources::ResourceTypes>,
    /// Emit a C function of this name that runs the frame function.
    c_export: bool,
}

#[derive(Clone)]
struct LowerCap {
    /// Binding in the function that creates the closure.
    parent: u32,
    /// Slot in this function that receives the copy.
    slot: usize,
    ty: Ty,
    cells: i64,
    /// Cell offset inside the environment object. Cell 0 is the refcount.
    offset: i64,
    /// The captured cells are a box pointer, not a copy of the value.
    shared: bool,
}

struct Seg {
    insns: Vec<Insn>,
    term: Option<Term>,
}

enum Insn {
    EraseOwner {
        dest: usize,
        src: usize,
        owner: u32,
    },
    Replace {
        dest: usize,
        place: usize,
        value: usize,
    },
    IntoInner {
        dest: usize,
        src: usize,
    },
    Move {
        dest: usize,
        src: usize,
    },
    Drop {
        slot: usize,
    },
    Owned {
        dest: usize,
        value: usize,
        cleanup: usize,
        inner: Ty,
    },
    Const {
        slot: usize,
        value: i64,
    },
    Copy {
        dest: usize,
        src: usize,
    },
    Bin {
        dest: usize,
        op: BinOp,
        lhs: usize,
        rhs: usize,
    },
    Neg {
        dest: usize,
        src: usize,
    },
    Not {
        dest: usize,
        src: usize,
    },
    BitNot {
        dest: usize,
        src: usize,
    },
    /// Allocate the shared cell for a mutable capture and store its pointer.
    BoxNew {
        slot: usize,
    },
    /// Store the address of a C function.
    CFn {
        dest: usize,
        symbol: String,
    },
    /// Indirect C call through a function pointer.
    CApply {
        dest: usize,
        callee: usize,
        args: Vec<usize>,
        ret: Ty,
    },
    Cast {
        dest: usize,
        src: usize,
        from: Ty,
        to: Ty,
    },
    /// Build a constructor into `dest`. Empty `fields` is an immediate nullary value.
    Make {
        dest: usize,
        tag: u16,
        fields: Vec<(usize, i64)>,
    },
    /// Copy `cells` words from a payload object into `dest`.
    Read {
        dest: usize,
        src: usize,
        offset: i64,
        cells: i64,
    },
    /// Decimal integer or `true`/`false`, optionally followed by a newline.
    Print {
        slot: usize,
        ty: Ty,
        newline: bool,
    },
    PrintLit {
        chars: Vec<u16>,
        newline: bool,
    },
    Putc {
        slot: usize,
    },
    /// Build a closure value for a lifted lambda.
    Closure {
        dest: usize,
        func: String,
    },
    VecLit {
        dest: usize,
        elems: Vec<usize>,
        elem: Ty,
    },
    VecFill {
        dest: usize,
        len: u32,
        value: i64,
    },
    ArrayLit {
        dest: usize,
        elems: Vec<usize>,
        elem: Ty,
    },
    /// `base[start..end]`. `inclusive` means the end index is kept.
    SliceNew {
        dest: usize,
        base: usize,
        start: usize,
        end: usize,
        inclusive: bool,
        elem: Ty,
    },
    ToVec {
        dest: usize,
        src: usize,
        elem: Ty,
    },
    Index {
        dest: usize,
        vec: usize,
        index: usize,
        elem: Ty,
    },
    Len {
        dest: usize,
        vec: usize,
    },
    Empty {
        dest: usize,
        vec: usize,
    },
    Clear {
        elem: Ty,
        vec: usize,
        offset: i64,
    },
    Pop {
        vec: usize,
        offset: i64,
        value: usize,
        dest: usize,
        elem: Ty,
        some_tag: u16,
        none_tag: u16,
    },
    Get {
        vec: usize,
        index: usize,
        value: usize,
        dest: usize,
        elem: Ty,
        some_tag: u16,
        none_tag: u16,
    },
    Push {
        vec: usize,
        offset: i64,
        value: usize,
        elem: Ty,
    },
    IndexStore {
        vec: usize,
        index: usize,
        value: usize,
        elem: Ty,
        /// Added to the slot offset. A field's vector lives inside its struct.
        offset: i64,
        /// `Vec`, `Array`, or a reference to a vector. Selects the store shape.
        container: Ty,
    },
    /// Address of a local slot. The destination holds that pointer.
    Borrow {
        dest: usize,
        place: Place,
        mutable: bool,
    },
    /// `expr?`. Failure writes the function result and returns.
    Try {
        src: usize,
        dest: usize,
        result: usize,
        ok_tag: u16,
        fail_tag: u16,
        payload: Ty,
        fail_slot: Option<usize>,
        fail: Option<Ty>,
        cleanup: Vec<usize>,
    },
    /// Release `dest`, then put `src` there. `transfer` clears `src` without an extra retain.
    VecStore {
        dest: usize,
        src: usize,
        transfer: bool,
    },
    /// Store fields of an inline struct or tuple, in order.
    Record {
        dest: usize,
        fields: Vec<usize>,
    },
    /// Copy `cells` words from `src` at `offset` into `dest`.
    Take {
        dest: usize,
        src: usize,
        offset: i64,
    },
    /// Store `src` at a cell offset inside `dest`.
    FieldStore {
        dest: usize,
        offset: i64,
        src: usize,
    },
    /// Call a C function. `ret` is `Unit` when the call is discarded as a value.
    /// Arguments at `fixed` and beyond are variadic and go on the stack.
    Extern {
        dest: usize,
        name: String,
        args: Vec<usize>,
        ret: Ty,
        fixed: usize,
    },
    /// `left + right` for strings.
    StrConcat {
        dest: usize,
        lhs: usize,
        rhs: usize,
    },
    /// `s[start..end]` copied into a new string. Bounds are byte offsets.
    StrSlice {
        dest: usize,
        base: usize,
        start: usize,
        end: usize,
        inclusive: bool,
    },
    StrFromChar {
        dest: usize,
        src: usize,
    },
    StrFromByte {
        dest: usize,
        src: usize,
    },
    /// Address of a static `c"..."` global, stored as a raw pointer.
    CStr {
        dest: usize,
        bytes: Vec<u16>,
    },
    /// Build a `String` from UTF-8 octets. An empty slice is the null string.
    Str {
        dest: usize,
        bytes: Vec<u8>,
    },
    /// Load `cells` words through a raw pointer. `index` selects an element.
    PtrLoad {
        dest: usize,
        ptr: usize,
        index: Option<usize>,
        stride: i64,
        cells: i64,
    },
    /// Store `cells` words through a raw pointer.
    PtrStore {
        ptr: usize,
        index: Option<usize>,
        src: usize,
        stride: i64,
        cells: i64,
    },
    /// Move a raw pointer by `index` elements.
    PtrOff {
        dest: usize,
        ptr: usize,
        index: usize,
        stride: i64,
    },
    /// Address or bank cell. `cell` is 0 or 1.
    PtrWord {
        dest: usize,
        ptr: usize,
        cell: i64,
    },
    /// Address cell is zero.
    PtrNull {
        dest: usize,
        ptr: usize,
    },
}

/// A frame root or an already computed address, followed by projections.
struct Place {
    root: usize,
    indirect: bool,
    projections: Vec<PlaceStep>,
}

enum PlaceStep {
    Owned,
    Field {
        offset: i64,
    },
    Index {
        index: usize,
        container: Ty,
        elem: Ty,
    },
}

enum Term {
    Return,
    Jump(usize),
    If {
        cond: usize,
        then_seg: usize,
        else_seg: usize,
    },
    Call {
        callee: String,
        args: Vec<usize>,
        dest: usize,
        next: usize,
    },
    /// Call the closure in `slot`. `arg_tys` selects where each argument is stored.
    Apply {
        slot: usize,
        args: Vec<usize>,
        arg_tys: Vec<Ty>,
        dest: usize,
        next: usize,
    },
    Tag {
        slot: usize,
        tag: u16,
        then_seg: usize,
        else_seg: usize,
    },
    Fail,
    /// Direct tail call. The callee reuses this frame's caller link.
    Tail {
        callee: String,
        args: Vec<usize>,
    },
    /// Compare `slot` with `value` and branch.
    Lit {
        slot: usize,
        value: i64,
        then_seg: usize,
        else_seg: usize,
    },
}

struct Built {
    procs: Vec<Proc>,
    adts: Vec<AdtDef>,
    collections: Arc<crate::typed::CollectionTypes>,
    /// Parallel to `structs`.
    repr_c: Vec<bool>,
    need_alloc: bool,
    need_fail: bool,
    need_heap: bool,
    need_io: bool,
    need_print_num: bool,
    need_print_float: bool,
    externs: Vec<crate::typed::ExternFn>,
    cstrs: Vec<Vec<u16>>,
}

pub fn lower(module_name: &str, module: &TModule) -> Result<Module, CompileError> {
    let built = build(module)?;
    emit(module_name, &built)
}

fn build(module: &TModule) -> Result<Built, CompileError> {
    let structs = Arc::new(module.structs.clone());
    let tuples = Arc::new(module.tuples.clone());
    let collections = Arc::new(module.collections.clone());
    let resources = Arc::new(crate::resources::ResourceTypes::new(module));
    let mut procs = module
        .functions
        .iter()
        .enumerate()
        .map(|(id, func)| {
            build_fn(
                func,
                id,
                structs.clone(),
                tuples.clone(),
                collections.clone(),
                resources.clone(),
            )
        })
        .collect::<Result<Vec<_>, _>>()?;
    retain_reachable(&mut procs);
    for proc in &mut procs {
        fold_scalar_constants(proc);
    }
    let mut need_alloc = !module.collections.owners.is_empty();
    let mut need_fail = need_alloc;
    let mut need_heap = need_alloc;
    let mut need_io = false;
    let mut need_print_num = false;
    let mut need_print_float = false;
    for proc in &procs {
        if proc.segs.iter().any(|seg| {
            matches!(
                seg.term,
                Some(Term::Call { .. } | Term::Apply { .. } | Term::Tail { .. })
            )
        }) {
            need_alloc = true;
        }
        if proc.segs.len() > 1
            || proc
                .segs
                .iter()
                .any(|seg| matches!(seg.term, Some(Term::Fail)))
        {
            need_fail = true;
        }
        if proc.segs.iter().any(|seg| {
            seg.insns.iter().any(|insn| {
                matches!(insn, Insn::Bin { op: BinOp::Div | BinOp::Rem, lhs, .. } if !proc.slots[*lhs].ty.is_float())
            })
        }) {
            need_fail = true;
        }
        if proc.segs.iter().any(|seg| {
            seg.insns
                .iter()
                .any(|insn| matches!(insn, Insn::Make { fields, .. } if !fields.is_empty()))
        }) {
            need_heap = true;
            need_fail = true;
        }
        if !proc.captures.is_empty() {
            need_heap = true;
            need_fail = true;
        }
        if proc.slots.iter().any(|slot| slot.ty.owns_buffer()) {
            need_heap = true;
            need_fail = true;
        }
        if proc
            .slots
            .iter()
            .any(|slot| matches!(slot.ty, Ty::Adt(_) | Ty::Func(_) | Ty::String))
        {
            need_heap = true;
        }
        if proc.segs.iter().any(|seg| {
            seg.insns
                .iter()
                .any(|insn| matches!(insn, Insn::Str { bytes, .. } if !bytes.is_empty()))
        }) {
            need_heap = true;
            need_fail = true;
        }
        if proc.segs.iter().any(|seg| {
            seg.insns.iter().any(|insn| {
                matches!(
                    insn,
                    Insn::StrConcat { .. }
                        | Insn::StrSlice { .. }
                        | Insn::StrFromChar { .. }
                        | Insn::StrFromByte { .. }
                )
            })
        }) {
            need_heap = true;
            need_fail = true;
        }
        if proc.segs.iter().any(|seg| {
            seg.insns.iter().any(
                |insn| matches!(insn, Insn::Cast { to: Ty::Char, from, .. } if *from != Ty::Char),
            )
        }) {
            need_fail = true;
        }
        if proc.segs.iter().any(|seg| {
            seg.insns.iter().any(|insn| {
                matches!(
                    insn,
                    Insn::PtrLoad { .. } | Insn::PtrStore { .. } | Insn::BoxNew { .. }
                )
            })
        }) {
            need_fail = true;
            need_heap = true;
        }
        if proc.segs.iter().any(|seg| {
            seg.insns.iter().any(|insn| {
                matches!(
                    insn,
                    Insn::Pop { .. }
                        | Insn::Get { .. }
                        | Insn::Push { .. }
                        | Insn::Clear { .. }
                        | Insn::IndexStore { .. }
                        | Insn::Index { .. }
                        | Insn::VecLit { .. }
                        | Insn::SliceNew { .. }
                        | Insn::ToVec { .. }
                        | Insn::Try { fail: Some(_), .. }
                )
            })
        }) {
            need_heap = true;
            need_fail = true;
        }
        for seg in &proc.segs {
            for insn in &seg.insns {
                if let Insn::Borrow { place, .. } = insn {
                    if place
                        .projections
                        .iter()
                        .any(|p| matches!(p, PlaceStep::Index { .. }))
                    {
                        need_fail = true;
                        need_heap = true;
                    }
                }
                match insn {
                    Insn::Print { ty, .. } => {
                        need_io = true;
                        if ty.is_int() {
                            need_print_num = true;
                        }
                        if ty.is_float() {
                            need_print_num = true;
                            need_print_float = true;
                        }
                    }
                    Insn::PrintLit { .. } | Insn::Putc { .. } => need_io = true,
                    _ => {}
                }
            }
        }
    }
    if !procs.iter().any(|proc| proc.name == "main") {
        return Err(CompileError::plain("program has no main"));
    }
    let cstrs = c_strings(&procs);
    Ok(Built {
        procs,
        adts: module.adts.clone(),
        collections,
        repr_c: module.repr_c.clone(),
        need_alloc,
        need_fail,
        need_heap,
        need_io,
        need_print_num,
        need_print_float,
        externs: module.externs.clone(),
        cstrs,
    })
}

fn c_strings(procs: &[Proc]) -> Vec<Vec<u16>> {
    let mut cstrs = Vec::new();
    for proc in procs {
        for seg in &proc.segs {
            for insn in &seg.insns {
                if let Insn::CStr { bytes, .. } = insn {
                    if !cstrs.iter().any(|found: &Vec<u16>| found == bytes) {
                        cstrs.push(bytes.clone());
                    }
                }
            }
        }
    }
    cstrs
}

fn emit(module_name: &str, built: &Built) -> Result<Module, CompileError> {
    let mut module = Module::new(module_name.to_string());
    let mut builder = IrBuilder::new();
    if built.need_alloc || built.need_fail || built.need_heap {
        module.add_function(extern_fn("rfl_halt", IrType::Void, Vec::new()));
        if !built.need_io {
            module.add_function(extern_fn("putchar", IrType::Void, vec![IrType::I16]));
        }
    }
    if built.need_alloc || built.need_heap {
        module.add_function(extern_fn("malloc", ptr_ty(), vec![IrType::I32]));
        module.add_function(extern_fn("free", IrType::Void, vec![ptr_ty()]));
    }
    if built
        .procs
        .iter()
        .flat_map(|p| &p.segs)
        .flat_map(|s| &s.insns)
        .any(|i| matches!(i, Insn::VecFill { .. }))
    {
        module.add_function(extern_fn(
            "memset",
            ptr_ty(),
            vec![ptr_ty(), IrType::I16, IrType::I32],
        ));
    }
    if built.need_io {
        module.add_function(extern_fn("putchar", IrType::Void, vec![IrType::I16]));
    }
    for ext in &built.externs {
        if module.get_function(&ext.name).is_some() {
            continue;
        }
        let params = ext
            .params
            .iter()
            .copied()
            .map(|ty| c_param_ir(ty, &built.repr_c))
            .collect();
        let mut func = extern_fn(&ext.name, c_ret_ir(ext.ret, &built.repr_c), params);
        func.is_vararg = ext.variadic;
        module.add_function(func);
    }
    for (index, bytes) in built.cstrs.iter().enumerate() {
        let mut values: Vec<i64> = bytes.iter().map(|cell| i64::from(*cell)).collect();
        values.push(0);
        module.add_global(GlobalVariable {
            name: format!("__rfl_cstr_{index}"),
            var_type: IrType::Array {
                element_type: Box::new(IrType::I16),
                size: values.len() as u64,
            },
            is_constant: true,
            initializer: Some(Value::ConstantArray(values)),
            linkage: Linkage::Internal,
            symbol_id: None,
        });
    }
    if built.need_alloc {
        module.add_global(GlobalVariable {
            name: "rfl_bump".to_string(),
            var_type: ptr_ty(),
            is_constant: false,
            initializer: Some(null_ptr()),
            linkage: Linkage::Internal,
            symbol_id: None,
        });
    }
    for proc in &built.procs {
        emit_proc(&mut builder, proc, built)?;
        let mut function = builder.finish_function().unwrap();
        promote_frame_words(&mut function, proc);
        module.add_function(function);
    }
    emit_driver(&mut builder, built)?;
    module.add_function(builder.finish_function().unwrap());
    for proc in &built.procs {
        if proc.c_export {
            emit_c_export(&mut builder, proc, built)?;
            module.add_function(builder.finish_function().unwrap());
        }
    }
    if built.need_alloc {
        emit_frame_alloc(&mut builder)?;
        module.add_function(builder.finish_function().unwrap());
        emit_frame_release(&mut builder)?;
        module.add_function(builder.finish_function().unwrap());
    }
    if built.need_print_num {
        emit_print_u64(&mut builder)?;
        module.add_function(builder.finish_function().unwrap());
    }
    if built.need_print_float {
        emit_print_f64(&mut builder)?;
        module.add_function(builder.finish_function().unwrap());
    }
    if built.need_alloc || built.need_fail || built.need_heap {
        emit_rfl_fail(&mut builder)?;
        module.add_function(builder.finish_function().unwrap());
    }
    emit_string_runtime(&mut builder, &mut module, built)?;
    emit_retain_runtime(&mut builder, &mut module, built)?;
    emit_managed_releases(&mut builder, &mut module, built)?;
    emit_frame_drops(&mut builder, &mut module, built)?;
    emit_arc(&mut builder, &mut module, built)?;
    emit_owner_drops(&mut builder, &mut module, built)?;
    emit_erased_drop(&mut builder, &mut module, built)?;
    Ok(module)
}

const VEC_REFS: i64 = 0;
/// `usize` values in headers and index arithmetic. Two cells.
const COUNT: IrType = IrType::I32;
const VEC_LEN: i64 = 1;
const VEC_CAP: i64 = 3;
const VEC_DATA: i64 = 5;
const SLICE_START: i64 = 2;
const SLICE_LEN: i64 = 4;
const SLICE_CELLS: i64 = 6;
/// Refcount, then tag, then constructor fields.
const OBJ_REFS: i64 = 0;
const OBJ_TAG: i64 = 1;
const OBJ_FIELDS: i64 = 2;

trait IrResult<T> {
    fn ir(self) -> Result<T, CompileError>;
}

impl<T> IrResult<T> for Result<T, String> {
    fn ir(self) -> Result<T, CompileError> {
        self.map_err(CompileError::plain)
    }
}
