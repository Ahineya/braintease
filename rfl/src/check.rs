mod async_functions;
mod bindings;
mod borrows;
mod calls;
mod captures;
mod closures;
mod collections;
mod constants;
mod constructors;
mod control;
mod coverage;
mod declarations;
mod display;
mod editor;
mod expressions;
mod extraction;
mod facts;
mod ffi;
mod functions;
mod inference;
mod iteration;
mod methods;
mod numeric;
mod operators;
mod patterns;
mod products;
mod projections;
mod register;
mod resources;
mod statements;
mod traits;
mod types;

use captures::*;
use declarations::*;
use display::*;
pub(crate) use editor::trait_method_signature;
use numeric::*;

use std::collections::{HashMap, HashSet};

use crate::ast::{
    Adt, BinOp, Block, Ctor, Expr, ExprKind, Function, Impl, Module, Param, ParsedTy, Pat, Recv,
    Span, Stmt, Trait, TraitScope, TypeParam, UnaryOp,
};
use crate::error::CompileError;
use crate::ty::Ty;
use crate::typed::*;

#[derive(Clone)]
struct Sig {
    params: Vec<Ty>,
    ret: Option<Ty>,
    /// Call this as a C function, not as an RFL frame.
    c_abi: bool,
    /// `...` after the named parameters.
    variadic: bool,
}

#[derive(Clone)]
struct Binding {
    ty: Ty,
    mutable: bool,
    /// Writes inside a closure share this binding through a heap cell.
    shared: bool,
    def: Span,
}

/// A name introduced by a pattern, parameter, or function, and where it can be used.
#[derive(Clone)]
pub struct BindingFact {
    pub name: String,
    pub def: Span,
    pub ty: String,
    pub scope: Span,
    pub visible_from: usize,
    /// An algebraic-data-type constructor, nullary or with a payload.
    pub variant: bool,
}

/// The signature of a call, covering the whole call expression.
#[derive(Clone)]
pub struct CallSig {
    pub span: Span,
    pub label: String,
}

/// The type of a source expression, using that expression's full span.
#[derive(Clone)]
pub struct ExprType {
    pub span: Span,
    pub ty: String,
}

/// A use of a name. `def` is the binding's defining span.
#[derive(Clone)]
pub struct NameUse {
    pub span: Span,
    pub def: Span,
    /// `None` means `def` is in the same file as `span`.
    pub def_file: Option<std::path::PathBuf>,
    pub ty: String,
}

#[derive(Clone)]
struct AdtSchema {
    name: String,
    params: Vec<String>,
    type_params: Vec<TypeParam>,
    ctors: Vec<Vec<ParsedTy>>,
}

#[derive(Clone)]
struct FnSchema {
    index: usize,
    type_params: Vec<String>,
}

struct Suspended {
    scopes: Vec<HashMap<String, u32>>,
    bindings: Vec<Binding>,
    fn_ret: Ty,
    loops: u32,
}

/// A method proven applicable to an expression, including generic bounds.
#[derive(Clone)]
pub struct MethodFact {
    pub receiver: Span,
    pub receiver_type: String,
    pub trait_name: String,
    pub name: String,
    pub signature: String,
}

#[derive(Clone)]
pub struct TypeMemberFact {
    pub qualifier: String,
    pub contract: String,
    pub name: String,
    pub detail: String,
    pub scope: Span,
    pub concrete: bool,
}

pub struct ProgramInfo {
    /// Source-level names for editor display only. Type identities stay mangled.
    pub display_names: HashMap<String, String>,
    pub facts: Vec<BindingFact>,
    pub uses: Vec<NameUse>,
    pub expr_types: Vec<ExprType>,
    /// `(type name, method name)` for every inherent method that was checked.
    pub methods: Vec<(String, String)>,
    pub call_sigs: Vec<CallSig>,
    pub trait_methods: Vec<MethodFact>,
    pub type_members: Vec<TypeMemberFact>,
}

pub(crate) fn completion_function(func: &Function) -> Option<crate::completion::ExtensionFunction> {
    if func.params.is_empty() {
        return None;
    }
    Some(crate::completion::ExtensionFunction {
        name: func.name.clone(),
        signature: parsed_signature(func, false),
        generics: func
            .type_params
            .iter()
            .map(|param| param.name.clone())
            .collect(),
    })
}

pub(crate) fn completion_type_name(ty: &ParsedTy) -> String {
    parsed_brief(ty)
}

pub fn check(source: &str, module: &Module) -> Result<TModule, CompileError> {
    analyze_inner(source, module, false).1
}

pub fn analyze(source: &str, module: &Module) -> (ProgramInfo, Result<TModule, CompileError>) {
    analyze_inner(source, module, true)
}

fn analyze_inner(
    source: &str,
    module: &Module,
    editor: bool,
) -> (ProgramInfo, Result<TModule, CompileError>) {
    let mut checker = Checker {
        editor,
        type_members: Vec::new(),
        trait_methods: Vec::new(),
        method_cache: HashMap::new(),
        method_targets: HashMap::new(),
        source,
        sigs: HashMap::new(),
        scopes: Vec::new(),
        bindings: Vec::new(),
        fn_ret: Ty::Unit,
        async_context: None,
        loops: 0,
        schemas: Vec::new(),
        ctor_schema: HashMap::new(),
        adts: Vec::new(),
        mono: HashMap::new(),
        fn_schemas: HashMap::new(),
        ast_fns: module.functions.clone(),
        fn_mono: HashMap::new(),
        typed: Vec::new(),
        funcs: Vec::new(),
        lambda_count: 0,
        outer_scopes: Vec::new(),
        outer_bindings: Vec::new(),
        captures: Vec::new(),
        cfns: Vec::new(),
        shared_spans: HashSet::new(),
        capture_error: None,
        vec_elems: Vec::new(),
        owner_elems: Vec::new(),
        arrays: Vec::new(),
        slice_elems: Vec::new(),
        refs: Vec::new(),
        borrows: Vec::new(),
        methods: Vec::new(),
        traits: Vec::new(),
        projection_types: HashMap::new(),
        projection_stack: Vec::new(),
        impl_contexts: HashMap::new(),
        assoc_self: None,
        trait_sigs: HashMap::new(),
        trait_impls: Vec::new(),
        trait_scopes: module.trait_scopes.clone(),
        opaque: HashMap::new(),
        subst: HashMap::new(),
        generic_check: false,
        trait_dispatch: HashMap::new(),
        declarations_checked: HashSet::new(),
        declarations_active: HashSet::new(),
        loose_receiver: false,
        consts: HashMap::new(),
        const_defs: HashMap::new(),
        structs: Vec::new(),
        struct_names: HashMap::new(),
        tuples: Vec::new(),
        ptrs: Vec::new(),
        externs: Vec::new(),
        unsafe_depth: 0,
        scope_spans: Vec::new(),
        facts: Vec::new(),
        uses: Vec::new(),
        expr_types: Vec::new(),
        next_visible: None,
        current_span: Span::empty(),
        sig_skip_self: false,
        call_sigs: Vec::new(),
        type_defs: HashMap::new(),
    };
    let mut prelude_base = None;
    let result = (|| -> Result<TModule, CompileError> {
        let mut owned = module.clone();
        owned.adts.insert(0, prelude_async());
        crate::modules::isolate_prelude_types(&mut owned);
        if checker.trait_scopes.is_empty() {
            let mut visible: Vec<_> = owned.traits.iter().map(|t| t.name.clone()).collect();
            if !visible.iter().any(|n| n == "Printable") {
                visible.push("$std$Printable".into());
            }
            let owned_names = owned
                .traits
                .iter()
                .map(|t| t.name.clone())
                .chain(owned.adts.iter().map(|a| a.name.clone()))
                .chain(owned.structs.iter().map(|s| s.name.clone()))
                .collect();
            checker.trait_scopes.push(TraitScope {
                span: Span::new(0, source.len()),
                visible,
                owned: owned_names,
                core: false,
            });
        }
        if !owned.adts.iter().any(|a| a.name == "Option") {
            owned.adts.insert(0, prelude_option());
        }
        if !owned.adts.iter().any(|a| a.name == "Result") {
            owned.adts.insert(0, prelude_result());
        }
        if !owned.traits.iter().any(|t| t.name == "$std$Printable") {
            prelude_base = Some(source.len() + 1);
            let mut prelude = crate::modules::printing_prelude(source.len() + 1)?;
            owned.traits.append(&mut prelude.traits);
            owned.impls.append(&mut prelude.impls);
            owned.functions.append(&mut prelude.functions);
            checker.trait_scopes.append(&mut prelude.trait_scopes);
        }
        checker.prepare_traits(&mut owned)?;
        checker.note_type_members(&owned);
        checker.methods = expand_impls(source, &mut owned)?;
        checker.ast_fns = owned.functions.clone();
        checker.register(&mut owned)?;
        checker.register_trait_signatures()?;
        // Rigid generic types are a checking-only universe. They must not
        // contaminate runtime layouts or force unreachable monomorphizations.
        let mut declarations = checker.clone();
        declarations.validate_traits()?;
        declarations.check_generic_declarations()?;
        checker.trait_dispatch = declarations.trait_dispatch;
        checker.facts = declarations.facts;
        checker.uses = declarations.uses;
        checker.expr_types = declarations.expr_types;
        checker.call_sigs = declarations.call_sigs;
        checker.trait_methods = declarations.trait_methods;
        checker.method_targets = declarations.method_targets;
        for item in &owned.consts {
            if checker.ctor_schema.contains_key(&item.name) {
                return Err(CompileError::at(
                    source,
                    item.name_span.start,
                    format!("`{}` is already a constructor", item.name),
                ));
            }
        }
        let concrete: Vec<Function> = owned
            .functions
            .iter()
            .filter(|func| func.type_params.is_empty())
            .cloned()
            .collect();
        for func in &concrete {
            if !checker.typed.iter().any(|f| f.name == func.name) {
                checker.check_fn(func, &func.name, true, false)?;
            }
        }
        let structs = checker
            .structs
            .iter()
            .map(|info| info.fields.iter().map(|(_, ty)| *ty).collect())
            .collect();
        let repr_c = checker.structs.iter().map(|info| info.repr_c).collect();
        let module = TModule {
            collections: CollectionTypes {
                vectors: checker.vec_elems.clone(),
                owners: checker.owner_elems.clone(),
                slices: checker.slice_elems.clone(),
                arrays: checker.arrays.iter().map(|a| a.elem).collect(),
            },
            functions: std::mem::take(&mut checker.typed),
            adts: std::mem::take(&mut checker.adts),
            structs,
            repr_c,
            tuples: std::mem::take(&mut checker.tuples),
            externs: std::mem::take(&mut checker.externs),
        };
        crate::resources::check(source, &module)?;
        Ok(module)
    })();
    checker.locate_prelude_uses(prelude_base);
    let info = ProgramInfo {
        display_names: HashMap::new(),
        type_members: std::mem::take(&mut checker.type_members),
        trait_methods: std::mem::take(&mut checker.trait_methods),
        facts: std::mem::take(&mut checker.facts),
        uses: std::mem::take(&mut checker.uses),
        expr_types: std::mem::take(&mut checker.expr_types),
        methods: checker
            .methods
            .iter()
            .map(|method| (method.type_name.clone(), method.method.clone()))
            .collect(),
        call_sigs: std::mem::take(&mut checker.call_sigs),
    };
    (info, result)
}

/// Globally available compiler functions, shared with editor completion.
pub(crate) const BUILTIN_FUNCTIONS: &[(&str, &str)] = &[
    ("print", "fn<T: Printable>(T) -> ()"),
    ("println", "fn<T: Printable>(T) -> ()"),
    ("putc", "fn(integer | char) -> ()"),
    ("from_char", "fn(char) -> String"),
    ("from_byte", "fn(u8) -> String"),
    ("owned", "fn<T>(T, fn(&mut T) -> unit) -> Owned<T>"),
    ("move", "fn<T>(T) -> T"),
    ("drop", "fn<T>(T) -> unit"),
];

fn builtin_signature(name: &str) -> Option<&'static str> {
    if name == "__expect_value" {
        return Some("fn(Option<T> | Result<T, E>, String) -> T");
    }
    if name == "__async_poll" {
        return Some("fn(&mut Async<T>, &mut Context) -> Poll<T>");
    }
    if name == "__async_cancel" {
        return Some("fn(&mut Async<T>) -> unit");
    }
    if name == "__erase_owned" {
        return Some("fn(Owned<T>) -> ErasedOwner");
    }
    if name == "__borrow_raw" {
        return Some("unsafe fn(*mut T) -> &mut T");
    }
    if name == "__replace_value" {
        return Some("fn(&mut T, T) -> T");
    }
    if name == "__write_value" {
        return Some("fn(scalar | String) -> ()");
    }
    BUILTIN_FUNCTIONS
        .iter()
        .find_map(|(builtin, signature)| (*builtin == name).then_some(*signature))
}

fn is_builtin(name: &str) -> bool {
    !matches!(name, "print" | "println") && builtin_signature(name).is_some()
}

fn always_returns(block: &Block) -> bool {
    match block.stmts.last() {
        Some(Stmt::Return { .. }) => true,
        Some(Stmt::Expr(expr)) => expr_returns(expr),
        _ => false,
    }
}

fn expr_returns(expr: &Expr) -> bool {
    match &expr.kind {
        ExprKind::If {
            then_body,
            else_body,
            ..
        } => always_returns(then_body) && else_body.as_ref().is_some_and(|e| expr_returns(e)),
        ExprKind::Block(block) => block.tail.is_none() && always_returns(block),
        _ => false,
    }
}

#[derive(Clone)]
struct Checker<'a> {
    /// None outside async bodies; Some(None) until the first await needs Context.
    async_context: Option<Option<u32>>,
    editor: bool,
    type_members: Vec<TypeMemberFact>,
    trait_methods: Vec<MethodFact>,
    method_cache: HashMap<(Ty, usize), Vec<(String, String, String)>>,
    /// Full source call and method name to the selected declaration.
    method_targets: HashMap<(usize, usize, String), (Span, String)>,
    source: &'a str,
    sigs: HashMap<String, Sig>,
    scopes: Vec<HashMap<String, u32>>,
    bindings: Vec<Binding>,
    fn_ret: Ty,
    loops: u32,
    schemas: Vec<AdtSchema>,
    /// Constructor name to `(schema index, tag)`.
    ctor_schema: HashMap<String, (usize, u16)>,
    adts: Vec<AdtDef>,
    mono: HashMap<(usize, Vec<Ty>), u32>,
    fn_schemas: HashMap<String, FnSchema>,
    ast_fns: Vec<Function>,
    fn_mono: HashMap<(String, Vec<Ty>), String>,
    typed: Vec<TFn>,
    funcs: Vec<FuncSig>,
    lambda_count: u32,
    /// Bindings of the function that is checking a lambda. Empty otherwise.
    outer_scopes: Vec<HashMap<String, u32>>,
    outer_bindings: Vec<Binding>,
    captures: Vec<crate::typed::Capture>,
    vec_elems: Vec<Ty>,
    owner_elems: Vec<Ty>,
    arrays: Vec<ArrayInfo>,
    slice_elems: Vec<Ty>,
    refs: Vec<RefTy>,
    borrows: Vec<LiveBorrow>,
    methods: Vec<Inherent>,
    traits: Vec<Trait>,
    projection_types: HashMap<(Ty, String, String), Ty>,
    projection_stack: Vec<(Ty, String, String)>,
    impl_contexts: HashMap<String, String>,
    assoc_self: Option<(String, Ty)>,
    trait_sigs: HashMap<(String, String), FuncSig>,
    trait_impls: Vec<std::rc::Rc<traits::TraitImpl>>,
    trait_scopes: Vec<TraitScope>,
    /// Rigid declaration-time parameters, represented by opaque nominal types.
    opaque: HashMap<Ty, (String, Vec<String>)>,
    subst: HashMap<String, Ty>,
    generic_check: bool,
    trait_dispatch: HashMap<(usize, usize, String), String>,
    declarations_checked: HashSet<String>,
    declarations_active: HashSet<String>,
    /// The receiver argument of an inherent `&self` method accepts `&mut T`.
    loose_receiver: bool,
    /// Module constants, in source order. A name maps to its type and value.
    consts: HashMap<String, (Ty, i64)>,
    /// Declaration locations for editor navigation and documentation.
    const_defs: HashMap<String, Span>,
    structs: Vec<StructInfo>,
    struct_names: HashMap<String, u32>,
    tuples: Vec<Vec<Ty>>,
    ptrs: Vec<RefTy>,
    /// Signatures of `extern "C" fn` types.
    cfns: Vec<FuncSig>,
    /// `let mut` patterns this function assigns from inside a closure.
    shared_spans: HashSet<usize>,
    /// A mutable capture the lowering cannot represent.
    capture_error: Option<CompileError>,
    externs: Vec<crate::typed::ExternFn>,
    /// Nested `unsafe` blocks. A reference becomes a raw pointer only inside one.
    unsafe_depth: u32,
    /// Block spans, innermost last. A binding is visible inside the top span.
    scope_spans: Vec<Span>,
    facts: Vec<BindingFact>,
    uses: Vec<NameUse>,
    expr_types: Vec<ExprType>,
    /// When set, the next pattern binding becomes visible at this offset.
    /// `let` uses the semicolon so the initializer cannot see the new name.
    next_visible: Option<usize>,
    /// The expression currently being checked. Call signatures use its span.
    current_span: Span,
    /// The next noted call signature drops its first parameter (the receiver).
    sig_skip_self: bool,
    call_sigs: Vec<CallSig>,
    /// Defining span of a struct or algebraic type name.
    type_defs: HashMap<String, Span>,
}

#[derive(Clone)]
struct StructInfo {
    name: String,
    fields: Vec<(String, Ty)>,
    field_spans: HashMap<String, Span>,
    width: u16,
    repr_c: bool,
}

#[derive(Clone)]
struct Inherent {
    type_name: String,
    method: String,
    func: String,
    receiver: Option<Recv>,
}

#[derive(Clone)]
struct ArrayInfo {
    elem: Ty,
    len: u16,
}

#[derive(Clone)]
struct RefTy {
    mutable: bool,
    inner: Ty,
}

#[derive(Clone)]
struct LiveBorrow {
    binding: u32,
    mutable: bool,
    depth: usize,
}

#[derive(Clone)]
struct FuncSig {
    params: Vec<Ty>,
    ret: Ty,
}

#[derive(Clone)]
enum IInfo {
    Concrete(Ty),
    Func { params: Vec<u32>, ret: u32 },
    Adt { schema: usize, args: Vec<u32> },
    Tuple(Vec<u32>),
    Struct(u32),
}

struct Solver {
    parent: Vec<u32>,
    info: Vec<Option<IInfo>>,
    numeric: Vec<bool>,
    /// Unbound variable introduced by a float literal. Defaults to `f64`.
    floaty: Vec<bool>,
}

impl Solver {
    fn new() -> Self {
        Self {
            parent: Vec::new(),
            info: Vec::new(),
            numeric: Vec::new(),
            floaty: Vec::new(),
        }
    }

    fn fresh(&mut self) -> u32 {
        let id = self.parent.len() as u32;
        self.parent.push(id);
        self.info.push(None);
        self.numeric.push(false);
        self.floaty.push(false);
        id
    }

    fn find(&self, mut var: u32) -> u32 {
        while self.parent[var as usize] != var {
            var = self.parent[var as usize];
        }
        var
    }

    fn concrete(&mut self, ty: Ty) -> u32 {
        let id = self.fresh();
        self.info[id as usize] = Some(IInfo::Concrete(ty));
        id
    }

    fn func(&mut self, params: Vec<u32>, ret: u32) -> u32 {
        let id = self.fresh();
        self.info[id as usize] = Some(IInfo::Func { params, ret });
        id
    }

    fn adt(&mut self, schema: usize, args: Vec<u32>) -> u32 {
        let id = self.fresh();
        self.info[id as usize] = Some(IInfo::Adt { schema, args });
        id
    }

    fn tuple(&mut self, fields: Vec<u32>) -> u32 {
        let id = self.fresh();
        self.info[id as usize] = Some(IInfo::Tuple(fields));
        id
    }

    fn structure(&mut self, id: u32) -> u32 {
        let var = self.fresh();
        self.info[var as usize] = Some(IInfo::Struct(id));
        var
    }

    fn mark_numeric(&mut self, var: u32) {
        let var = self.find(var);
        self.numeric[var as usize] = true;
    }

    fn mark_float(&mut self, var: u32) {
        let var = self.find(var);
        self.floaty[var as usize] = true;
    }

    fn unify(&mut self, a: u32, b: u32) -> Result<(), &'static str> {
        let a = self.find(a);
        let b = self.find(b);
        if a == b {
            return Ok(());
        }
        let numeric = self.numeric[a as usize] || self.numeric[b as usize];
        let floaty = self.floaty[a as usize] || self.floaty[b as usize];
        if numeric && floaty {
            return Err("incompatible types");
        }
        let a_info = self.info[a as usize].take();
        let b_info = self.info[b as usize].take();
        self.parent[a as usize] = b;
        self.numeric[b as usize] = numeric;
        self.floaty[b as usize] = floaty;
        match (a_info, b_info) {
            (None, None) => Ok(()),
            (Some(info), None) | (None, Some(info)) => {
                if numeric && !info_allows_numeric(&info) {
                    return Err("incompatible types");
                }
                if floaty && !info_allows_float(&info) {
                    return Err("incompatible types");
                }
                self.info[b as usize] = Some(info);
                Ok(())
            }
            (Some(left), Some(right)) => {
                if numeric && (!info_allows_numeric(&left) || !info_allows_numeric(&right)) {
                    return Err("incompatible types");
                }
                if floaty && (!info_allows_float(&left) || !info_allows_float(&right)) {
                    return Err("incompatible types");
                }
                self.info[b as usize] = Some(right.clone());
                self.unify_info(left, right)
            }
        }
    }

    fn unify_info(&mut self, left: IInfo, right: IInfo) -> Result<(), &'static str> {
        match (left, right) {
            (IInfo::Concrete(a), IInfo::Concrete(b)) if a == b => Ok(()),
            (IInfo::Struct(a), IInfo::Struct(b)) if a == b => Ok(()),
            (IInfo::Func { params: a, ret: ar }, IInfo::Func { params: b, ret: br }) => {
                if a.len() != b.len() {
                    return Err("incompatible types");
                }
                for (left, right) in a.into_iter().zip(b) {
                    self.unify(left, right)?;
                }
                self.unify(ar, br)
            }
            (
                IInfo::Adt {
                    schema: a,
                    args: aa,
                },
                IInfo::Adt {
                    schema: b,
                    args: ba,
                },
            ) => {
                if a != b || aa.len() != ba.len() {
                    return Err("incompatible types");
                }
                for (left, right) in aa.into_iter().zip(ba) {
                    self.unify(left, right)?;
                }
                Ok(())
            }
            (IInfo::Tuple(a), IInfo::Tuple(b)) => {
                if a.len() != b.len() {
                    return Err("incompatible types");
                }
                for (left, right) in a.into_iter().zip(b) {
                    self.unify(left, right)?;
                }
                Ok(())
            }
            _ => Err("incompatible types"),
        }
    }

    fn default_numbers(&mut self) {
        for index in 0..self.parent.len() {
            if self.parent[index] != index as u32 || self.info[index].is_some() {
                continue;
            }
            if self.floaty[index] {
                self.info[index] = Some(IInfo::Concrete(Ty::F64));
            } else if self.numeric[index] {
                self.info[index] = Some(IInfo::Concrete(Ty::I16));
            }
        }
    }
}

fn info_allows_numeric(info: &IInfo) -> bool {
    matches!(info, IInfo::Concrete(ty) if ty.is_int())
}

fn info_allows_float(info: &IInfo) -> bool {
    matches!(info, IInfo::Concrete(ty) if ty.is_float())
}

fn rewrite_layout(ty: Ty, struct_w: &[u16], tuple_w: &[u16], arrays: &[ArrayInfo]) -> Ty {
    match ty {
        Ty::Struct { id, .. } => Ty::Struct {
            id,
            width: struct_w[id as usize],
        },
        Ty::Tuple { id, .. } => Ty::Tuple {
            id,
            width: tuple_w[id as usize],
        },
        Ty::Array { id, len, .. } => {
            let elem = rewrite_layout(arrays[id as usize].elem, struct_w, tuple_w, arrays);
            let width = elem.cells();
            Ty::Array {
                id,
                len,
                width: width as u16,
            }
        }
        other => other,
    }
}

fn type_var_name(index: usize) -> String {
    match index {
        0 => "T".to_string(),
        1 => "U".to_string(),
        2 => "V".to_string(),
        3 => "W".to_string(),
        n => format!("T{n}"),
    }
}

fn escaped_ref(ty: &ParsedTy) -> bool {
    match ty {
        ParsedTy::Projection { .. } => false,
        ParsedTy::Ref { .. } => true,
        ParsedTy::Ptr { inner, .. } => escaped_ref(inner),
        ParsedTy::Array { elem, .. } => escaped_ref(elem),
        ParsedTy::Tuple { fields, .. } => fields.iter().any(escaped_ref),
        ParsedTy::Named { args, .. } => args.iter().any(escaped_ref),
        ParsedTy::Func { ret, .. } | ParsedTy::CFn { ret, .. } => escaped_ref(ret),
    }
}

fn mentions_unsolved(ty: &ParsedTy, params: &[String], solved: &[Option<Ty>]) -> bool {
    match ty {
        ParsedTy::Projection { base, .. } => mentions_unsolved(base, params, solved),
        ParsedTy::Func {
            params: fn_params,
            ret,
            ..
        }
        | ParsedTy::CFn {
            params: fn_params,
            ret,
            ..
        } => {
            fn_params
                .iter()
                .any(|param| mentions_unsolved(param, params, solved))
                || mentions_unsolved(ret, params, solved)
        }
        ParsedTy::Ref { inner, .. } => mentions_unsolved(inner, params, solved),
        ParsedTy::Ptr { inner, .. } => mentions_unsolved(inner, params, solved),
        ParsedTy::Array { elem, .. } => mentions_unsolved(elem, params, solved),
        ParsedTy::Tuple { fields, .. } => fields
            .iter()
            .any(|field| mentions_unsolved(field, params, solved)),
        ParsedTy::Named { name, args, .. } => {
            if let Some(index) = params.iter().position(|param| param == name) {
                return solved[index].is_none();
            }
            args.iter()
                .any(|arg| mentions_unsolved(arg, params, solved))
        }
    }
}

fn arm_span(pat: &Pat) -> usize {
    pat.span().start
}

fn pat_names(pat: &Pat, out: &mut Vec<(String, Span)>) {
    match pat {
        Pat::Name { name, span } if name != "_" => out.push((name.clone(), *span)),
        Pat::Name { .. } | Pat::Int { .. } | Pat::Bool { .. } | Pat::Char { .. } => {}
        Pat::Ctor { fields, .. } | Pat::Tuple { fields, .. } => {
            for field in fields {
                pat_names(field, out);
            }
        }
        Pat::Struct { fields, .. } => {
            for (_, _, field) in fields {
                pat_names(field, out);
            }
        }
    }
}
