use std::collections::{HashMap, HashSet};

use crate::ast::{
    Adt, BinOp, Block, Ctor, Expr, ExprKind, Function, Impl, Module, Param, ParsedTy, Pat, Recv,
    Span, Stmt, TypeParam, UnaryOp,
};
use crate::error::CompileError;
use crate::ty::Ty;
use crate::typed::*;

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

struct AdtSchema {
    name: String,
    params: Vec<String>,
    ctors: Vec<Vec<ParsedTy>>,
}

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

pub struct ProgramInfo {
    pub facts: Vec<BindingFact>,
    pub uses: Vec<NameUse>,
    pub expr_types: Vec<ExprType>,
    /// `(type name, method name)` for every inherent method that was checked.
    pub methods: Vec<(String, String)>,
    pub call_sigs: Vec<CallSig>,
}

pub fn check(source: &str, module: &Module) -> Result<TModule, CompileError> {
    analyze(source, module).1
}

pub fn analyze(source: &str, module: &Module) -> (ProgramInfo, Result<TModule, CompileError>) {
    let mut checker = Checker {
        source,
        sigs: HashMap::new(),
        scopes: Vec::new(),
        bindings: Vec::new(),
        fn_ret: Ty::Unit,
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
        arrays: Vec::new(),
        slice_elems: Vec::new(),
        refs: Vec::new(),
        borrows: Vec::new(),
        methods: Vec::new(),
        loose_receiver: false,
        consts: HashMap::new(),
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
    let result = (|| -> Result<TModule, CompileError> {
        let mut owned = module.clone();
        owned.adts.insert(0, prelude_option());
        owned.adts.insert(0, prelude_result());
        checker.methods = expand_impls(source, &mut owned)?;
        checker.ast_fns = owned.functions.clone();
        checker.eval_consts(&owned.consts)?;
        checker.register(&mut owned)?;
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
            checker.check_fn(func, &func.name, true, false)?;
        }
        let structs = checker
            .structs
            .iter()
            .map(|info| info.fields.iter().map(|(_, ty)| *ty).collect())
            .collect();
        let repr_c = checker.structs.iter().map(|info| info.repr_c).collect();
        Ok(TModule {
            functions: std::mem::take(&mut checker.typed),
            adts: std::mem::take(&mut checker.adts),
            structs,
            repr_c,
            tuples: std::mem::take(&mut checker.tuples),
            externs: std::mem::take(&mut checker.externs),
        })
    })();
    let info = ProgramInfo {
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

fn prelude_result() -> Adt {
    let span = Span::empty();
    let value = ParsedTy::Named {
        span,
        name: "T".to_string(),
        args: Vec::new(),
    };
    let error = ParsedTy::Named {
        span,
        name: "E".to_string(),
        args: Vec::new(),
    };
    Adt {
        name: "Result".to_string(),
        name_span: span,
        params: vec![
            TypeParam {
                name: "T".to_string(),
                span,
            },
            TypeParam {
                name: "E".to_string(),
                span,
            },
        ],
        ctors: vec![
            Ctor {
                name: "Ok".to_string(),
                name_span: span,
                fields: vec![value],
            },
            Ctor {
                name: "Err".to_string(),
                name_span: span,
                fields: vec![error],
            },
        ],
        exported: false,
    }
}

fn expand_impls(source: &str, module: &mut Module) -> Result<Vec<Inherent>, CompileError> {
    let impls = std::mem::take(&mut module.impls);
    let mut methods = Vec::new();
    for item in impls {
        let type_name = impl_type_name(source, &item)?;
        for method in item.items {
            if methods
                .iter()
                .any(|found: &Inherent| found.type_name == type_name && found.method == method.name)
            {
                return Err(CompileError::at(
                    source,
                    method.name_span.start,
                    format!("duplicate method `{type_name}::{}`", method.name),
                ));
            }
            if type_name == "Vec"
                && matches!(
                    method.name.as_str(),
                    "len" | "push" | "is_empty" | "clear" | "pop" | "get"
                )
            {
                return Err(CompileError::at(
                    source,
                    method.name_span.start,
                    format!("`Vec` already has `{}`", method.name),
                ));
            }
            let func = format!("{type_name}${}", method.name);
            let mut type_params = item.type_params.clone();
            type_params.extend(method.type_params.iter().cloned());
            let mut params = Vec::new();
            if let Some(recv) = method.receiver {
                let ty = match recv {
                    Recv::Value => item.ty.clone(),
                    Recv::Shared => ParsedTy::Ref {
                        span: method.name_span,
                        mutable: false,
                        inner: Box::new(item.ty.clone()),
                    },
                    Recv::Mut => ParsedTy::Ref {
                        span: method.name_span,
                        mutable: true,
                        inner: Box::new(item.ty.clone()),
                    },
                };
                params.push(Param {
                    name: "self".to_string(),
                    name_span: method.name_span,
                    ty: Some(ty),
                    pat: Pat::Name {
                        name: "self".to_string(),
                        span: method.name_span,
                    },
                });
            }
            params.extend(method.params.iter().cloned());
            let ret = method.ret.or_else(|| {
                if method.body.tail.is_none() {
                    Some(ParsedTy::Named {
                        span: method.name_span,
                        name: "unit".to_string(),
                        args: Vec::new(),
                    })
                } else {
                    None
                }
            });
            module.functions.push(Function {
                name: func.clone(),
                name_span: method.name_span,
                type_params,
                params,
                ret,
                body: method.body,
                c_export: false,
                exported: false,
            });
            methods.push(Inherent {
                type_name: type_name.clone(),
                method: method.name,
                func,
                receiver: method.receiver,
            });
        }
    }
    Ok(methods)
}

fn impl_type_name(source: &str, item: &Impl) -> Result<String, CompileError> {
    let ParsedTy::Named { span, name, args } = &item.ty else {
        return Err(CompileError::at(
            source,
            item.span.start,
            "an impl type must be a named type",
        ));
    };
    if item.type_params.is_empty() {
        return Ok(name.clone());
    }
    if args.len() != item.type_params.len() {
        return Err(CompileError::at(
            source,
            span.start,
            format!("`{name}` expects {} type arguments", item.type_params.len()),
        ));
    }
    for (arg, param) in args.iter().zip(&item.type_params) {
        let ok = matches!(arg, ParsedTy::Named { name: arg_name, args, .. } if args.is_empty() && arg_name == &param.name);
        if !ok {
            return Err(CompileError::at(
                source,
                arg.span().start,
                "a generic impl uses its type parameters in order",
            ));
        }
    }
    Ok(name.clone())
}

fn prelude_option() -> Adt {
    let span = Span::empty();
    let param = ParsedTy::Named {
        span,
        name: "T".to_string(),
        args: Vec::new(),
    };
    Adt {
        name: "Option".to_string(),
        name_span: span,
        params: vec![TypeParam {
            name: "T".to_string(),
            span,
        }],
        ctors: vec![
            Ctor {
                name: "None".to_string(),
                name_span: span,
                fields: Vec::new(),
            },
            Ctor {
                name: "Some".to_string(),
                name_span: span,
                fields: vec![param],
            },
        ],
        exported: false,
    }
}

fn is_builtin(name: &str) -> bool {
    matches!(
        name,
        "print" | "println" | "putc" | "from_char" | "from_byte"
    )
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

struct Checker<'a> {
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
    arrays: Vec<ArrayInfo>,
    slice_elems: Vec<Ty>,
    refs: Vec<RefTy>,
    borrows: Vec<LiveBorrow>,
    methods: Vec<Inherent>,
    /// The receiver argument of an inherent `&self` method accepts `&mut T`.
    loose_receiver: bool,
    /// Module constants, in source order. A name maps to its type and value.
    consts: HashMap<String, (Ty, i64)>,
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

struct StructInfo {
    name: String,
    fields: Vec<(String, Ty)>,
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

struct ArrayInfo {
    elem: Ty,
    len: u16,
}

struct RefTy {
    mutable: bool,
    inner: Ty,
}

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
            let width = len as i64 * elem.cells();
            Ty::Array {
                id,
                len,
                width: width as u16,
            }
        }
        other => other,
    }
}

fn signature_accepts_receiver(signature: &str, receiver: &str) -> bool {
    let Some(inside) = signature.strip_prefix("fn(") else {
        return false;
    };
    let Some(close) = close_paren(inside) else {
        return false;
    };
    let first = split_top(&inside[..close]).into_iter().next().unwrap_or("");
    first == receiver || first == format!("&{receiver}") || first == format!("&mut {receiver}")
}

fn close_paren(text: &str) -> Option<usize> {
    let mut depth = 0i32;
    for (index, ch) in text.char_indices() {
        match ch {
            '(' | '<' | '[' => depth += 1,
            ')' if depth == 0 => return Some(index),
            ')' | '>' | ']' => depth -= 1,
            _ => {}
        }
    }
    None
}

fn split_top(text: &str) -> Vec<&str> {
    let mut parts = Vec::new();
    let mut depth = 0i32;
    let mut start = 0usize;
    for (index, ch) in text.char_indices() {
        match ch {
            '(' | '<' | '[' => depth += 1,
            ')' | '>' | ']' => depth -= 1,
            ',' if depth == 0 => {
                parts.push(text[start..index].trim());
                start = index + 1;
            }
            _ => {}
        }
    }
    let tail = text[start..].trim();
    if !tail.is_empty() {
        parts.push(tail);
    }
    parts
}

fn parsed_brief(ty: &ParsedTy) -> String {
    match ty {
        ParsedTy::Named { name, args, .. } if args.is_empty() => name.clone(),
        ParsedTy::Named { name, args, .. } => {
            format!(
                "{name}<{}>",
                args.iter().map(parsed_brief).collect::<Vec<_>>().join(", ")
            )
        }
        ParsedTy::Ref { mutable, inner, .. } => format!(
            "{}{}",
            if *mutable { "&mut " } else { "&" },
            parsed_brief(inner)
        ),
        ParsedTy::Ptr { mutable, inner, .. } => format!(
            "{}{}",
            if *mutable { "*mut " } else { "*const " },
            parsed_brief(inner)
        ),
        ParsedTy::Func { params, ret, .. } => {
            format!(
                "fn({}) -> {}",
                params
                    .iter()
                    .map(parsed_brief)
                    .collect::<Vec<_>>()
                    .join(", "),
                parsed_brief(ret)
            )
        }
        ParsedTy::CFn { params, ret, .. } => {
            format!(
                "extern \"C\" fn({}) -> {}",
                params
                    .iter()
                    .map(parsed_brief)
                    .collect::<Vec<_>>()
                    .join(", "),
                parsed_brief(ret)
            )
        }
        ParsedTy::Tuple { fields, .. } => format!(
            "({})",
            fields
                .iter()
                .map(parsed_brief)
                .collect::<Vec<_>>()
                .join(", ")
        ),
        ParsedTy::Array { elem, .. } => format!("[{}; _]", parsed_brief(elem)),
    }
}

fn parsed_signature(func: &Function, skip_first: bool) -> String {
    let mut params: Vec<String> = func
        .params
        .iter()
        .map(|param| {
            param
                .ty
                .as_ref()
                .map(parsed_brief)
                .unwrap_or_else(|| "_".to_string())
        })
        .collect();
    if skip_first && !params.is_empty() {
        params.remove(0);
    }
    let ret = func
        .ret
        .as_ref()
        .map(parsed_brief)
        .unwrap_or_else(|| "()".to_string());
    format!("fn({}) -> {ret}", params.join(", "))
}

fn literal_int(expr: &Expr) -> Option<i64> {
    match &expr.kind {
        ExprKind::Int(value) => Some(*value),
        ExprKind::Unary {
            op: UnaryOp::Neg,
            expr,
        } => literal_int(expr).map(|value| value.wrapping_neg()),
        _ => None,
    }
}

/// `0..10` stays `i16`. `0..44100` is `i32`, because 44100 does not fit in i16.
fn signed_fit(start: i64, end: i64) -> Ty {
    for ty in [Ty::I16, Ty::I32, Ty::I64] {
        if ty.in_range(start) && ty.in_range(end) {
            return ty;
        }
    }
    Ty::I64
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

impl<'a> Checker<'a> {
    fn bind(&mut self, name: String, ty: Ty, mutable: bool) -> u32 {
        self.bind_spanned(name, ty, mutable, Span::empty())
    }

    fn bind_spanned(&mut self, name: String, ty: Ty, mutable: bool, def: Span) -> u32 {
        let id = self.bindings.len() as u32;
        self.bindings.push(Binding {
            ty,
            mutable,
            shared: false,
            def,
        });
        self.scopes.last_mut().unwrap().insert(name.clone(), id);
        if def.end > def.start && !name.starts_with("__") {
            let scope = self.scope_spans.last().copied().unwrap_or(def);
            let visible_from = self.next_visible.take().unwrap_or(def.end);
            let shown = self.show(ty);
            self.facts.push(BindingFact {
                name,
                def,
                ty: shown,
                scope,
                visible_from,
                variant: false,
            });
        }
        id
    }

    fn lookup(&self, name: &str) -> Option<u32> {
        for scope in self.scopes.iter().rev() {
            if let Some(id) = scope.get(name) {
                return Some(*id);
            }
        }
        None
    }

    /// A name from an enclosing function becomes an immutable local.
    /// The value is copied when the closure is created.
    fn resolve(&mut self, name: &str) -> Option<u32> {
        if let Some(id) = self.lookup(name) {
            return Some(id);
        }
        let parent = self
            .outer_scopes
            .iter()
            .rev()
            .find_map(|scope| scope.get(name).copied())?;
        if let Some(found) = self.captures.iter().find(|cap| cap.parent == parent) {
            self.scopes
                .last_mut()
                .unwrap()
                .insert(name.to_string(), found.local);
            return Some(found.local);
        }
        let parent_binding = self.outer_bindings[parent as usize].clone();
        let ty = parent_binding.ty;
        if parent_binding.shared && !shareable(ty) && self.capture_error.is_none() {
            self.capture_error = Some(CompileError::at(
                self.source,
                0,
                format!("`{}` cannot be captured mutably", self.show(ty)),
            ));
        }
        let local = self.bind(name.to_string(), ty, parent_binding.shared);
        if parent_binding.shared {
            self.bindings[local as usize].shared = true;
        }
        self.captures.push(crate::typed::Capture {
            parent,
            local,
            ty,
            shared: parent_binding.shared,
        });
        Some(local)
    }

    fn register(&mut self, module: &mut Module) -> Result<(), CompileError> {
        self.declare_structs(&module.structs)?;
        for adt in &module.adts {
            if self.schemas.iter().any(|schema| schema.name == adt.name)
                || self.struct_names.contains_key(&adt.name)
            {
                return Err(CompileError::at(
                    self.source,
                    adt.name_span.start,
                    format!("duplicate type `{}`", adt.name),
                ));
            }
            let mut params = Vec::new();
            for param in &adt.params {
                if params.contains(&param.name) {
                    return Err(CompileError::at(
                        self.source,
                        param.span.start,
                        format!("duplicate type parameter `{}`", param.name),
                    ));
                }
                if Ty::parse_name(&param.name).is_some() {
                    return Err(CompileError::at(
                        self.source,
                        param.span.start,
                        format!("`{}` is not a valid type parameter", param.name),
                    ));
                }
                params.push(param.name.clone());
            }
            self.schemas.push(AdtSchema {
                name: adt.name.clone(),
                params,
                ctors: Vec::new(),
            });
            if adt.name_span.end > adt.name_span.start {
                self.type_defs.insert(adt.name.clone(), adt.name_span);
            }
        }
        for (index, adt) in module.adts.iter().enumerate() {
            for ctor in &adt.ctors {
                if is_builtin(&ctor.name) {
                    return Err(CompileError::at(
                        self.source,
                        ctor.name_span.start,
                        format!("`{}` is a builtin", ctor.name),
                    ));
                }
                if self.ctor_schema.contains_key(&ctor.name) {
                    return Err(CompileError::at(
                        self.source,
                        ctor.name_span.start,
                        format!("duplicate constructor `{}`", ctor.name),
                    ));
                }
                for field in &ctor.fields {
                    if escaped_ref(field) {
                        return Err(CompileError::at(
                            self.source,
                            field.span().start,
                            "a reference cannot be stored in a type",
                        ));
                    }
                    self.validate_ty(field, &self.schemas[index].params.clone())?;
                }
                let tag = self.schemas[index].ctors.len() as u16;
                self.schemas[index].ctors.push(ctor.fields.clone());
                self.ctor_schema.insert(ctor.name.clone(), (index, tag));
                if ctor.name_span.end > ctor.name_span.start {
                    let ret = if adt.params.is_empty() {
                        adt.name.clone()
                    } else {
                        format!(
                            "{}<{}>",
                            adt.name,
                            adt.params
                                .iter()
                                .map(|param| param.name.as_str())
                                .collect::<Vec<_>>()
                                .join(", ")
                        )
                    };
                    let ty = if ctor.fields.is_empty() {
                        ret
                    } else {
                        format!(
                            "fn({}) -> {ret}",
                            ctor.fields
                                .iter()
                                .map(parsed_brief)
                                .collect::<Vec<_>>()
                                .join(", ")
                        )
                    };
                    self.facts.push(BindingFact {
                        name: ctor.name.clone(),
                        def: ctor.name_span,
                        ty,
                        scope: Span::new(0, self.source.len()),
                        visible_from: 0,
                        variant: true,
                    });
                }
            }
        }
        self.finish_structs(&module.structs)?;
        let plain: Vec<usize> = self
            .schemas
            .iter()
            .enumerate()
            .filter(|(_, schema)| schema.params.is_empty())
            .map(|(index, _)| index)
            .collect();
        for index in plain {
            self.monomorphize(index, &[])?;
        }
        self.infer_omitted(&mut module.functions)?;
        self.ast_fns = module.functions.clone();
        self.register_externs(&module.externs)?;
        for (index, func) in module.functions.iter().enumerate() {
            if is_builtin(&func.name) {
                return Err(CompileError::at(
                    self.source,
                    func.name_span.start,
                    format!("`{}` is a builtin", func.name),
                ));
            }
            if self.sigs.contains_key(&func.name)
                || self.fn_schemas.contains_key(&func.name)
                || self.ctor_schema.contains_key(&func.name)
            {
                return Err(CompileError::at(
                    self.source,
                    func.name_span.start,
                    format!("duplicate function `{}`", func.name),
                ));
            }
            let mut type_params = Vec::new();
            for param in &func.type_params {
                if type_params.contains(&param.name) {
                    return Err(CompileError::at(
                        self.source,
                        param.span.start,
                        format!("duplicate type parameter `{}`", param.name),
                    ));
                }
                if Ty::parse_name(&param.name).is_some() {
                    return Err(CompileError::at(
                        self.source,
                        param.span.start,
                        format!("`{}` is not a valid type parameter", param.name),
                    ));
                }
                type_params.push(param.name.clone());
            }
            for param in &func.params {
                let Some(ty) = &param.ty else {
                    return Err(CompileError::at(
                        self.source,
                        param.name_span.start,
                        format!("parameter `{}` needs a type", param.name),
                    ));
                };
                self.validate_ty(ty, &type_params)?;
            }
            if let Some(ret) = &func.ret {
                if escaped_ref(ret) {
                    return Err(CompileError::at(
                        self.source,
                        ret.span().start,
                        "a function cannot return a reference",
                    ));
                }
                self.validate_ty(ret, &type_params)?;
            }
            if func.c_export {
                if !type_params.is_empty() {
                    return Err(CompileError::at(
                        self.source,
                        func.name_span.start,
                        "an exported function cannot be generic",
                    ));
                }
                if func.name == "main" {
                    return Err(CompileError::at(
                        self.source,
                        func.name_span.start,
                        "cannot export `main`",
                    ));
                }
            }
            if !type_params.is_empty() {
                if func.name == "main" {
                    return Err(CompileError::at(
                        self.source,
                        func.name_span.start,
                        "main cannot be generic",
                    ));
                }
                if func.ret.is_none() {
                    return Err(CompileError::at(
                        self.source,
                        func.name_span.start,
                        format!("generic function `{}` needs a return type", func.name),
                    ));
                }
                self.fn_schemas
                    .insert(func.name.clone(), FnSchema { index, type_params });
                self.note_fn_fact(func);
                continue;
            }
            let mut params = Vec::new();
            for param in &func.params {
                params.push(self.apply(param.ty.as_ref().unwrap(), &HashMap::new())?);
            }
            let ret = match &func.ret {
                Some(ty) => Some(self.apply(ty, &HashMap::new())?),
                None => None,
            };
            self.sigs.insert(
                func.name.clone(),
                Sig {
                    params,
                    ret,
                    c_abi: func.c_export,
                    variadic: false,
                },
            );
            self.note_fn_fact(func);
        }
        Ok(())
    }

    fn note_fn_fact(&mut self, func: &Function) {
        if func.name_span.end <= func.name_span.start {
            return;
        }
        let written = self
            .source
            .get(func.name_span.start..func.name_span.end)
            .unwrap_or(func.name.as_str())
            .to_string();
        self.facts.push(BindingFact {
            name: written,
            def: func.name_span,
            ty: parsed_signature(func, false),
            scope: Span::new(0, self.source.len()),
            visible_from: 0,
            variant: false,
        });
    }

    fn register_externs(&mut self, decls: &[crate::ast::ExternDecl]) -> Result<(), CompileError> {
        for decl in decls {
            if is_builtin(&decl.name) {
                return Err(CompileError::at(
                    self.source,
                    decl.name_span.start,
                    format!("`{}` is a builtin", decl.name),
                ));
            }
            if self.sigs.contains_key(&decl.name)
                || self.fn_schemas.contains_key(&decl.name)
                || self.ctor_schema.contains_key(&decl.name)
            {
                return Err(CompileError::at(
                    self.source,
                    decl.name_span.start,
                    format!("duplicate function `{}`", decl.name),
                ));
            }
            let mut params = Vec::new();
            for param in &decl.params {
                let Some(ty) = &param.ty else {
                    return Err(CompileError::at(
                        self.source,
                        param.name_span.start,
                        format!("parameter `{}` needs a type", param.name),
                    ));
                };
                self.validate_ty(ty, &[])?;
                let ty = self.apply(ty, &HashMap::new())?;
                self.require_c_type(param.name_span.start, ty, false)?;
                params.push(ty);
            }
            let ret = match &decl.ret {
                Some(ty) => {
                    self.validate_ty(ty, &[])?;
                    let ty = self.apply(ty, &HashMap::new())?;
                    self.require_c_type(decl.name_span.start, ty, true)?;
                    ty
                }
                None => Ty::Unit,
            };
            self.externs.push(crate::typed::ExternFn {
                name: decl.name.clone(),
                params: params.clone(),
                ret,
                variadic: decl.variadic,
            });
            let ret = if ret == Ty::Unit { None } else { Some(ret) };
            self.sigs.insert(
                decl.name.clone(),
                Sig {
                    params,
                    ret,
                    c_abi: true,
                    variadic: decl.variadic,
                },
            );
            if decl.name_span.end > decl.name_span.start {
                let shown = self.sigs[&decl.name]
                    .params
                    .iter()
                    .map(|ty| self.show(*ty))
                    .collect::<Vec<_>>()
                    .join(", ");
                let ret = self.sigs[&decl.name]
                    .ret
                    .map(|ty| self.show(ty))
                    .unwrap_or_else(|| "()".to_string());
                self.facts.push(BindingFact {
                    name: decl.name.clone(),
                    def: decl.name_span,
                    ty: format!("extern \"C\" fn({shown}) -> {ret}"),
                    scope: Span::new(0, self.source.len()),
                    visible_from: 0,
                    variant: false,
                });
            }
        }
        Ok(())
    }

    fn require_c_type(&self, span: usize, ty: Ty, is_return: bool) -> Result<(), CompileError> {
        if is_return && ty == Ty::Unit {
            return Ok(());
        }
        if ty.is_int() || ty.is_float() {
            return Ok(());
        }
        if self.is_repr_struct(ty) {
            return Ok(());
        }
        if let Ty::Ptr(id) = ty {
            let inner = self.ptrs[id as usize].inner;
            if self.c_pointee(inner) {
                return Ok(());
            }
        }
        if matches!(ty, Ty::CFn(_)) {
            return Ok(());
        }
        Err(CompileError::at(
            self.source,
            span,
            format!("`{}` cannot be used in a C signature", self.show(ty)),
        ))
    }

    fn is_repr_struct(&self, ty: Ty) -> bool {
        matches!(ty, Ty::Struct { id, .. } if self.structs.get(id as usize).is_some_and(|info| info.repr_c))
    }

    fn c_pointee(&self, ty: Ty) -> bool {
        ty.is_int() || ty.is_float() || self.is_repr_struct(ty)
    }

    /// `&mut T` may become `*mut T` or `*const T`. `&T` may become `*const T`.
    fn ptr_from_ref(&self, want: Ty, got: Ty) -> bool {
        let Ty::Ptr(want_id) = want else { return false };
        let Ty::Ref(got_id) = got else { return false };
        let want_ptr = &self.ptrs[want_id as usize];
        let got_ref = &self.refs[got_id as usize];
        want_ptr.inner == got_ref.inner && (!want_ptr.mutable || got_ref.mutable)
    }

    fn promote_vararg(&self, span: usize, expr: TExpr) -> Result<TExpr, CompileError> {
        if expr.ty == Ty::F32 {
            return Ok(TExpr {
                ty: Ty::F64,
                kind: TExprKind::Cast {
                    expr: Box::new(expr),
                    to: Ty::F64,
                },
            });
        }
        if matches!(expr.ty, Ty::I8 | Ty::U8) {
            return Ok(TExpr {
                ty: Ty::I16,
                kind: TExprKind::Cast {
                    expr: Box::new(expr),
                    to: Ty::I16,
                },
            });
        }
        if expr.ty.is_int()
            || expr.ty.is_float()
            || matches!(expr.ty, Ty::Ptr(_))
            || self.is_repr_struct(expr.ty)
        {
            return Ok(expr);
        }
        Err(CompileError::at(
            self.source,
            span,
            format!(
                "`{}` cannot be passed to a variadic function",
                self.show(expr.ty)
            ),
        ))
    }

    fn coerce_raw(&self, span: usize, expr: TExpr, want: Ty) -> Result<TExpr, CompileError> {
        if expr.ty == want {
            return Ok(expr);
        }
        if self.ptr_from_ref(want, expr.ty) {
            if self.unsafe_depth == 0 {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "converting a reference to a raw pointer needs `unsafe`",
                ));
            }
            return Ok(TExpr {
                ty: want,
                kind: expr.kind,
            });
        }
        Ok(expr)
    }

    fn validate_ty(&self, ty: &ParsedTy, params: &[String]) -> Result<(), CompileError> {
        match ty {
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
                for param in fn_params {
                    self.validate_ty(param, params)?;
                }
                self.validate_ty(ret, params)
            }
            ParsedTy::Ref { inner, .. } => self.validate_ty(inner, params),
            ParsedTy::Ptr { inner, .. } => self.validate_ty(inner, params),
            ParsedTy::Array { elem, .. } => self.validate_ty(elem, params),
            ParsedTy::Tuple { fields, .. } => {
                for field in fields {
                    self.validate_ty(field, params)?;
                }
                Ok(())
            }
            ParsedTy::Named { span, name, args } => {
                if params.iter().any(|param| param == name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("type parameter `{name}` does not take arguments"),
                        ));
                    }
                    return Ok(());
                }
                if Ty::parse_name(name).is_some() {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(());
                }
                if name == "Vec" || name == "Slice" {
                    if args.len() != 1 {
                        return Err(self.arity_error(span.start, name, 1, args.len()));
                    }
                    return self.validate_ty(&args[0], params);
                }
                if self.struct_names.contains_key(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(());
                }
                let Some(schema) = self.schemas.iter().find(|schema| schema.name == *name) else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown type `{name}`"),
                    ));
                };
                if args.len() != schema.params.len() {
                    return Err(self.arity_error(
                        span.start,
                        name,
                        schema.params.len(),
                        args.len(),
                    ));
                }
                for arg in args {
                    self.validate_ty(arg, params)?;
                }
                Ok(())
            }
        }
    }

    fn arity_error(&self, offset: usize, name: &str, expect: usize, found: usize) -> CompileError {
        if expect == 0 {
            CompileError::at(
                self.source,
                offset,
                format!("`{name}` does not take type arguments"),
            )
        } else {
            CompileError::at(
                self.source,
                offset,
                format!("`{name}` expects {expect} type arguments, found {found}"),
            )
        }
    }

    fn check_fn(
        &mut self,
        func: &Function,
        name: &str,
        closure: bool,
        capture: bool,
    ) -> Result<(), CompileError> {
        let saved = Suspended {
            scopes: std::mem::take(&mut self.scopes),
            bindings: std::mem::take(&mut self.bindings),
            fn_ret: self.fn_ret,
            loops: self.loops,
        };
        let saved_outer_scopes = std::mem::take(&mut self.outer_scopes);
        let saved_outer_bindings = std::mem::take(&mut self.outer_bindings);
        let saved_captures = std::mem::take(&mut self.captures);
        let saved_shared = std::mem::take(&mut self.shared_spans);
        self.shared_spans = shared_assign_spans(&func.body);
        let saved_borrows = std::mem::take(&mut self.borrows);
        let saved_loose = self.loose_receiver;
        self.loose_receiver = false;
        if capture {
            self.outer_scopes = saved.scopes.clone();
            self.outer_bindings = saved.bindings.clone();
        }
        self.scopes = vec![HashMap::new()];
        self.loops = 0;
        let declared = self.sigs[name].ret;
        self.fn_ret = declared.unwrap_or(Ty::Unit);
        let mut params = Vec::new();
        let mut seen = HashMap::new();
        let mut prefix = Vec::new();
        for (index, param) in func.params.iter().enumerate() {
            let ty = self.sigs[name].params[index];
            let simple = matches!(&param.pat, Pat::Name { name, .. } if name != "_");
            if simple {
                if seen.contains_key(&param.name) {
                    return Err(CompileError::at(
                        self.source,
                        param.name_span.start,
                        format!("duplicate parameter `{}`", param.name),
                    ));
                }
                seen.insert(param.name.clone(), ());
                let id = self.bind(param.name.clone(), ty, false);
                self.bindings[id as usize].def = param.name_span;
                let shown = self.show(ty);
                self.facts.push(BindingFact {
                    name: param.name.clone(),
                    def: param.name_span,
                    ty: shown,
                    scope: func.body.span,
                    visible_from: func.body.span.start,
                    variant: false,
                });
                params.push(id);
            } else {
                let mut names = Vec::new();
                pat_names(&param.pat, &mut names);
                for (bound, span) in &names {
                    if seen.contains_key(bound) {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("duplicate parameter `{bound}`"),
                        ));
                    }
                    seen.insert(bound.clone(), ());
                }
                let hidden = self.bind(format!("__arg{index}"), ty, false);
                params.push(hidden);
                self.scope_spans.push(func.body.span);
                self.next_visible = Some(func.body.span.start);
                let pat = self.check_pat(&param.pat, ty, false);
                self.scope_spans.pop();
                self.next_visible = None;
                let pat = pat?;
                if !self.irrefutable_pat(&pat, ty) {
                    return Err(CompileError::at(
                        self.source,
                        param.name_span.start,
                        "a parameter pattern must be irrefutable",
                    ));
                }
                prefix.push(TStmt::Destructure {
                    pat,
                    init: TExpr {
                        ty,
                        kind: TExprKind::Name(hidden),
                    },
                });
            }
        }
        let ret = declared.unwrap_or_else(|| self.peek_block(&func.body).unwrap_or(Ty::I16));
        if func.name == "main" && !matches!(ret, Ty::Unit | Ty::I16) {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                format!("main returns unit or i16, not {}", self.show(ret)),
            ));
        }
        self.fn_ret = ret;
        if ret.is_ref() {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                "a function cannot return a reference",
            ));
        }
        self.sigs.get_mut(name).unwrap().ret = Some(ret);
        if func.type_params.is_empty() && func.name_span.end > func.name_span.start {
            let params = self.sigs[name]
                .params
                .iter()
                .map(|ty| self.show(*ty))
                .collect::<Vec<_>>()
                .join(", ");
            let signature = format!("fn({params}) -> {}", self.show(ret));
            if let Some(fact) = self
                .facts
                .iter_mut()
                .find(|fact| fact.def == func.name_span)
            {
                fact.ty = signature;
            }
        }
        let mut body = self.check_block(&func.body, Some(ret))?;
        for (index, stmt) in prefix.into_iter().enumerate() {
            body.stmts.insert(index, stmt);
        }
        let body_ty = body.tail.as_ref().map(|expr| expr.ty).unwrap_or(Ty::Unit);
        if func.body.tail.is_none() && ret != Ty::Unit && !always_returns(&func.body) {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                format!("`{}` must return {}", func.name, self.show(ret)),
            ));
        }
        if func.body.tail.is_some() && body_ty != ret {
            return Err(CompileError::at(
                self.source,
                func.name_span.start,
                format!(
                    "`{}` returns {}, the body has type {}",
                    func.name,
                    self.show(ret),
                    self.show(body_ty)
                ),
            ));
        }
        if let Some(err) = self.capture_error.take() {
            return Err(err);
        }
        let shared: Vec<bool> = self.bindings.iter().map(|binding| binding.shared).collect();
        let bindings = self.bindings.iter().map(|binding| binding.ty).collect();
        if func.c_export {
            let params = self.sigs[name].params.clone();
            for ty in params {
                self.require_c_type(func.name_span.start, ty, false)?;
            }
            self.require_c_type(func.name_span.start, ret, true)?;
        }
        let captures = std::mem::take(&mut self.captures);
        self.typed.push(TFn {
            name: name.to_string(),
            params,
            ret,
            bindings,
            body,
            closure,
            captures,
            shared,
            c_export: func.c_export,
        });
        self.scopes = saved.scopes;
        self.bindings = saved.bindings;
        self.fn_ret = saved.fn_ret;
        self.loops = saved.loops;
        self.outer_scopes = saved_outer_scopes;
        self.outer_bindings = saved_outer_bindings;
        self.captures = saved_captures;
        self.shared_spans = saved_shared;
        self.borrows = saved_borrows;
        self.loose_receiver = saved_loose;
        Ok(())
    }

    fn instantiate(&mut self, name: &str, args: &[Ty]) -> Result<String, CompileError> {
        let key = (name.to_string(), args.to_vec());
        if let Some(mangled) = self.fn_mono.get(&key) {
            return Ok(mangled.clone());
        }
        let mangled = self.mangle(name, args);
        self.fn_mono.insert(key, mangled.clone());
        let index = self.fn_schemas[name].index;
        let type_params = self.fn_schemas[name].type_params.clone();
        let func = self.ast_fns[index].clone();
        let mut subst = HashMap::new();
        for (param, arg) in type_params.iter().zip(args) {
            subst.insert(param.clone(), *arg);
        }
        let mut params = Vec::new();
        for param in &func.params {
            params.push(self.apply(param.ty.as_ref().unwrap(), &subst)?);
        }
        let ret = self.apply(func.ret.as_ref().unwrap(), &subst)?;
        self.sigs.insert(
            mangled.clone(),
            Sig {
                params,
                ret: Some(ret),
                c_abi: false,
                variadic: false,
            },
        );
        self.check_fn(&func, &mangled, true, false)?;
        Ok(mangled)
    }

    fn mangle(&self, name: &str, args: &[Ty]) -> String {
        let mut mangled = name.to_string();
        for arg in args {
            mangled.push('$');
            mangled.push_str(&self.mangle_ty(*arg));
        }
        mangled
    }

    fn mangle_ty(&self, ty: Ty) -> String {
        match ty {
            Ty::Adt(id) => {
                let adt = &self.adts[id as usize];
                let mut text = self.schemas[adt.schema].name.clone();
                for arg in &adt.args {
                    text.push('$');
                    text.push_str(&self.mangle_ty(*arg));
                }
                text.push('$');
                text
            }
            Ty::Vec(id) => format!("Vec${}$", self.mangle_ty(self.vec_elems[id as usize])),
            Ty::Slice(id) => format!("Slice${}$", self.mangle_ty(self.slice_elems[id as usize])),
            Ty::Array { id, .. } => {
                let info = &self.arrays[id as usize];
                format!("arr${}${}$", info.len, self.mangle_ty(info.elem))
            }
            Ty::Ref(id) => {
                let refty = &self.refs[id as usize];
                let prefix = if refty.mutable { "refmut$" } else { "ref$" };
                format!("{prefix}{}", self.mangle_ty(refty.inner))
            }
            Ty::Ptr(id) => {
                let ptr = &self.ptrs[id as usize];
                let prefix = if ptr.mutable { "ptrmut$" } else { "ptr$" };
                format!("{prefix}{}$", self.mangle_ty(ptr.inner))
            }
            Ty::Func(id) => {
                let sig = &self.funcs[id as usize];
                let mut text = String::from("fn");
                for param in &sig.params {
                    text.push('$');
                    text.push_str(&self.mangle_ty(*param));
                }
                text.push('$');
                text.push_str(&self.mangle_ty(sig.ret));
                text.push('$');
                text
            }
            Ty::Struct { id, .. } => format!("{}$", self.structs[id as usize].name),
            Ty::Tuple { id, .. } => {
                let mut text = String::from("tup");
                for field in &self.tuples[id as usize] {
                    text.push('$');
                    text.push_str(&self.mangle_ty(*field));
                }
                text.push('$');
                text
            }
            other => other.to_string(),
        }
    }

    fn monomorphize(&mut self, schema: usize, args: &[Ty]) -> Result<u32, CompileError> {
        let key = (schema, args.to_vec());
        if let Some(id) = self.mono.get(&key) {
            return Ok(*id);
        }
        let name = self.schema_name(schema);
        let params = self.schemas[schema].params.clone();
        let ctors = self.schemas[schema].ctors.clone();
        let id = self.adts.len() as u32;
        let display = applied_name(&name, args, self);
        self.adts.push(AdtDef {
            name: display,
            schema,
            args: args.to_vec(),
            ctors: Vec::new(),
        });
        self.mono.insert(key, id);
        let mut subst = HashMap::new();
        for (param, arg) in params.iter().zip(args) {
            subst.insert(param.clone(), *arg);
        }
        for ctor in ctors {
            let mut fields = Vec::new();
            for field in &ctor {
                fields.push(self.apply(field, &subst)?);
            }
            self.adts[id as usize].ctors.push(CtorDef { fields });
        }
        Ok(id)
    }

    fn schema_name(&self, schema: usize) -> String {
        self.schemas[schema].name.clone()
    }

    fn apply(&mut self, ty: &ParsedTy, subst: &HashMap<String, Ty>) -> Result<Ty, CompileError> {
        match ty {
            ParsedTy::Func { params, ret, .. } => {
                let mut fn_params = Vec::new();
                for param in params {
                    fn_params.push(self.apply(param, subst)?);
                }
                let fn_ret = self.apply(ret, subst)?;
                Ok(self.intern_func(fn_params, fn_ret))
            }
            ParsedTy::CFn { span, params, ret } => {
                let mut fn_params = Vec::new();
                for param in params {
                    let ty = self.apply(param, subst)?;
                    self.require_c_type(span.start, ty, false)?;
                    fn_params.push(ty);
                }
                let fn_ret = self.apply(ret, subst)?;
                self.require_c_type(span.start, fn_ret, true)?;
                Ok(self.intern_cfn(fn_params, fn_ret))
            }
            ParsedTy::Ref {
                span,
                mutable,
                inner,
            } => {
                let inner = self.apply(inner, subst)?;
                if inner.is_ref() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        "cannot borrow a reference",
                    ));
                }
                Ok(self.intern_ref(*mutable, inner))
            }
            ParsedTy::Ptr { mutable, inner, .. } => {
                let inner = self.apply(inner, subst)?;
                Ok(self.intern_ptr(*mutable, inner))
            }
            ParsedTy::Array { span, elem, len } => {
                let elem = self.apply(elem, subst)?;
                self.require_vec_elem(span.start, elem)?;
                let (_, value) = self.eval_const(len, Some(Ty::Usize))?;
                if !(0..=u16::MAX as i64).contains(&value) {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("array length {value} does not fit in usize"),
                    ));
                }
                Ok(self.intern_array(elem, value as u16))
            }
            ParsedTy::Tuple { span, fields } => {
                let mut typed = Vec::new();
                for field in fields {
                    typed.push(self.apply(field, subst)?);
                }
                self.intern_tuple(span.start, typed)
            }
            ParsedTy::Named { span, name, args } => {
                if let Some(found) = subst.get(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("type parameter `{name}` does not take arguments"),
                        ));
                    }
                    return Ok(*found);
                }
                if let Some(prim) = Ty::parse_name(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(prim);
                }
                if name == "Vec" || name == "Slice" {
                    if args.len() != 1 {
                        return Err(self.arity_error(span.start, name, 1, args.len()));
                    }
                    let elem = self.apply(&args[0], subst)?;
                    self.require_vec_elem(span.start, elem)?;
                    return Ok(if name == "Vec" {
                        self.intern_vec(elem)
                    } else {
                        self.intern_slice(elem)
                    });
                }
                if let Some(id) = self.struct_names.get(name).copied() {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    self.note_type(*span, name);
                    return Ok(Ty::Struct {
                        id,
                        width: self.struct_width(id),
                    });
                }
                let Some(schema) = self.schemas.iter().position(|schema| schema.name == *name)
                else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown type `{name}`"),
                    ));
                };
                self.note_type(*span, name);
                let expect = self.schemas[schema].params.len();
                if args.len() != expect {
                    return Err(self.arity_error(span.start, name, expect, args.len()));
                }
                let mut applied = Vec::new();
                for arg in args {
                    applied.push(self.apply(arg, subst)?);
                }
                let id = self.monomorphize(schema, &applied)?;
                Ok(Ty::Adt(id))
            }
        }
    }

    fn intern_ptr(&mut self, mutable: bool, inner: Ty) -> Ty {
        if let Some(index) = self
            .ptrs
            .iter()
            .position(|ptr| ptr.mutable == mutable && ptr.inner == inner)
        {
            return Ty::Ptr(index as u32);
        }
        let id = self.ptrs.len() as u32;
        self.ptrs.push(RefTy { mutable, inner });
        Ty::Ptr(id)
    }

    fn intern_ref(&mut self, mutable: bool, inner: Ty) -> Ty {
        if let Some(index) = self
            .refs
            .iter()
            .position(|refty| refty.mutable == mutable && refty.inner == inner)
        {
            return Ty::Ref(index as u32);
        }
        let id = self.refs.len() as u32;
        self.refs.push(RefTy { mutable, inner });
        Ty::Ref(id)
    }

    fn referent(&self, ty: Ty) -> Ty {
        if let Ty::Ref(id) = ty {
            self.refs[id as usize].inner
        } else {
            ty
        }
    }

    fn place_mutable(&self, binding: u32, ty: Ty) -> bool {
        if let Ty::Ref(id) = ty {
            self.refs[id as usize].mutable
        } else {
            self.bindings[binding as usize].mutable
        }
    }

    fn note_borrow(
        &mut self,
        span: usize,
        binding: u32,
        mutable: bool,
    ) -> Result<(), CompileError> {
        let conflict = self
            .borrows
            .iter()
            .any(|borrow| borrow.binding == binding && (mutable || borrow.mutable));
        if conflict {
            return Err(CompileError::at(self.source, span, "already borrowed"));
        }
        self.borrows.push(LiveBorrow {
            binding,
            mutable,
            depth: self.scopes.len(),
        });
        Ok(())
    }

    fn note_use(&self, span: usize, binding: u32, mutate: bool) -> Result<(), CompileError> {
        let conflict = self
            .borrows
            .iter()
            .any(|borrow| borrow.binding == binding && (mutate || borrow.mutable));
        if conflict {
            return Err(CompileError::at(self.source, span, "already borrowed"));
        }
        Ok(())
    }

    fn pop_scope(&mut self) {
        let depth = self.scopes.len();
        self.borrows.retain(|borrow| borrow.depth < depth);
        self.scopes.pop();
    }

    fn intern_array(&mut self, elem: Ty, len: u16) -> Ty {
        let width = elem.cells() as u16;
        if let Some(index) = self
            .arrays
            .iter()
            .position(|info| info.elem == elem && info.len == len)
        {
            return Ty::Array {
                id: index as u32,
                len,
                width,
            };
        }
        let id = self.arrays.len() as u32;
        self.arrays.push(ArrayInfo { elem, len });
        Ty::Array { id, len, width }
    }

    fn intern_slice(&mut self, elem: Ty) -> Ty {
        if let Some(index) = self.slice_elems.iter().position(|ty| *ty == elem) {
            return Ty::Slice(index as u32);
        }
        let id = self.slice_elems.len() as u32;
        self.slice_elems.push(elem);
        Ty::Slice(id)
    }

    fn intern_vec(&mut self, elem: Ty) -> Ty {
        if let Some(index) = self.vec_elems.iter().position(|ty| *ty == elem) {
            return Ty::Vec(index as u32);
        }
        let id = self.vec_elems.len() as u32;
        self.vec_elems.push(elem);
        Ty::Vec(id)
    }

    fn require_vec_elem(&self, span: usize, elem: Ty) -> Result<(), CompileError> {
        if elem.is_int() || elem.is_float() || elem == Ty::Bool {
            return Ok(());
        }
        Err(CompileError::at(
            self.source,
            span,
            format!(
                "vector elements must be numbers or bool, found {}",
                self.show(elem)
            ),
        ))
    }

    fn intern_cfn(&mut self, params: Vec<Ty>, ret: Ty) -> Ty {
        if let Some(index) = self
            .cfns
            .iter()
            .position(|sig| sig.params == params && sig.ret == ret)
        {
            return Ty::CFn(index as u32);
        }
        let id = self.cfns.len() as u32;
        self.cfns.push(FuncSig { params, ret });
        Ty::CFn(id)
    }

    fn intern_func(&mut self, params: Vec<Ty>, ret: Ty) -> Ty {
        if let Some(index) = self
            .funcs
            .iter()
            .position(|sig| sig.params == params && sig.ret == ret)
        {
            return Ty::Func(index as u32);
        }
        let id = self.funcs.len() as u32;
        self.funcs.push(FuncSig { params, ret });
        Ty::Func(id)
    }

    fn check_block(&mut self, block: &Block, expected: Option<Ty>) -> Result<TBlock, CompileError> {
        self.scopes.push(HashMap::new());
        self.scope_spans.push(block.span);
        let mut stmts = Vec::new();
        for stmt in &block.stmts {
            stmts.push(self.check_stmt(stmt)?);
        }
        let tail = match &block.tail {
            Some(expr) => Some(self.check_expr(expr, expected)?),
            None => None,
        };
        if let Some(tail_expr) = &tail {
            if tail_expr.ty.is_ref() {
                let span = block.tail.as_ref().map(|expr| expr.span.start).unwrap_or(0);
                return Err(CompileError::at(
                    self.source,
                    span,
                    "a reference cannot escape this block",
                ));
            }
        }
        self.pop_scope();
        self.scope_spans.pop();
        Ok(TBlock { stmts, tail })
    }

    fn check_stmt(&mut self, stmt: &Stmt) -> Result<TStmt, CompileError> {
        match stmt {
            Stmt::Let {
                span: let_span,
                pattern,
                mutable,
                ty,
                init,
            } => {
                self.next_visible = Some(let_span.end);
                let annotated = match ty {
                    Some(parsed) => Some(self.apply(parsed, &HashMap::new())?),
                    None => None,
                };
                let init = self.check_expr(init, annotated)?;
                let span = arm_span(pattern);
                if let Some(want) = annotated {
                    if init.ty != want {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!(
                                "the pattern has type {want}, the initializer has type {}",
                                init.ty
                            ),
                        ));
                    }
                }
                let pat = self.check_pat(pattern, init.ty, *mutable)?;
                if !self.irrefutable_pat(&pat, init.ty) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "a `let` pattern must be irrefutable",
                    ));
                }
                match pat {
                    TPat::Bind(binding) => Ok(TStmt::Let { binding, init }),
                    TPat::Wild => Ok(TStmt::Expr(init)),
                    other => Ok(TStmt::Destructure { pat: other, init }),
                }
            }
            Stmt::Assign {
                name,
                name_span,
                value,
            } => {
                let Some(id) = self.resolve(name) else {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("unknown name `{name}`"),
                    ));
                };
                let ty = self.bindings[id as usize].ty;
                if ty.is_ref() {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        "a reference cannot be reassigned",
                    ));
                }
                if !self.bindings[id as usize].mutable {
                    let captured = self.captures.iter().any(|cap| cap.local == id);
                    let message = if captured {
                        format!("`{name}` was captured by value and cannot be assigned")
                    } else {
                        format!("`{name}` is not mutable")
                    };
                    return Err(CompileError::at(self.source, name_span.start, message));
                }
                self.note_use(name_span.start, id, true)?;
                let value = self.check_expr(value, Some(ty))?;
                if value.ty != ty {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("cannot assign {} to `{name}` of type {ty}", value.ty),
                    ));
                }
                Ok(TStmt::Assign { binding: id, value })
            }
            Stmt::DerefAssign { ptr, value } => self.check_ptr_store(ptr, None, value),
            Stmt::IndexAssign {
                name,
                name_span,
                fields,
                index,
                value,
            } => {
                if !fields.is_empty() {
                    return self.check_field_index(name, name_span.start, fields, index, value);
                }
                let Some(id) = self.resolve(name) else {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("unknown name `{name}`"),
                    ));
                };
                let ty = self.bindings[id as usize].ty;
                if let Ty::Ptr(pid) = ty {
                    self.require_unsafe(name_span.start)?;
                    let info = &self.ptrs[pid as usize];
                    if !info.mutable {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            "cannot store through `*const`",
                        ));
                    }
                    let elem = info.inner;
                    self.require_pointee(name_span.start, elem)?;
                    let stride = elem.cells();
                    let index = self.check_expr(index, Some(Ty::Usize))?;
                    if index.ty != Ty::Usize {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            format!("expected usize, found {}", self.show(index.ty)),
                        ));
                    }
                    let value = self.check_expr(value, Some(elem))?;
                    if value.ty != elem {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            format!(
                                "expected {}, found {}",
                                self.show(elem),
                                self.show(value.ty)
                            ),
                        ));
                    }
                    let ptr = TExpr {
                        ty,
                        kind: TExprKind::Name(id),
                    };
                    return Ok(TStmt::PtrStore {
                        ptr,
                        index: Some(index),
                        value,
                        stride,
                    });
                }
                if matches!(ty, Ty::Slice(_)) || matches!(self.referent(ty), Ty::Slice(_)) {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        "slices are read-only",
                    ));
                }
                if !ty.is_ref() {
                    self.note_use(name_span.start, id, true)?;
                }
                if !self.place_mutable(id, ty) {
                    let message = if ty.is_ref() {
                        format!("`{name}` needs a mutable reference")
                    } else {
                        format!("`{name}` is not mutable")
                    };
                    return Err(CompileError::at(self.source, name_span.start, message));
                }
                let elem = if let Ty::Vec(vec_id) = self.referent(ty) {
                    self.vec_elems[vec_id as usize]
                } else if let Ty::Array { id: array_id, .. } = ty {
                    self.arrays[array_id as usize].elem
                } else {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("`{name}` is not a vector or array"),
                    ));
                };
                let index = self.check_expr(index, Some(Ty::Usize))?;
                if index.ty != Ty::Usize {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("expected usize, found {}", self.show(index.ty)),
                    ));
                }
                let value = self.check_expr(value, Some(elem))?;
                if value.ty != elem {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!(
                            "expected {}, found {}",
                            self.show(elem),
                            self.show(value.ty)
                        ),
                    ));
                }
                Ok(TStmt::IndexAssign {
                    binding: id,
                    offset: 0,
                    index,
                    value,
                    elem,
                    container: ty,
                })
            }
            Stmt::FieldAssign {
                name,
                name_span,
                fields,
                value,
            } => self.check_field_assign(name, name_span.start, fields, value),
            Stmt::Expr(expr) => Ok(TStmt::Expr(self.check_expr(expr, None)?)),
            Stmt::Return { span, value } => {
                let value = match value {
                    Some(expr) => Some(self.check_expr(expr, Some(self.fn_ret))?),
                    None => None,
                };
                let got = value.as_ref().map(|e| e.ty).unwrap_or(Ty::Unit);
                if got != self.fn_ret {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!(
                            "return has type {got}, the function returns {}",
                            self.fn_ret
                        ),
                    ));
                }
                Ok(TStmt::Return(value))
            }
            Stmt::Break { span } => {
                if self.loops == 0 {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        "`break` is outside a loop",
                    ));
                }
                Ok(TStmt::Break)
            }
            Stmt::Continue { span } => {
                if self.loops == 0 {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        "`continue` is outside a loop",
                    ));
                }
                Ok(TStmt::Continue)
            }
        }
    }

    fn check_expr(&mut self, expr: &Expr, expected: Option<Ty>) -> Result<TExpr, CompileError> {
        let saved = self.current_span;
        self.current_span = expr.span;
        let typed = self.check_expr_kind(expr, expected)?;
        let ty = self.show(typed.ty);
        self.expr_types.push(ExprType {
            span: expr.span,
            ty,
        });
        self.current_span = saved;
        Ok(typed)
    }

    fn note_type(&mut self, span: Span, name: &str) {
        let Some(def) = self.type_defs.get(name).copied() else {
            return;
        };
        if def.end <= def.start || span.end <= span.start {
            return;
        }
        self.uses.push(NameUse {
            span,
            def,
            def_file: None,
            ty: format!("type {name}"),
        });
    }

    fn note_sig(&mut self, label: String) {
        let span = self.current_span;
        if span.end <= span.start {
            return;
        }
        if let Some(found) = self.call_sigs.iter_mut().find(|sig| sig.span == span) {
            found.label = label;
        } else {
            self.call_sigs.push(CallSig { span, label });
        }
    }

    fn noted_fn(&mut self, params: &[Ty], ret: Ty, skip_first: bool) {
        let mut shown: Vec<String> = params.iter().map(|ty| self.show(*ty)).collect();
        if skip_first && !shown.is_empty() {
            shown.remove(0);
        }
        self.note_sig(format!("fn({}) -> {}", shown.join(", "), self.show(ret)));
    }

    fn method_def(&self, method: &str, receiver: &Expr) -> Option<(Span, String)> {
        let recv = self
            .expr_types
            .iter()
            .rev()
            .find(|expr| expr.span == receiver.span)?
            .ty
            .as_str();
        let facts: Vec<&BindingFact> = self
            .facts
            .iter()
            .filter(|fact| fact.name == method && fact.ty.starts_with("fn("))
            .collect();
        let matched = facts
            .iter()
            .copied()
            .find(|fact| signature_accepts_receiver(&fact.ty, recv));
        let fact = matched.or_else(|| {
            if facts.len() == 1 {
                Some(facts[0])
            } else {
                None
            }
        })?;
        Some((fact.def, fact.ty.clone()))
    }

    fn check_expr_kind(
        &mut self,
        expr: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let span = expr.span.start;
        match &expr.kind {
            ExprKind::Int(value) => {
                let ty = match expected {
                    Some(ty) if ty.is_int() => ty,
                    Some(ty) => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {ty}, found an integer literal"),
                        ));
                    }
                    None => Ty::I16,
                };
                if !ty.in_range(*value) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("{value} does not fit in {ty}"),
                    ));
                }
                Ok(TExpr {
                    ty,
                    kind: TExprKind::Int(*value),
                })
            }
            ExprKind::Float(text) => {
                let value = parse_float_literal(self.source, span, text)?;
                let ty = match expected {
                    Some(ty) if ty.is_float() => ty,
                    Some(ty) => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {ty}, found a float literal"),
                        ));
                    }
                    None => Ty::F64,
                };
                let bits = float_literal_bits(ty, value)
                    .map_err(|msg| CompileError::at(self.source, span, msg))?;
                Ok(TExpr {
                    ty,
                    kind: TExprKind::Float(bits),
                })
            }
            ExprKind::Bool(value) => {
                if let Some(ty) = expected {
                    if ty != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {ty}, found bool"),
                        ));
                    }
                }
                Ok(TExpr {
                    ty: Ty::Bool,
                    kind: TExprKind::Bool(*value),
                })
            }
            ExprKind::Name(name) => {
                if self.lookup(name).is_none() {
                    let parent = self
                        .outer_scopes
                        .iter()
                        .rev()
                        .find_map(|scope| scope.get(name).copied());
                    if let Some(parent) = parent {
                        if self.outer_bindings[parent as usize].ty.is_ref() {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                "cannot capture a reference",
                            ));
                        }
                    }
                }
                let Some(id) = self.resolve(name) else {
                    if let Some((ty, value)) = self.consts.get(name).copied() {
                        if let Some(want) = expected {
                            if want != ty {
                                return Err(CompileError::at(
                                    self.source,
                                    span,
                                    format!(
                                        "expected {}, `{name}` has type {}",
                                        self.show(want),
                                        self.show(ty)
                                    ),
                                ));
                            }
                        }
                        let kind = if ty == Ty::Bool {
                            TExprKind::Bool(value != 0)
                        } else if ty.is_float() {
                            TExprKind::Float(value as u64)
                        } else {
                            TExprKind::Int(value)
                        };
                        return Ok(TExpr { ty, kind });
                    }
                    if let Some(value) = self.fn_value(span, name, expected)? {
                        return Ok(value);
                    }
                    return self.nullary(span, name, expected);
                };
                let ty = self.bindings[id as usize].ty;
                let def = self.bindings[id as usize].def;
                if def.end > def.start {
                    let shown = self.show(ty);
                    self.uses.push(NameUse {
                        span: expr.span,
                        def,
                        def_file: None,
                        ty: shown,
                    });
                }
                self.note_use(span, id, false)?;
                if let Some(want) = expected {
                    if want != ty {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!(
                                "expected {}, `{name}` has type {}",
                                self.show(want),
                                self.show(ty)
                            ),
                        ));
                    }
                }
                Ok(TExpr {
                    ty,
                    kind: TExprKind::Name(id),
                })
            }
            ExprKind::Borrow { mutable, expr } => self.check_borrow(span, *mutable, expr, expected),
            ExprKind::Deref(inner) => self.check_deref(span, inner, expected),
            ExprKind::Assoc {
                ty_name,
                method,
                args,
            } => self.check_assoc(span, ty_name, method, args, expected),
            ExprKind::Try(expr) => self.check_try(span, expr, expected),
            ExprKind::Unary { op, expr: inner } => self.check_unary(span, *op, inner, expected),
            ExprKind::Binary { op, lhs, rhs } => self.check_binary(span, *op, lhs, rhs, expected),
            ExprKind::Char(scalar) => {
                if let Some(want) = expected {
                    if want != Ty::Char {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found char", self.show(want)),
                        ));
                    }
                }
                Ok(TExpr {
                    ty: Ty::Char,
                    kind: TExprKind::Char(*scalar),
                })
            }
            ExprKind::Str(chars) => {
                let bytes =
                    utf8_bytes(chars).map_err(|msg| CompileError::at(self.source, span, msg))?;
                if let Some(want) = expected {
                    if want != Ty::String {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found String", self.show(want)),
                        ));
                    }
                }
                Ok(TExpr {
                    ty: Ty::String,
                    kind: TExprKind::Str(bytes),
                })
            }
            ExprKind::CStr(chars) => {
                let ty = self.intern_ptr(false, Ty::I16);
                if let Some(want) = expected {
                    if want != ty {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found {}", self.show(want), self.show(ty)),
                        ));
                    }
                }
                Ok(TExpr {
                    ty,
                    kind: TExprKind::CStr(chars.clone()),
                })
            }
            ExprKind::Call {
                callee,
                callee_span,
                type_args,
                args,
            } => {
                let mark = self.borrows.len();
                let result = self.check_call(span, callee, type_args, args, expected)?;
                self.borrows.truncate(mark);
                let written = self.source.get(callee_span.start..callee_span.end);
                let found = self
                    .facts
                    .iter()
                    .rev()
                    .find(|fact| self.source.get(fact.def.start..fact.def.end) == written)
                    .map(|fact| (fact.def, fact.ty.clone()));
                if let Some((def, ty)) = found {
                    self.uses.push(NameUse {
                        span: *callee_span,
                        def,
                        def_file: None,
                        ty,
                    });
                }
                Ok(result)
            }
            ExprKind::Method {
                receiver,
                method,
                method_span,
                resolved,
                args,
            } => {
                let mark = self.borrows.len();
                let result = self.check_method(span, receiver, method, resolved, args, expected);
                self.borrows.truncate(mark);
                if let Some((def, ty)) = self.method_def(method, receiver) {
                    self.uses.push(NameUse {
                        span: *method_span,
                        def,
                        def_file: None,
                        ty,
                    });
                }
                result
            }
            ExprKind::Array(elems) => self.check_array(span, elems, expected),
            ExprKind::Index { base, index } => self.check_index(span, base, index, expected),
            ExprKind::Slice {
                base,
                start,
                end,
                inclusive,
            } => self.check_slice(span, base, start, end, *inclusive, expected),
            ExprKind::Lambda { params, body } => self.check_lambda(span, params, body, expected),
            ExprKind::Match { scrutinee, arms } => {
                self.check_match(span, scrutinee, arms, expected)
            }
            ExprKind::If {
                cond,
                then_body,
                else_body,
            } => self.check_if(cond, then_body, else_body, expected),
            ExprKind::While { cond, body } => self.check_while(span, cond, body, expected),
            ExprKind::For { pat, iter, body } => self.check_for(span, pat, iter, body, expected),
            ExprKind::Tuple(fields) => self.check_tuple(span, fields, expected),
            ExprKind::StructLit { name, fields } => {
                self.check_struct_lit(span, name, fields, expected)
            }
            ExprKind::Field { base, name, .. } => self.check_field(span, base, name, expected),
            ExprKind::Range { .. } => Err(CompileError::at(
                self.source,
                span,
                "a range can only be used in `for`",
            )),
            ExprKind::Unsafe(block) => {
                self.unsafe_depth += 1;
                let body = self.check_block(block, expected);
                self.unsafe_depth -= 1;
                let body = body?;
                let ty = body.tail.as_ref().map(|e| e.ty).unwrap_or(Ty::Unit);
                Ok(TExpr {
                    ty,
                    kind: TExprKind::Block(Box::new(body)),
                })
            }
            ExprKind::Block(block) => {
                let body = self.check_block(block, expected)?;
                let ty = body.tail.as_ref().map(|e| e.ty).unwrap_or(Ty::Unit);
                Ok(TExpr {
                    ty,
                    kind: TExprKind::Block(Box::new(body)),
                })
            }
            ExprKind::Cast { expr: inner, ty } => {
                let to = self.apply(ty, &HashMap::new())?;
                let inner = self.check_expr(inner, None)?;
                if !can_cast(inner.ty, to) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("cannot cast {} to {to}", inner.ty),
                    ));
                }
                if let Some(folded) = fold_cast(inner.ty, to, &inner.kind) {
                    let folded = folded.map_err(|msg| CompileError::at(self.source, span, msg))?;
                    return Ok(TExpr {
                        ty: to,
                        kind: folded,
                    });
                }
                Ok(TExpr {
                    ty: to,
                    kind: TExprKind::Cast {
                        expr: Box::new(inner),
                        to,
                    },
                })
            }
        }
    }

    fn check_unary(
        &mut self,
        span: usize,
        op: UnaryOp,
        inner: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        match op {
            UnaryOp::Not => {
                let inner = self.check_expr(inner, Some(Ty::Bool))?;
                if inner.ty != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`!` expects bool, found {}", inner.ty),
                    ));
                }
                Ok(TExpr {
                    ty: Ty::Bool,
                    kind: TExprKind::Unary {
                        op,
                        expr: Box::new(inner),
                    },
                })
            }
            UnaryOp::Neg => {
                let want = expected.filter(|ty| ty.is_int() || ty.is_float());
                let inner = self.check_expr(inner, want)?;
                if inner.ty.is_float() {
                    if let TExprKind::Float(bits) = inner.kind {
                        let sign = if inner.ty == Ty::F32 {
                            0x8000_0000
                        } else {
                            1u64 << 63
                        };
                        return Ok(TExpr {
                            ty: inner.ty,
                            kind: TExprKind::Float(bits ^ sign),
                        });
                    }
                    return Ok(TExpr {
                        ty: inner.ty,
                        kind: TExprKind::Unary {
                            op,
                            expr: Box::new(inner),
                        },
                    });
                }
                if !inner.ty.is_int() || !inner.ty.is_signed() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "`-` expects a signed integer or a float, found {}",
                            inner.ty
                        ),
                    ));
                }
                if let TExprKind::Int(value) = inner.kind {
                    let Some(negated) = value.checked_neg() else {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("-{value} does not fit in {}", inner.ty),
                        ));
                    };
                    if !inner.ty.in_range(negated) {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("{negated} does not fit in {}", inner.ty),
                        ));
                    }
                    return Ok(TExpr {
                        ty: inner.ty,
                        kind: TExprKind::Int(negated),
                    });
                }
                Ok(TExpr {
                    ty: inner.ty,
                    kind: TExprKind::Unary {
                        op,
                        expr: Box::new(inner),
                    },
                })
            }
            UnaryOp::BitNot => {
                let want = expected.filter(|ty| ty.is_int());
                let inner = self.check_expr(inner, want)?;
                if !inner.ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`~` expects an integer, found {}", self.show(inner.ty)),
                    ));
                }
                Ok(TExpr {
                    ty: inner.ty,
                    kind: TExprKind::Unary {
                        op,
                        expr: Box::new(inner),
                    },
                })
            }
        }
    }

    fn check_binary(
        &mut self,
        span: usize,
        op: BinOp,
        lhs: &Expr,
        rhs: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if op.is_logic() {
            let lhs = self.check_expr(lhs, Some(Ty::Bool))?;
            let rhs = self.check_expr(rhs, Some(Ty::Bool))?;
            if lhs.ty != Ty::Bool || rhs.ty != Ty::Bool {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "`&&` and `||` expect bool",
                ));
            }
            return Ok(TExpr {
                ty: Ty::Bool,
                kind: TExprKind::Binary {
                    op,
                    lhs: Box::new(lhs),
                    rhs: Box::new(rhs),
                },
            });
        }
        if op == BinOp::Add
            && (self.peek(lhs) == Some(Ty::String) || self.peek(rhs) == Some(Ty::String))
        {
            let lhs = self.check_expr(lhs, Some(Ty::String))?;
            let rhs = self.check_expr(rhs, Some(Ty::String))?;
            if lhs.ty != Ty::String || rhs.ty != Ty::String {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "cannot combine {} and {}",
                        self.show(lhs.ty),
                        self.show(rhs.ty)
                    ),
                ));
            }
            return Ok(TExpr {
                ty: Ty::String,
                kind: TExprKind::StrConcat {
                    lhs: Box::new(lhs),
                    rhs: Box::new(rhs),
                },
            });
        }
        if op.is_bitwise() {
            let operand_ty = expected
                .filter(|ty| ty.is_int())
                .or_else(|| self.peek(lhs))
                .or_else(|| self.peek(rhs))
                .unwrap_or_else(|| number_default(lhs, rhs));
            if !operand_ty.is_int() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "operator expects an integer, found {}",
                        self.show(operand_ty)
                    ),
                ));
            }
            let lhs = self.check_expr(lhs, Some(operand_ty))?;
            let rhs = self.check_expr(rhs, Some(operand_ty))?;
            if lhs.ty != operand_ty || rhs.ty != operand_ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "cannot combine {} and {}",
                        self.show(lhs.ty),
                        self.show(rhs.ty)
                    ),
                ));
            }
            return Ok(TExpr {
                ty: operand_ty,
                kind: TExprKind::Binary {
                    op,
                    lhs: Box::new(lhs),
                    rhs: Box::new(rhs),
                },
            });
        }
        if matches!(op, BinOp::Eq | BinOp::Ne) || op.is_compare() {
            let hint = self.peek(lhs).or_else(|| self.peek(rhs));
            if hint == Some(Ty::String) || hint == Some(Ty::Char) {
                if hint == Some(Ty::String) && !matches!(op, BinOp::Eq | BinOp::Ne) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "strings can only be compared with `==` and `!=`",
                    ));
                }
                if hint == Some(Ty::Char) && !op.is_compare() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "a char cannot be used in arithmetic",
                    ));
                }
                let lhs = self.check_expr(lhs, hint)?;
                let rhs = self.check_expr(rhs, Some(lhs.ty))?;
                if lhs.ty != rhs.ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("cannot combine {} and {}", lhs.ty, rhs.ty),
                    ));
                }
                return Ok(TExpr {
                    ty: Ty::Bool,
                    kind: TExprKind::Binary {
                        op,
                        lhs: Box::new(lhs),
                        rhs: Box::new(rhs),
                    },
                });
            }
        }
        let operand_ty = if op.is_compare() {
            self.peek(lhs)
                .or_else(|| self.peek(rhs))
                .unwrap_or_else(|| number_default(lhs, rhs))
        } else {
            expected
                .filter(|ty| ty.is_int() || ty.is_float())
                .or_else(|| self.peek(lhs))
                .or_else(|| self.peek(rhs))
                .unwrap_or_else(|| number_default(lhs, rhs))
        };
        if operand_ty.is_float() {
            if op == BinOp::Rem {
                return Err(CompileError::at(self.source, span, "`%` expects integers"));
            }
        } else if !operand_ty.is_int() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("operator expects a number, found {operand_ty}"),
            ));
        }
        let lhs = self.check_expr(lhs, Some(operand_ty))?;
        let rhs = self.check_expr(rhs, Some(operand_ty))?;
        if lhs.ty != operand_ty || rhs.ty != operand_ty {
            return Err(CompileError::at(
                self.source,
                span,
                format!("cannot combine {} and {}", lhs.ty, rhs.ty),
            ));
        }
        let ty = if op.is_compare() {
            Ty::Bool
        } else {
            operand_ty
        };
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {want}, this expression has type {ty}"),
                ));
            }
        }
        if operand_ty.is_float() {
            if let (TExprKind::Float(left), TExprKind::Float(right)) = (&lhs.kind, &rhs.kind) {
                let left = *left;
                let right = *right;
                if op.is_compare() {
                    return Ok(TExpr {
                        ty: Ty::Bool,
                        kind: TExprKind::Bool(float_compare(operand_ty, op, left, right)),
                    });
                }
                let bits = float_arith(operand_ty, op, left, right);
                return Ok(TExpr {
                    ty,
                    kind: TExprKind::Float(bits),
                });
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Binary {
                op,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            },
        })
    }

    fn check_call(
        &mut self,
        span: usize,
        callee: &str,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let skip_self = std::mem::replace(&mut self.sig_skip_self, false);
        if is_builtin(callee) {
            if !type_args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` does not take type arguments"),
                ));
            }
            return self.check_builtin(span, callee, args, expected);
        }
        if let Some(id) = self.resolve(callee) {
            return self.check_apply(span, callee, id, type_args, args, expected);
        }
        if self.ctor_schema.contains_key(callee) {
            return self.check_ctor(span, callee, type_args, args, expected);
        }
        if self.fn_schemas.contains_key(callee) {
            self.sig_skip_self = skip_self;
            return self.check_generic_call(span, callee, type_args, args, expected);
        }
        if !type_args.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` does not take type arguments"),
            ));
        }
        let Some(sig) = self.sigs.get(callee) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown function `{callee}`"),
            ));
        };
        let c_abi = sig.c_abi;
        let variadic = sig.variadic;
        let ret = if c_abi {
            sig.ret.unwrap_or(Ty::Unit)
        } else {
            let Some(ret) = sig.ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("function `{callee}` needs a return type before it can be called"),
                ));
            };
            ret
        };
        let params = sig.params.clone();
        let fixed = params.len();
        self.noted_fn(&params, ret, skip_self);
        if variadic {
            if args.len() < fixed {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`{callee}` expects at least {fixed} arguments, found {}",
                        args.len()
                    ),
                ));
            }
        } else if args.len() != fixed {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects {fixed} arguments, found {}", args.len()),
            ));
        }
        let loose_first = std::mem::replace(&mut self.loose_receiver, false);
        let mut checked = Vec::new();
        for (index, (arg, ty)) in args.iter().take(fixed).zip(params).enumerate() {
            let arg = if loose_first && index == 0 {
                let typed = self.check_expr(arg, None)?;
                if !self.ref_compatible(typed.ty, ty) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, found {}", self.show(ty), self.show(typed.ty)),
                    ));
                }
                typed
            } else {
                let mut typed = self.check_expr(arg, Some(ty))?;
                if c_abi {
                    typed = self.coerce_raw(span, typed, ty)?;
                }
                if typed.ty != ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, found {}", self.show(ty), self.show(typed.ty)),
                    ));
                }
                typed
            };
            checked.push(arg);
        }
        if variadic {
            for arg in args.iter().skip(fixed) {
                let typed = self.check_expr(arg, None)?;
                checked.push(self.promote_vararg(span, typed)?);
            }
        }
        let kind = if c_abi {
            TExprKind::Extern {
                callee: callee.to_string(),
                args: checked,
                fixed,
            }
        } else {
            TExprKind::Call {
                callee: callee.to_string(),
                args: checked,
            }
        };
        Ok(TExpr { ty: ret, kind })
    }

    fn check_c_apply(
        &mut self,
        span: usize,
        callee: &str,
        binding: u32,
        ty: Ty,
        id: u32,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if !type_args.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` does not take type arguments"),
            ));
        }
        let sig = self.cfns[id as usize].clone();
        if args.len() != sig.params.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{callee}` expects {} arguments, found {}",
                    sig.params.len(),
                    args.len()
                ),
            ));
        }
        let mut checked = Vec::new();
        for (arg, param) in args.iter().zip(&sig.params) {
            let typed = self.check_expr(arg, Some(*param))?;
            if typed.ty != *param {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, found {}",
                        self.show(*param),
                        self.show(typed.ty)
                    ),
                ));
            }
            checked.push(typed);
        }
        if let Some(want) = expected {
            if want != sig.ret {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{callee}` has type {}",
                        self.show(want),
                        self.show(sig.ret)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty: sig.ret,
            kind: TExprKind::CApply {
                callee: Box::new(TExpr {
                    ty,
                    kind: TExprKind::Name(binding),
                }),
                args: checked,
            },
        })
    }

    fn check_apply(
        &mut self,
        span: usize,
        callee: &str,
        binding: u32,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let ty = self.bindings[binding as usize].ty;
        if let Ty::CFn(id) = ty {
            return self.check_c_apply(span, callee, binding, ty, id, type_args, args, expected);
        }
        let Ty::Func(id) = ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` is not a function"),
            ));
        };
        if !type_args.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` does not take type arguments"),
            ));
        }
        let sig = self.funcs[id as usize].clone();
        self.noted_fn(&sig.params, sig.ret, false);
        if args.len() != sig.params.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{callee}` expects {} arguments, found {}",
                    sig.params.len(),
                    args.len()
                ),
            ));
        }
        let mut checked = Vec::new();
        for (arg, param) in args.iter().zip(&sig.params) {
            let typed = self.check_expr(arg, Some(*param))?;
            if typed.ty != *param {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, found {}",
                        self.show(*param),
                        self.show(typed.ty)
                    ),
                ));
            }
            checked.push(typed);
        }
        if let Some(want) = expected {
            if want != sig.ret {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{callee}` has type {}",
                        self.show(want),
                        self.show(sig.ret)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty: sig.ret,
            kind: TExprKind::Apply {
                callee: Box::new(TExpr {
                    ty,
                    kind: TExprKind::Name(binding),
                }),
                args: checked,
                params: sig.params,
            },
        })
    }

    fn check_generic_call(
        &mut self,
        span: usize,
        callee: &str,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let skip_self = std::mem::replace(&mut self.sig_skip_self, false);
        let type_params = self.fn_schemas[callee].type_params.clone();
        let func = self.ast_fns[self.fn_schemas[callee].index].clone();
        self.note_sig(parsed_signature(&func, skip_self));
        if args.len() != func.params.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{callee}` expects {} arguments, found {}",
                    func.params.len(),
                    args.len()
                ),
            ));
        }
        let loose_first = std::mem::replace(&mut self.loose_receiver, false);
        let mut solved = vec![None; type_params.len()];
        if !type_args.is_empty() {
            if type_args.len() != type_params.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`{callee}` expects {} type arguments, found {}",
                        type_params.len(),
                        type_args.len()
                    ),
                ));
            }
            for (index, ty) in type_args.iter().enumerate() {
                solved[index] = Some(self.apply(ty, &HashMap::new())?);
            }
        } else if let Some(want) = expected {
            if let Some(ret) = &func.ret {
                self.infer_field(ret, want, &type_params, &mut solved, span)?;
            }
        }
        let mut checked = Vec::new();
        for (index, (arg, param)) in args.iter().zip(&func.params).enumerate() {
            let param_ty = param.ty.as_ref().unwrap();
            if loose_first && index == 0 {
                let typed = self.check_expr(arg, None)?;
                if let Some(ty) = self.concrete_field(param_ty, &type_params, &solved)? {
                    if !self.ref_compatible(typed.ty, ty) {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found {}", self.show(ty), self.show(typed.ty)),
                        ));
                    }
                } else {
                    self.loose_receiver = true;
                    self.infer_field(param_ty, typed.ty, &type_params, &mut solved, span)?;
                    self.loose_receiver = false;
                }
                checked.push(typed);
                continue;
            }
            if let Some(ty) = self.concrete_field(param_ty, &type_params, &solved)? {
                let typed = self.check_expr(arg, Some(ty))?;
                if typed.ty != ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, found {}", self.show(ty), self.show(typed.ty)),
                    ));
                }
                checked.push(typed);
            } else {
                let typed = self.check_expr(arg, None)?;
                self.infer_field(param_ty, typed.ty, &type_params, &mut solved, span)?;
                checked.push(typed);
            }
        }
        if solved.iter().any(|slot| slot.is_none()) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("cannot infer type arguments for `{callee}`"),
            ));
        }
        let concrete: Vec<Ty> = solved.into_iter().map(Option::unwrap).collect();
        let mangled = self.instantiate(callee, &concrete)?;
        let concrete_params = self.sigs[&mangled].params.clone();
        let ret = self.sigs[&mangled].ret.unwrap();
        self.noted_fn(&concrete_params, ret, skip_self);
        if let Some(want) = expected {
            if want != ret {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{callee}` has type {}",
                        self.show(want),
                        self.show(ret)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty: ret,
            kind: TExprKind::Call {
                callee: mangled,
                args: checked,
            },
        })
    }

    /// `print`, `println`, and `putc` are temporary compiler builtins so programs
    /// can log values before `std/io` exists. They lower to direct `putchar` calls.
    fn check_builtin(
        &mut self,
        span: usize,
        callee: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if callee == "from_char" || callee == "from_byte" {
            let arg_ty = if callee == "from_char" { "char" } else { "u8" };
            self.note_sig(format!("fn({arg_ty}) -> String"));
            if args.len() != 1 {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` expects 1 argument, found {}", args.len()),
                ));
            }
            let want_arg = if callee == "from_char" {
                Ty::Char
            } else {
                Ty::U8
            };
            let arg = self.check_expr(&args[0], Some(want_arg))?;
            if arg.ty != want_arg {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` expects {want_arg}, found {}", self.show(arg.ty)),
                ));
            }
            if let Some(want) = expected {
                if want != Ty::String {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, `{callee}` has type String", self.show(want)),
                    ));
                }
            }
            let kind = if callee == "from_char" {
                TExprKind::StrFromChar(Box::new(arg))
            } else {
                TExprKind::StrFromByte(Box::new(arg))
            };
            return Ok(TExpr {
                ty: Ty::String,
                kind,
            });
        }
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {want}, `{callee}` has type unit"),
                ));
            }
        }
        if args.len() != 1 {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects 1 argument, found {}", args.len()),
            ));
        }
        let newline = callee == "println";
        self.note_sig("fn(integer | bool | char | float | String) -> ()".to_string());
        if callee == "putc" {
            self.note_sig("fn(integer | char) -> ()".to_string());
            let arg = self.check_expr(&args[0], None)?;
            if !arg.ty.is_int() && arg.ty != Ty::Char {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`putc` expects an integer or char, found {}",
                        self.show(arg.ty)
                    ),
                ));
            }
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::Putc(Box::new(arg)),
            });
        }
        if let ExprKind::Str(chars) = &args[0].kind {
            let bytes =
                utf8_bytes(chars).map_err(|msg| CompileError::at(self.source, span, msg))?;
            let cells = bytes.into_iter().map(u16::from).collect();
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::PrintLit {
                    chars: cells,
                    newline,
                },
            });
        }
        let arg = self.check_expr(&args[0], None)?;
        if !arg.ty.is_int()
            && !arg.ty.is_float()
            && arg.ty != Ty::Bool
            && arg.ty != Ty::Char
            && arg.ty != Ty::String
        {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{callee}` expects an integer, float, bool, char, or string, found {}",
                    self.show(arg.ty)
                ),
            ));
        }
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Print {
                value: Box::new(arg),
                newline,
            },
        })
    }

    fn check_if(
        &mut self,
        cond: &Expr,
        then_body: &Block,
        else_body: &Option<Box<Expr>>,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let cond = self.check_expr(cond, Some(Ty::Bool))?;
        let Some(else_body) = else_body else {
            let then_body = self.check_block(then_body, None)?;
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::If {
                    cond: Box::new(cond),
                    then_body: Box::new(then_body),
                    else_body: None,
                },
            });
        };
        let then_body = self.check_block(then_body, expected)?;
        let then_ty = then_body.tail.as_ref().map(|e| e.ty).unwrap_or(Ty::Unit);
        let else_expr = self.check_expr(else_body, Some(then_ty))?;
        if else_expr.ty != then_ty {
            return Err(CompileError::at(
                self.source,
                else_body.span.start,
                format!("if branches have types {then_ty} and {}", else_expr.ty),
            ));
        }
        Ok(TExpr {
            ty: then_ty,
            kind: TExprKind::If {
                cond: Box::new(cond),
                then_body: Box::new(then_body),
                else_body: Some(Box::new(else_expr)),
            },
        })
    }

    fn check_while(
        &mut self,
        span: usize,
        cond: &Expr,
        body: &Block,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {want}, `while` has type unit"),
                ));
            }
        }
        let cond = self.check_expr(cond, Some(Ty::Bool))?;
        if cond.ty != Ty::Bool {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`while` expects bool, found {}", cond.ty),
            ));
        }
        self.loops += 1;
        let body = self.check_block(body, None)?;
        self.loops -= 1;
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::While {
                cond: Box::new(cond),
                body: Box::new(body),
            },
        })
    }

    /// A monomorphic function name is a callable value: code id plus an empty environment.
    /// Every function frame reserves the environment slot, so the same call protocol reaches
    /// named functions and lambdas.
    fn fn_value(
        &mut self,
        span: usize,
        name: &str,
        expected: Option<Ty>,
    ) -> Result<Option<TExpr>, CompileError> {
        if self.fn_schemas.contains_key(name) {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "generic function `{name}` needs a concrete signature to be used as a value"
                ),
            ));
        }
        let Some(sig) = self.sigs.get(name) else {
            return Ok(None);
        };
        if sig.c_abi {
            if sig.variadic {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("variadic function `{name}` cannot be used as a value"),
                ));
            }
            let Some(ret) = sig.ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("function `{name}` needs a return type to be used as a value"),
                ));
            };
            let params = sig.params.clone();
            let ty = self.intern_cfn(params, ret);
            if let Some(want) = expected {
                if want != ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, `{name}` has type {}",
                            self.show(want),
                            self.show(ty)
                        ),
                    ));
                }
            }
            return Ok(Some(TExpr {
                ty,
                kind: TExprKind::CFn {
                    symbol: name.to_string(),
                },
            }));
        }
        let Some(ret) = sig.ret else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("function `{name}` needs a return type to be used as a value"),
            ));
        };
        let params = sig.params.clone();
        let ty = self.intern_func(params, ret);
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{name}` has type {}",
                        self.show(want),
                        self.show(ty)
                    ),
                ));
            }
        }
        Ok(Some(TExpr {
            ty,
            kind: TExprKind::Closure {
                func: name.to_string(),
            },
        }))
    }

    fn check_array(
        &mut self,
        span: usize,
        elems: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let (expected_elem, array_len) = match expected {
            Some(Ty::Vec(id)) => (Some(self.vec_elems[id as usize]), None),
            Some(Ty::Array { id, len, .. }) => (Some(self.arrays[id as usize].elem), Some(len)),
            Some(want) => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found an array", self.show(want)),
                ));
            }
            None => (None, None),
        };
        if let Some(len) = array_len {
            if elems.len() != len as usize {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {len} elements, found {}", elems.len()),
                ));
            }
        }
        if elems.is_empty() {
            let Some(elem) = expected_elem else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "cannot infer the element type of an empty array",
                ));
            };
            if array_len == Some(0) {
                let ty = self.intern_array(elem, 0);
                return Ok(TExpr {
                    ty,
                    kind: TExprKind::ArrayLit {
                        elems: Vec::new(),
                        elem,
                    },
                });
            }
            return Ok(TExpr {
                ty: self.intern_vec(elem),
                kind: TExprKind::VecLit {
                    elems: Vec::new(),
                    elem,
                },
            });
        }
        let mut checked = Vec::new();
        let first = self.check_expr(&elems[0], expected_elem)?;
        let elem = first.ty;
        self.require_vec_elem(span, elem)?;
        if let Some(want) = expected_elem {
            if want != elem {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(elem)),
                ));
            }
        }
        checked.push(first);
        for expr in &elems[1..] {
            let item = self.check_expr(expr, Some(elem))?;
            if item.ty != elem {
                return Err(CompileError::at(
                    self.source,
                    expr.span.start,
                    format!("expected {}, found {}", self.show(elem), self.show(item.ty)),
                ));
            }
            checked.push(item);
        }
        if let Some(len) = array_len {
            let ty = self.intern_array(elem, len);
            return Ok(TExpr {
                ty,
                kind: TExprKind::ArrayLit {
                    elems: checked,
                    elem,
                },
            });
        }
        Ok(TExpr {
            ty: self.intern_vec(elem),
            kind: TExprKind::VecLit {
                elems: checked,
                elem,
            },
        })
    }

    fn check_try(
        &mut self,
        span: usize,
        expr: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let operand = self.check_expr(expr, None)?;
        let Ty::Adt(id) = operand.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`?` expects Option or Result, found {}",
                    self.show(operand.ty)
                ),
            ));
        };
        let schema = self.adts[id as usize].schema;
        let name = self.schemas[schema].name.clone();
        let args = self.adts[id as usize].args.clone();
        let (payload, fail, ok_tag, fail_tag) = if name == "Option" {
            let Ty::Adt(ret_id) = self.fn_ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` needs the function to return Option, it returns {}",
                        self.show(self.fn_ret)
                    ),
                ));
            };
            if self.schemas[self.adts[ret_id as usize].schema].name != "Option" {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` needs the function to return Option, it returns {}",
                        self.show(self.fn_ret)
                    ),
                ));
            }
            (
                args[0],
                None,
                self.ctor_schema["Some"].1,
                self.ctor_schema["None"].1,
            )
        } else if name == "Result" {
            let Ty::Adt(ret_id) = self.fn_ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` needs the function to return Result, it returns {}",
                        self.show(self.fn_ret)
                    ),
                ));
            };
            if self.schemas[self.adts[ret_id as usize].schema].name != "Result" {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` needs the function to return Result, it returns {}",
                        self.show(self.fn_ret)
                    ),
                ));
            }
            let ret_err = self.adts[ret_id as usize].args[1];
            if ret_err != args[1] {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`?` error type is {}, the function returns {}",
                        self.show(args[1]),
                        self.show(ret_err)
                    ),
                ));
            }
            (
                args[0],
                Some(args[1]),
                self.ctor_schema["Ok"].1,
                self.ctor_schema["Err"].1,
            )
        } else {
            return Err(CompileError::at(
                self.source,
                span,
                "`?` expects Option or Result",
            ));
        };
        if let Some(want) = expected {
            if want != payload {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `?` has type {}",
                        self.show(want),
                        self.show(payload)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty: payload,
            kind: TExprKind::Try {
                expr: Box::new(operand),
                ok_tag,
                fail_tag,
                payload,
                fail,
            },
        })
    }

    fn check_borrow(
        &mut self,
        span: usize,
        mutable: bool,
        expr: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let ExprKind::Name(name) = &expr.kind else {
            return Err(CompileError::at(
                self.source,
                span,
                "only a name can be borrowed",
            ));
        };
        let Some(id) = self.resolve(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown name `{name}`"),
            ));
        };
        let ty = self.bindings[id as usize].ty;
        if ty.is_ref() {
            return Err(CompileError::at(
                self.source,
                span,
                "cannot borrow a reference",
            ));
        }
        if mutable && !self.bindings[id as usize].mutable {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not mutable"),
            ));
        }
        let refty = self.intern_ref(mutable, ty);
        if let Some(want) = expected {
            if self.ptr_from_ref(want, refty) {
                self.require_unsafe(span)?;
                self.note_borrow(span, id, mutable)?;
                return Ok(TExpr {
                    ty: want,
                    kind: TExprKind::Borrow { binding: id },
                });
            }
            if want != refty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(refty)),
                ));
            }
        }
        self.note_borrow(span, id, mutable)?;
        Ok(TExpr {
            ty: refty,
            kind: TExprKind::Borrow { binding: id },
        })
    }

    fn require_unsafe(&self, span: usize) -> Result<(), CompileError> {
        if self.unsafe_depth == 0 {
            Err(CompileError::at(
                self.source,
                span,
                "a raw pointer operation needs `unsafe`",
            ))
        } else {
            Ok(())
        }
    }

    fn plain_pointee(&self, ty: Ty) -> bool {
        if ty.is_int()
            || ty.is_float()
            || ty == Ty::Bool
            || ty == Ty::Char
            || matches!(ty, Ty::Ptr(_))
        {
            return true;
        }
        if let Ty::Struct { id, .. } = ty {
            let info = &self.structs[id as usize];
            return info.repr_c
                && info
                    .fields
                    .iter()
                    .all(|(_, field)| self.plain_pointee(*field));
        }
        if let Ty::Array { id, .. } = ty {
            return self.plain_pointee(self.arrays[id as usize].elem);
        }
        false
    }

    fn require_pointee(&self, span: usize, ty: Ty) -> Result<(), CompileError> {
        if self.plain_pointee(ty) {
            Ok(())
        } else {
            Err(CompileError::at(
                self.source,
                span,
                format!(
                    "cannot load or store `{}` through a raw pointer",
                    self.show(ty)
                ),
            ))
        }
    }

    fn check_deref(
        &mut self,
        span: usize,
        inner: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let ptr = self.check_expr(inner, None)?;
        self.check_ptr_load(span, ptr, None, expected)
    }

    fn check_ptr_load(
        &mut self,
        span: usize,
        ptr: TExpr,
        index: Option<&Expr>,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        self.require_unsafe(span)?;
        let Ty::Ptr(id) = ptr.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected a raw pointer, found {}", self.show(ptr.ty)),
            ));
        };
        let elem = self.ptrs[id as usize].inner;
        self.require_pointee(span, elem)?;
        if let Some(want) = expected {
            if want != elem {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(elem)),
                ));
            }
        }
        let index = match index {
            Some(index) => {
                let index = self.check_expr(index, Some(Ty::Usize))?;
                if index.ty != Ty::Usize {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected usize, found {}", self.show(index.ty)),
                    ));
                }
                Some(Box::new(index))
            }
            None => None,
        };
        Ok(TExpr {
            ty: elem,
            kind: TExprKind::Deref {
                ptr: Box::new(ptr),
                index,
                stride: elem.cells(),
            },
        })
    }

    fn check_ptr_store(
        &mut self,
        ptr: &Expr,
        index: Option<&Expr>,
        value: &Expr,
    ) -> Result<TStmt, CompileError> {
        let span = ptr.span.start;
        let ptr = self.check_expr(ptr, None)?;
        self.store_through_ptr(span, ptr, index, value)
    }

    fn store_through_ptr(
        &mut self,
        span: usize,
        ptr: TExpr,
        index: Option<&Expr>,
        value: &Expr,
    ) -> Result<TStmt, CompileError> {
        self.require_unsafe(span)?;
        let Ty::Ptr(id) = ptr.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected a raw pointer, found {}", self.show(ptr.ty)),
            ));
        };
        let info = &self.ptrs[id as usize];
        if !info.mutable {
            return Err(CompileError::at(
                self.source,
                span,
                "cannot store through `*const`",
            ));
        }
        let elem = info.inner;
        self.require_pointee(span, elem)?;
        let stride = elem.cells();
        let index = match index {
            Some(index) => {
                let index = self.check_expr(index, Some(Ty::Usize))?;
                if index.ty != Ty::Usize {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected usize, found {}", self.show(index.ty)),
                    ));
                }
                Some(index)
            }
            None => None,
        };
        let value = self.check_expr(value, Some(elem))?;
        if value.ty != elem {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected {}, found {}",
                    self.show(elem),
                    self.show(value.ty)
                ),
            ));
        }
        Ok(TStmt::PtrStore {
            ptr,
            index,
            value,
            stride,
        })
    }

    fn check_ptr_method(
        &mut self,
        span: usize,
        recv: TExpr,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let Ty::Ptr(id) = recv.ty else {
            return Err(CompileError::at(
                self.source,
                span,
                "expected a raw pointer",
            ));
        };
        let inner = self.ptrs[id as usize].inner;
        match method {
            "addr" | "bank" | "is_null" => {
                if !args.is_empty() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`{method}` expects 0 arguments, found {}", args.len()),
                    ));
                }
                let ty = if method == "is_null" {
                    Ty::Bool
                } else {
                    Ty::U16
                };
                if let Some(want) = expected {
                    if want != ty {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!(
                                "expected {}, `{method}` has type {}",
                                self.show(want),
                                self.show(ty)
                            ),
                        ));
                    }
                }
                let kind = match method {
                    "addr" => TExprKind::PtrAddr {
                        ptr: Box::new(recv),
                    },
                    "bank" => TExprKind::PtrBank {
                        ptr: Box::new(recv),
                    },
                    _ => TExprKind::PtrNull {
                        ptr: Box::new(recv),
                    },
                };
                Ok(TExpr { ty, kind })
            }
            "offset" => {
                self.require_unsafe(span)?;
                if args.len() != 1 {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`offset` expects 1 argument, found {}", args.len()),
                    ));
                }
                let index = self.check_expr(&args[0], Some(Ty::I16))?;
                if index.ty != Ty::I16 {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected i16, found {}", self.show(index.ty)),
                    ));
                }
                if let Some(want) = expected {
                    if want != recv.ty {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found {}", self.show(want), self.show(recv.ty)),
                        ));
                    }
                }
                Ok(TExpr {
                    ty: recv.ty,
                    kind: TExprKind::PtrOffset {
                        ptr: Box::new(recv),
                        index: Box::new(index),
                        stride: inner.cells(),
                    },
                })
            }
            _ => Err(CompileError::at(
                self.source,
                span,
                format!("a raw pointer has no method `{method}`"),
            )),
        }
    }

    fn check_index(
        &mut self,
        span: usize,
        base: &Expr,
        index: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let base = self.check_expr(base, None)?;
        if let Ty::Ptr(_) = base.ty {
            return self.check_ptr_load(span, base, Some(index), expected);
        }
        let Some(elem) = self.element_of(base.ty) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected a vector, slice, or array, found {}",
                    self.show(base.ty)
                ),
            ));
        };
        if let Some(want) = expected {
            if want != elem {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(elem)),
                ));
            }
        }
        let index = self.check_expr(index, Some(Ty::Usize))?;
        if index.ty != Ty::Usize {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected usize, found {}", self.show(index.ty)),
            ));
        }
        Ok(TExpr {
            ty: elem,
            kind: TExprKind::Index {
                base: Box::new(base),
                index: Box::new(index),
                elem,
            },
        })
    }

    fn check_slice(
        &mut self,
        span: usize,
        base: &Expr,
        start: &Expr,
        end: &Expr,
        inclusive: bool,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let base = self.check_expr(base, None)?;
        if base.ty == Ty::String {
            let start = self.check_expr(start, Some(Ty::Usize))?;
            let end = self.check_expr(end, Some(Ty::Usize))?;
            if start.ty != Ty::Usize || end.ty != Ty::Usize {
                return Err(CompileError::at(
                    self.source,
                    span,
                    "a slice bound must be usize",
                ));
            }
            if let Some(want) = expected {
                if want != Ty::String {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, found String", self.show(want)),
                    ));
                }
            }
            return Ok(TExpr {
                ty: Ty::String,
                kind: TExprKind::StrSlice {
                    base: Box::new(base),
                    start: Box::new(start),
                    end: Box::new(end),
                    inclusive,
                },
            });
        }
        let Some(elem) = self.element_of(base.ty) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected a vector, slice, or array, found {}",
                    self.show(base.ty)
                ),
            ));
        };
        let start = self.check_expr(start, Some(Ty::Usize))?;
        let end = self.check_expr(end, Some(Ty::Usize))?;
        if start.ty != Ty::Usize || end.ty != Ty::Usize {
            return Err(CompileError::at(
                self.source,
                span,
                "a slice bound must be usize",
            ));
        }
        let ty = self.intern_slice(elem);
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(ty)),
                ));
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Slice {
                base: Box::new(base),
                start: Box::new(start),
                end: Box::new(end),
                inclusive,
                elem,
            },
        })
    }

    /// Element type of a vector, slice, array, or a reference to a vector.
    fn element_of(&self, ty: Ty) -> Option<Ty> {
        match ty {
            Ty::Vec(id) => Some(self.vec_elems[id as usize]),
            Ty::Slice(id) => Some(self.slice_elems[id as usize]),
            Ty::Array { id, .. } => Some(self.arrays[id as usize].elem),
            Ty::String => Some(Ty::Char),
            Ty::Ref(id) => match self.refs[id as usize].inner {
                Ty::Vec(elem) => Some(self.vec_elems[elem as usize]),
                _ => None,
            },
            _ => None,
        }
    }

    fn check_method(
        &mut self,
        span: usize,
        receiver: &Expr,
        method: &str,
        resolved: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let recv = self.check_expr(receiver, None)?;
        if let Ty::Ptr(_) = recv.ty {
            return self.check_ptr_method(span, recv, method, args, expected);
        }
        if recv.ty == Ty::String && matches!(method, "len" | "is_empty") {
            return self.check_vec_method(span, recv, method, args, Ty::U8, expected);
        }
        let place = self.referent(recv.ty);
        if let Ty::Vec(id) = place {
            let elem = self.vec_elems[id as usize];
            if matches!(
                method,
                "len" | "push" | "is_empty" | "clear" | "pop" | "get"
            ) {
                return self.check_vec_method(span, recv, method, args, elem, expected);
            }
        }
        if let Ty::Slice(id) = recv.ty {
            let elem = self.slice_elems[id as usize];
            if matches!(method, "len" | "is_empty" | "get" | "to_vec") {
                return self.check_view_method(span, recv, method, args, elem, expected);
            }
            if matches!(method, "push" | "pop" | "clear") {
                return Err(CompileError::at(self.source, span, "slices are read-only"));
            }
        }
        if let Ty::Array { id, .. } = recv.ty {
            let elem = self.arrays[id as usize].elem;
            if matches!(method, "len" | "is_empty" | "get" | "to_vec") {
                return self.check_view_method(span, recv, method, args, elem, expected);
            }
        }
        if let Some(key) = self.type_key(recv.ty) {
            if let Some(info) = self
                .methods
                .iter()
                .find(|item| item.type_name == key && item.method == method)
                .cloned()
            {
                if info.receiver.is_none() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`{method}` is an associated function of `{key}`"),
                    ));
                }
                return self.check_inherent(span, receiver, recv.ty, &info, args, expected);
            }
        }
        let mut all = vec![receiver.clone()];
        all.extend(args.iter().cloned());
        self.check_call(span, resolved, &[], &all, expected)
    }

    fn check_inherent(
        &mut self,
        span: usize,
        receiver: &Expr,
        recv_ty: Ty,
        info: &Inherent,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let adapted = self.adapt_receiver(span, receiver, recv_ty, info.receiver)?;
        let mut all = vec![adapted];
        all.extend(args.iter().cloned());
        let arity = self
            .ast_fns
            .iter()
            .find(|func| func.name == info.func)
            .map(|func| func.params.len())
            .unwrap_or(0);
        let expect = arity - usize::from(info.receiver.is_some());
        if let Some(preview) = self
            .ast_fns
            .iter()
            .find(|func| func.name == info.func)
            .map(|func| parsed_signature(func, info.receiver.is_some()))
        {
            self.note_sig(preview);
        }
        if args.len() != expect {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{}` expects {expect} arguments, found {}",
                    info.method,
                    args.len()
                ),
            ));
        }
        self.loose_receiver = matches!(info.receiver, Some(Recv::Shared));
        self.sig_skip_self = true;
        let result = if self.fn_schemas.contains_key(&info.func) {
            self.check_generic_call(span, &info.func, &[], &all, expected)
        } else {
            self.check_call(span, &info.func, &[], &all, expected)
        };
        self.loose_receiver = false;
        self.sig_skip_self = false;
        result
    }

    fn adapt_receiver(
        &self,
        span: usize,
        expr: &Expr,
        ty: Ty,
        recv: Option<Recv>,
    ) -> Result<Expr, CompileError> {
        let Some(recv) = recv else {
            return Ok(expr.clone());
        };
        match recv {
            Recv::Value => {
                if ty.is_ref() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "this method takes `self` by value",
                    ));
                }
                Ok(expr.clone())
            }
            Recv::Shared => Ok(if ty.is_ref() {
                expr.clone()
            } else {
                Expr {
                    span: expr.span,
                    kind: ExprKind::Borrow {
                        mutable: false,
                        expr: Box::new(expr.clone()),
                    },
                }
            }),
            Recv::Mut => {
                if let Ty::Ref(id) = ty {
                    if !self.refs[id as usize].mutable {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            "this method needs a mutable reference",
                        ));
                    }
                    return Ok(expr.clone());
                }
                Ok(Expr {
                    span: expr.span,
                    kind: ExprKind::Borrow {
                        mutable: true,
                        expr: Box::new(expr.clone()),
                    },
                })
            }
        }
    }

    fn type_key(&self, ty: Ty) -> Option<String> {
        let ty = self.referent(ty);
        Some(match ty {
            Ty::Adt(id) => self.schemas[self.adts[id as usize].schema as usize]
                .name
                .clone(),
            Ty::Vec(_) => "Vec".to_string(),
            Ty::Slice(_) => "Slice".to_string(),
            Ty::I8 => "i8".to_string(),
            Ty::U8 => "u8".to_string(),
            Ty::I16 => "i16".to_string(),
            Ty::U16 => "u16".to_string(),
            Ty::I32 => "i32".to_string(),
            Ty::U32 => "u32".to_string(),
            Ty::I64 => "i64".to_string(),
            Ty::U64 => "u64".to_string(),
            Ty::F32 => "f32".to_string(),
            Ty::F64 => "f64".to_string(),
            Ty::Usize => "usize".to_string(),
            Ty::Bool => "bool".to_string(),
            Ty::Char => "char".to_string(),
            Ty::String => "String".to_string(),
            Ty::Struct { id, .. } => self.structs[id as usize].name.clone(),
            Ty::Unit
            | Ty::Func(_)
            | Ty::CFn(_)
            | Ty::Ref(_)
            | Ty::Ptr(_)
            | Ty::Array { .. }
            | Ty::Tuple { .. } => return None,
        })
    }

    fn ref_compatible(&self, actual: Ty, want: Ty) -> bool {
        if actual == want {
            return true;
        }
        let (Ty::Ref(actual_id), Ty::Ref(want_id)) = (actual, want) else {
            return false;
        };
        let actual = &self.refs[actual_id as usize];
        let want = &self.refs[want_id as usize];
        actual.inner == want.inner && actual.mutable && !want.mutable
    }

    fn check_assoc(
        &mut self,
        span: usize,
        ty_name: &str,
        method: &str,
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let Some(info) = self
            .methods
            .iter()
            .find(|item| item.type_name == ty_name && item.method == method)
            .cloned()
        else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{ty_name}` has no function `{method}`"),
            ));
        };
        if self.fn_schemas.contains_key(&info.func) {
            self.check_generic_call(span, &info.func, &[], args, expected)
        } else {
            self.check_call(span, &info.func, &[], args, expected)
        }
    }

    fn check_view_method(
        &mut self,
        span: usize,
        recv: TExpr,
        method: &str,
        args: &[Expr],
        elem: Ty,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if method == "to_vec" {
            self.note_sig(format!("fn() -> Vec<{}>", self.show(elem)));
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`to_vec` expects 0 arguments, found {}", args.len()),
                ));
            }
            let ty = self.intern_vec(elem);
            if let Some(want) = expected {
                if want != ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, `to_vec` has type {}",
                            self.show(want),
                            self.show(ty)
                        ),
                    ));
                }
            }
            return Ok(TExpr {
                ty,
                kind: TExprKind::ToVec {
                    src: Box::new(recv),
                    elem,
                },
            });
        }
        self.check_vec_method(span, recv, method, args, elem, expected)
    }

    fn check_vec_method(
        &mut self,
        span: usize,
        recv: TExpr,
        method: &str,
        args: &[Expr],
        elem: Ty,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if method == "len" {
            self.note_sig("fn() -> usize".to_string());
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`len` expects 0 arguments, found {}", args.len()),
                ));
            }
            if let Some(want) = expected {
                if want != Ty::Usize {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, `len` has type usize", self.show(want)),
                    ));
                }
            }
            return Ok(TExpr {
                ty: Ty::Usize,
                kind: TExprKind::Len {
                    vec: Box::new(recv),
                },
            });
        }
        if method == "is_empty" {
            self.note_sig("fn() -> bool".to_string());
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`is_empty` expects 0 arguments, found {}", args.len()),
                ));
            }
            if let Some(want) = expected {
                if want != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, `is_empty` has type bool", self.show(want)),
                    ));
                }
            }
            return Ok(TExpr {
                ty: Ty::Bool,
                kind: TExprKind::Empty {
                    vec: Box::new(recv),
                },
            });
        }
        if method == "clear" {
            self.note_sig("fn() -> ()".to_string());
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`clear` expects 0 arguments, found {}", args.len()),
                ));
            }
            let binding = self.vec_place(span, &recv, "clear", true)?;
            if let Some(want) = expected {
                if want != Ty::Unit {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, `clear` has type unit", self.show(want)),
                    ));
                }
            }
            return Ok(TExpr {
                ty: Ty::Unit,
                kind: TExprKind::Clear { binding },
            });
        }
        if method == "pop" {
            self.note_sig(format!("fn() -> Option<{}>", self.show(elem)));
            if !args.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`pop` expects 0 arguments, found {}", args.len()),
                ));
            }
            let binding = self.vec_place(span, &recv, "pop", true)?;
            let (option, some_tag, none_tag) = self.option_of(elem)?;
            if let Some(want) = expected {
                if want != option {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, `pop` has type {}",
                            self.show(want),
                            self.show(option)
                        ),
                    ));
                }
            }
            return Ok(TExpr {
                ty: option,
                kind: TExprKind::Pop {
                    binding,
                    elem,
                    some_tag,
                    none_tag,
                },
            });
        }
        if method == "get" {
            self.note_sig(format!("fn(usize) -> Option<{}>", self.show(elem)));
            if args.len() != 1 {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`get` expects 1 argument, found {}", args.len()),
                ));
            }
            let index = self.check_expr(&args[0], Some(Ty::Usize))?;
            if index.ty != Ty::Usize {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected usize, found {}", self.show(index.ty)),
                ));
            }
            let (option, some_tag, none_tag) = self.option_of(elem)?;
            if let Some(want) = expected {
                if want != option {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, `get` has type {}",
                            self.show(want),
                            self.show(option)
                        ),
                    ));
                }
            }
            return Ok(TExpr {
                ty: option,
                kind: TExprKind::Get {
                    vec: Box::new(recv),
                    index: Box::new(index),
                    elem,
                    some_tag,
                    none_tag,
                },
            });
        }
        self.note_sig(format!("fn({}) -> ()", self.show(elem)));
        if args.len() != 1 {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`push` expects 1 argument, found {}", args.len()),
            ));
        }
        let binding = self.vec_place(span, &recv, "push", true)?;
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, `push` has type unit", self.show(want)),
                ));
            }
        }
        let value = self.check_expr(&args[0], Some(elem))?;
        if value.ty != elem {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected {}, found {}",
                    self.show(elem),
                    self.show(value.ty)
                ),
            ));
        }
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Push {
                binding,
                value: Box::new(value),
                elem,
            },
        })
    }

    fn option_of(&mut self, elem: Ty) -> Result<(Ty, u16, u16), CompileError> {
        let schema = self
            .schemas
            .iter()
            .position(|schema| schema.name == "Option")
            .expect("prelude Option");
        let id = self.monomorphize(schema, &[elem])?;
        let none = self.ctor_schema["None"].1;
        let some = self.ctor_schema["Some"].1;
        Ok((Ty::Adt(id), some, none))
    }

    fn vec_place(
        &mut self,
        span: usize,
        recv: &TExpr,
        method: &str,
        mutate: bool,
    ) -> Result<u32, CompileError> {
        let TExprKind::Name(binding) = recv.kind else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{method}` needs a name"),
            ));
        };
        let ty = self.bindings[binding as usize].ty;
        if !matches!(self.referent(ty), Ty::Vec(_)) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{method}` needs a vector"),
            ));
        }
        if mutate && !self.place_mutable(binding, ty) {
            let message = if ty.is_ref() {
                format!("`{method}` needs a mutable reference")
            } else {
                format!("`{method}` needs a mutable name")
            };
            return Err(CompileError::at(self.source, span, message));
        }
        if mutate && !ty.is_ref() {
            self.note_use(span, binding, true)?;
        }
        Ok(binding)
    }

    fn show(&self, ty: Ty) -> String {
        match ty {
            Ty::Adt(id) => self.adts[id as usize].name.clone(),
            Ty::Vec(id) => format!("Vec<{}>", self.show(self.vec_elems[id as usize])),
            Ty::Slice(id) => format!("Slice<{}>", self.show(self.slice_elems[id as usize])),
            Ty::Array { id, len, .. } => {
                format!("[{}; {len}]", self.show(self.arrays[id as usize].elem))
            }
            Ty::Struct { id, .. } => self.structs[id as usize].name.clone(),
            Ty::Tuple { id, .. } => {
                let fields: Vec<String> = self.tuples[id as usize]
                    .iter()
                    .map(|ty| self.show(*ty))
                    .collect();
                format!("({})", fields.join(", "))
            }
            Ty::Ref(id) => {
                let refty = &self.refs[id as usize];
                let prefix = if refty.mutable { "&mut " } else { "&" };
                format!("{prefix}{}", self.show(refty.inner))
            }
            Ty::Ptr(id) => {
                let ptr = &self.ptrs[id as usize];
                let prefix = if ptr.mutable { "*mut " } else { "*const " };
                format!("{prefix}{}", self.show(ptr.inner))
            }
            Ty::Func(id) => {
                let sig = &self.funcs[id as usize];
                let params: Vec<String> =
                    sig.params.iter().map(|param| self.show(*param)).collect();
                format!("fn({}) -> {}", params.join(", "), self.show(sig.ret))
            }
            Ty::CFn(id) => {
                let sig = &self.cfns[id as usize];
                let params: Vec<String> =
                    sig.params.iter().map(|param| self.show(*param)).collect();
                format!(
                    "extern \"C\" fn({}) -> {}",
                    params.join(", "),
                    self.show(sig.ret)
                )
            }
            other => other.to_string(),
        }
    }

    fn check_lambda(
        &mut self,
        span: usize,
        params: &[crate::ast::Param],
        body: &Expr,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let expected_sig = match expected {
            Some(Ty::Func(id)) => Some(self.funcs[id as usize].clone()),
            Some(want) => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found a function", self.show(want)),
                ));
            }
            None => None,
        };
        if let Some(sig) = &expected_sig {
            if sig.params.len() != params.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {} parameters, found {}",
                        sig.params.len(),
                        params.len()
                    ),
                ));
            }
        }
        let mut sig_params = Vec::new();
        for (index, param) in params.iter().enumerate() {
            let ty = if let Some(parsed) = &param.ty {
                let ty = self.apply(parsed, &HashMap::new())?;
                if let Some(sig) = &expected_sig {
                    if sig.params[index] != ty {
                        return Err(CompileError::at(
                            self.source,
                            param.name_span.start,
                            format!(
                                "expected {}, `{}` has type {}",
                                self.show(sig.params[index]),
                                param.name,
                                self.show(ty)
                            ),
                        ));
                    }
                }
                ty
            } else if let Some(sig) = &expected_sig {
                sig.params[index]
            } else {
                Ty::I16
            };
            sig_params.push(ty);
        }
        let name = format!("__lambda{}", self.lambda_count);
        self.lambda_count += 1;
        let declared_ret = expected_sig.as_ref().map(|sig| sig.ret);
        self.sigs.insert(
            name.clone(),
            Sig {
                params: sig_params.clone(),
                ret: declared_ret,
                c_abi: false,
                variadic: false,
            },
        );
        let block = match &body.kind {
            ExprKind::Block(block) => (**block).clone(),
            _ => Block {
                span: body.span,
                stmts: Vec::new(),
                tail: Some(body.clone()),
            },
        };
        let func = Function {
            name: name.clone(),
            name_span: crate::ast::Span::new(span, span),
            type_params: Vec::new(),
            params: params.to_vec(),
            ret: None,
            body: block,
            c_export: false,
            exported: false,
        };
        self.check_fn(&func, &name, true, true)?;
        let ret = self.sigs[&name].ret.unwrap();
        let ty = self.intern_func(sig_params, ret);
        Ok(TExpr {
            ty,
            kind: TExprKind::Closure { func: name },
        })
    }

    fn nullary(
        &self,
        span: usize,
        name: &str,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let Some(&(schema, tag)) = self.ctor_schema.get(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown name `{name}`"),
            ));
        };
        if !self.schemas[schema].ctors[tag as usize].is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{name}` expects {} arguments",
                    self.schemas[schema].ctors[tag as usize].len()
                ),
            ));
        }
        let id = if let Some(Ty::Adt(id)) = expected {
            if self.adts[id as usize].schema != schema {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{name}` constructs {}",
                        self.show(Ty::Adt(id)),
                        self.schemas[schema].name
                    ),
                ));
            }
            id
        } else if self.schemas[schema].params.is_empty() {
            let id = self.mono[&(schema, Vec::new())];
            if let Some(want) = expected {
                if want != Ty::Adt(id) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, `{name}` has type {}",
                            self.show(want),
                            self.show(Ty::Adt(id))
                        ),
                    ));
                }
            }
            id
        } else if let Some(want) = expected {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected {}, `{name}` constructs {}",
                    self.show(want),
                    self.schemas[schema].name
                ),
            ));
        } else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("cannot infer type arguments for `{name}`"),
            ));
        };
        Ok(TExpr {
            ty: Ty::Adt(id),
            kind: TExprKind::Ctor {
                tag,
                args: Vec::new(),
            },
        })
    }

    fn check_ctor(
        &mut self,
        span: usize,
        name: &str,
        type_args: &[ParsedTy],
        args: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let (schema, tag) = self.ctor_schema[name];
        let params = self.schemas[schema].params.clone();
        let fields = self.schemas[schema].ctors[tag as usize].clone();
        if !type_args.is_empty() && params.is_empty() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` does not take type arguments"),
            ));
        }
        if args.len() != fields.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{name}` expects {} arguments, found {}",
                    fields.len(),
                    args.len()
                ),
            ));
        }
        let mut solved = vec![None; params.len()];
        if !type_args.is_empty() {
            if type_args.len() != params.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "`{name}` expects {} type arguments, found {}",
                        params.len(),
                        type_args.len()
                    ),
                ));
            }
            for (index, ty) in type_args.iter().enumerate() {
                solved[index] = Some(self.apply(ty, &HashMap::new())?);
            }
        } else if let Some(Ty::Adt(id)) = expected {
            if self.adts[id as usize].schema == schema {
                for (index, arg) in self.adts[id as usize].args.iter().enumerate() {
                    solved[index] = Some(*arg);
                }
            }
        }
        let mut checked = Vec::new();
        for (arg, field) in args.iter().zip(&fields) {
            if let Some(ty) = self.concrete_field(field, &params, &solved)? {
                let typed = self.check_expr(arg, Some(ty))?;
                if typed.ty != ty {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {}, found {}", self.show(ty), self.show(typed.ty)),
                    ));
                }
                checked.push(typed);
            } else {
                let typed = self.check_expr(arg, None)?;
                self.infer_field(field, typed.ty, &params, &mut solved, span)?;
                checked.push(typed);
            }
        }
        if solved.iter().any(|slot| slot.is_none()) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("cannot infer type arguments for `{name}`"),
            ));
        }
        let concrete: Vec<Ty> = solved.into_iter().map(Option::unwrap).collect();
        let id = self.monomorphize(schema, &concrete)?;
        let ty = Ty::Adt(id);
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, `{name}` has type {}",
                        self.show(want),
                        self.show(ty)
                    ),
                ));
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Ctor { tag, args: checked },
        })
    }

    fn concrete_field(
        &mut self,
        field: &ParsedTy,
        params: &[String],
        solved: &[Option<Ty>],
    ) -> Result<Option<Ty>, CompileError> {
        if mentions_unsolved(field, params, solved) {
            return Ok(None);
        }
        let mut subst = HashMap::new();
        for (index, param) in params.iter().enumerate() {
            if let Some(ty) = solved[index] {
                subst.insert(param.clone(), ty);
            }
        }
        Ok(Some(self.apply(field, &subst)?))
    }

    fn infer_field(
        &self,
        parsed: &ParsedTy,
        actual: Ty,
        params: &[String],
        solved: &mut [Option<Ty>],
        span: usize,
    ) -> Result<(), CompileError> {
        match parsed {
            ParsedTy::CFn { .. } => Err(CompileError::at(
                self.source,
                span,
                "cannot infer a type through an `extern \"C\"` function",
            )),
            ParsedTy::Func {
                params: fn_params,
                ret,
                ..
            } => {
                let Ty::Func(id) = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected a function, found {}", self.show(actual)),
                    ));
                };
                let sig = self.funcs[id as usize].clone();
                if fn_params.len() != sig.params.len() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {} parameters, found {}",
                            fn_params.len(),
                            sig.params.len()
                        ),
                    ));
                }
                for (param, got) in fn_params.iter().zip(sig.params) {
                    self.infer_field(param, got, params, solved, span)?;
                }
                self.infer_field(ret, sig.ret, params, solved, span)
            }
            ParsedTy::Ptr {
                span,
                mutable,
                inner,
            } => {
                let Ty::Ptr(id) = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected a pointer, found {}", self.show(actual)),
                    ));
                };
                if self.ptrs[id as usize].mutable != *mutable {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected a pointer, found {}", self.show(actual)),
                    ));
                }
                let inner_ty = self.ptrs[id as usize].inner;
                self.infer_field(inner, inner_ty, params, solved, span.start)
            }
            ParsedTy::Ref {
                span,
                mutable,
                inner,
            } => {
                let Ty::Ref(id) = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected a reference, found {}", self.show(actual)),
                    ));
                };
                let actual_mut = self.refs[id as usize].mutable;
                let inner_ty = self.refs[id as usize].inner;
                if actual_mut != *mutable && !(self.loose_receiver && actual_mut && !*mutable) {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!(
                            "expected a {}, found {}",
                            if *mutable {
                                "mutable reference"
                            } else {
                                "shared reference"
                            },
                            self.show(actual)
                        ),
                    ));
                }
                self.infer_field(inner, inner_ty, params, solved, span.start)
            }
            ParsedTy::Tuple { span, fields } => {
                let Ty::Tuple { id, .. } = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected a tuple, found {}", self.show(actual)),
                    ));
                };
                let got = self.tuples[id as usize].clone();
                if fields.len() != got.len() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {} fields, found {}", got.len(), fields.len()),
                    ));
                }
                for (field, ty) in fields.iter().zip(got) {
                    self.infer_field(field, ty, params, solved, span.start)?;
                }
                Ok(())
            }
            ParsedTy::Array { span, elem, len } => {
                let Ty::Array { id, len: got, .. } = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected an array, found {}", self.show(actual)),
                    ));
                };
                let (_, value) = self.eval_const(len, Some(Ty::Usize))?;
                if value != got as i64 {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected length {value}, found {got}"),
                    ));
                }
                self.infer_field(
                    elem,
                    self.arrays[id as usize].elem,
                    params,
                    solved,
                    span.start,
                )
            }
            ParsedTy::Named {
                span: name_span,
                name,
                args,
            } => {
                if let Some(index) = params.iter().position(|param| param == name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            format!("type parameter `{name}` does not take arguments"),
                        ));
                    }
                    match solved[index] {
                        None => solved[index] = Some(actual),
                        Some(existing) if existing == actual => {}
                        Some(existing) => {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                format!(
                                    "expected {}, found {}",
                                    self.show(existing),
                                    self.show(actual)
                                ),
                            ));
                        }
                    }
                    return Ok(());
                }
                if let Some(prim) = Ty::parse_name(name) {
                    if prim != actual {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {prim}, found {}", self.show(actual)),
                        ));
                    }
                    return Ok(());
                }
                if let Some(id) = self.struct_names.get(name).copied() {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            name_span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    let Ty::Struct { id: got, .. } = actual else {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {name}, found {}", self.show(actual)),
                        ));
                    };
                    if got != id {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {name}, found {}", self.show(actual)),
                        ));
                    }
                    return Ok(());
                }
                if name == "Vec" || name == "Slice" {
                    if args.len() != 1 {
                        return Err(self.arity_error(name_span.start, name, 1, args.len()));
                    }
                    let elem = if name == "Vec" {
                        let Ty::Vec(id) = actual else {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                format!("expected Vec, found {}", self.show(actual)),
                            ));
                        };
                        self.vec_elems[id as usize]
                    } else {
                        let Ty::Slice(id) = actual else {
                            return Err(CompileError::at(
                                self.source,
                                span,
                                format!("expected Slice, found {}", self.show(actual)),
                            ));
                        };
                        self.slice_elems[id as usize]
                    };
                    return self.infer_field(&args[0], elem, params, solved, span);
                }
                let Some(schema) = self.schemas.iter().position(|schema| schema.name == *name)
                else {
                    return Err(CompileError::at(
                        self.source,
                        name_span.start,
                        format!("unknown type `{name}`"),
                    ));
                };
                let Ty::Adt(id) = actual else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {name}, found {}", self.show(actual)),
                    ));
                };
                if self.adts[id as usize].schema != schema {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("expected {name}, found {}", self.show(actual)),
                    ));
                }
                let concrete = self.adts[id as usize].args.clone();
                if args.len() != concrete.len() {
                    return Err(self.arity_error(
                        name_span.start,
                        name,
                        concrete.len(),
                        args.len(),
                    ));
                }
                for (arg, got) in args.iter().zip(concrete) {
                    self.infer_field(arg, got, params, solved, span)?;
                }
                Ok(())
            }
        }
    }

    fn check_match(
        &mut self,
        span: usize,
        scrutinee: &Expr,
        arms: &[crate::ast::Arm],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let scrutinee = self.check_expr(scrutinee, None)?;
        let adt = match scrutinee.ty {
            Ty::Adt(id) => Some(id),
            Ty::Bool | Ty::Char | Ty::Struct { .. } | Ty::Tuple { .. } => None,
            ty if ty.is_int() => None,
            ty => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("match expects a data type, found {}", self.show(ty)),
                ));
            }
        };
        let mut covered = vec![
            false;
            adt.map(|id| self.adts[id as usize].ctors.len())
                .unwrap_or(0)
        ];
        let mut seen_lits: Vec<i64> = Vec::new();
        let mut seen_total = false;
        let mut result: Option<Ty> = expected;
        let mut typed = Vec::new();
        for arm in arms {
            let adt_done = adt.is_some() && covered.iter().all(|hit| *hit);
            let bool_done =
                scrutinee.ty == Ty::Bool && seen_lits.contains(&0) && seen_lits.contains(&1);
            if seen_total || adt_done || bool_done {
                return Err(CompileError::at(
                    self.source,
                    arm_span(&arm.pat),
                    "unreachable pattern",
                ));
            }
            self.scopes.push(HashMap::new());
            let arm_scope = Span::new(arm.pat.span().end, arm.body.span.end);
            self.scope_spans.push(arm_scope);
            let pat = self.check_pat(&arm.pat, scrutinee.ty, false)?;
            if let TPat::Ctor { tag, .. } = &pat {
                if adt.is_some() && covered[*tag as usize] {
                    return Err(CompileError::at(
                        self.source,
                        arm_span(&arm.pat),
                        "unreachable pattern",
                    ));
                }
            }
            if let TPat::Lit(value) = &pat {
                if seen_lits.contains(value) {
                    return Err(CompileError::at(
                        self.source,
                        arm_span(&arm.pat),
                        "unreachable pattern",
                    ));
                }
            }
            let guard = match &arm.guard {
                Some(expr) => {
                    let guard = self.check_expr(expr, Some(Ty::Bool))?;
                    if guard.ty != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            expr.span.start,
                            format!("a guard must be bool, found {}", self.show(guard.ty)),
                        ));
                    }
                    Some(guard)
                }
                None => None,
            };
            if guard.is_none() {
                match &pat {
                    TPat::Wild | TPat::Bind(_) => seen_total = true,
                    TPat::Ctor { tag, fields }
                        if fields.iter().all(|field| field_irrefutable(&field.pat)) =>
                    {
                        if adt.is_some() {
                            covered[*tag as usize] = true;
                        }
                    }
                    TPat::Ctor { .. } => {}
                    TPat::Lit(value) => seen_lits.push(*value),
                    TPat::Product { fields }
                        if fields.iter().all(|field| field_irrefutable(&field.pat)) =>
                    {
                        seen_total = true;
                    }
                    TPat::Product { .. } => {}
                }
            }
            let body = match &arm.body.kind {
                ExprKind::Block(block) => self.check_block(block, result)?,
                _ => {
                    let expr = self.check_expr(&arm.body, result)?;
                    TBlock {
                        stmts: Vec::new(),
                        tail: Some(expr),
                    }
                }
            };
            self.pop_scope();
            self.scope_spans.pop();
            let arm_ty = body.tail.as_ref().map(|expr| expr.ty).unwrap_or(Ty::Unit);
            if arm_ty.is_ref() {
                return Err(CompileError::at(
                    self.source,
                    arm.body.span.start,
                    "a reference cannot escape this match",
                ));
            }
            if let Some(want) = result {
                if arm_ty != want {
                    return Err(CompileError::at(
                        self.source,
                        arm.body.span.start,
                        format!(
                            "expected {}, this arm has type {}",
                            self.show(want),
                            self.show(arm_ty)
                        ),
                    ));
                }
            }
            result = Some(arm_ty);
            typed.push(TArm { pat, guard, body });
        }
        let bool_done =
            scrutinee.ty == Ty::Bool && seen_lits.contains(&0) && seen_lits.contains(&1);
        let adt_done = adt.is_some() && covered.iter().all(|hit| *hit) && !covered.is_empty();
        let open_scalar = (scrutinee.ty.is_int() || scrutinee.ty == Ty::Char) && !seen_total;
        if !seen_total && !bool_done && !adt_done || open_scalar {
            return Err(CompileError::at(self.source, span, "non-exhaustive match"));
        }
        let ty = result.unwrap_or(Ty::Unit);
        Ok(TExpr {
            ty,
            kind: TExprKind::Match {
                scrutinee: Box::new(scrutinee),
                arms: typed,
            },
        })
    }

    fn check_pat(&mut self, pat: &Pat, ty: Ty, mutable: bool) -> Result<TPat, CompileError> {
        let mut bound = Vec::new();
        self.check_pat_at(pat, ty, mutable, &mut bound)
    }

    fn check_pat_at(
        &mut self,
        pat: &Pat,
        ty: Ty,
        mutable: bool,
        bound: &mut Vec<String>,
    ) -> Result<TPat, CompileError> {
        match pat {
            Pat::Name { name, .. } if name == "_" => Ok(TPat::Wild),
            Pat::Name { name, span } => {
                if self.ctor_schema.contains_key(name) {
                    let (tag, fields) = self.ctor_fields(name, ty, span.start)?;
                    if !fields.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` expects {} fields", fields.len()),
                        ));
                    }
                    return Ok(TPat::Ctor {
                        tag,
                        fields: Vec::new(),
                    });
                }
                if bound.iter().any(|found| found == name) {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("`{name}` is bound twice in this pattern"),
                    ));
                }
                bound.push(name.clone());
                let id = self.bind_spanned(name.clone(), ty, mutable, *span);
                if mutable && self.shared_spans.contains(&span.start) {
                    if !shareable(ty) {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{}` cannot be captured mutably", self.show(ty)),
                        ));
                    }
                    self.bindings[id as usize].shared = true;
                }
                Ok(TPat::Bind(id))
            }
            Pat::Ctor {
                name,
                span,
                fields: pats,
            } => {
                let (tag, field_tys) = self.ctor_fields(name, ty, span.start)?;
                if pats.len() != field_tys.len() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!(
                            "`{name}` expects {} fields, found {}",
                            field_tys.len(),
                            pats.len()
                        ),
                    ));
                }
                let mut fields = Vec::new();
                for (pat, field_ty) in pats.iter().zip(field_tys) {
                    fields.push(FieldPat {
                        ty: field_ty,
                        pat: Box::new(self.check_pat_at(pat, field_ty, mutable, bound)?),
                    });
                }
                Ok(TPat::Ctor { tag, fields })
            }
            Pat::Int { value, span } => {
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found an integer pattern", self.show(ty)),
                    ));
                }
                if !ty.in_range(*value) {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("{value} does not fit in {}", self.show(ty)),
                    ));
                }
                Ok(TPat::Lit(*value))
            }
            Pat::Bool { value, span } => {
                if ty != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found a bool pattern", self.show(ty)),
                    ));
                }
                Ok(TPat::Lit(i64::from(*value)))
            }
            Pat::Char { scalar, span } => {
                if ty != Ty::Char {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found a char pattern", self.show(ty)),
                    ));
                }
                Ok(TPat::Lit(i64::from(*scalar)))
            }
            Pat::Tuple { span, fields } => {
                let Ty::Tuple { id, .. } = ty else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found a tuple pattern", self.show(ty)),
                    ));
                };
                let field_tys = self.tuples[id as usize].clone();
                if fields.len() != field_tys.len() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!(
                            "expected {} fields, found {}",
                            field_tys.len(),
                            fields.len()
                        ),
                    ));
                }
                Ok(TPat::Product {
                    fields: self.product_fields(fields, &field_tys, mutable, bound)?,
                })
            }
            Pat::Struct { name, span, fields } => {
                let Some(id) = self.struct_names.get(name).copied() else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown struct `{name}`"),
                    ));
                };
                let Ty::Struct { id: got, .. } = ty else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found `{name}`", self.show(ty)),
                    ));
                };
                if got != id {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("expected {}, found `{name}`", self.show(ty)),
                    ));
                }
                let info = self.structs[id as usize].fields.clone();
                if fields.len() != info.len() {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!(
                            "`{name}` expects {} fields, found {}",
                            info.len(),
                            fields.len()
                        ),
                    ));
                }
                let mut ordered = Vec::new();
                for (field_name, field_ty) in &info {
                    let Some((_, _, pat)) = fields.iter().find(|(name, _, _)| name == field_name)
                    else {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("missing field `{field_name}`"),
                        ));
                    };
                    ordered.push(self.check_pat_at(pat, *field_ty, mutable, bound)?);
                }
                let mut offset = 0i64;
                let mut product = Vec::new();
                for ((_, field_ty), pat) in info.iter().zip(ordered) {
                    product.push(ProductField {
                        offset,
                        ty: *field_ty,
                        pat: Box::new(pat),
                    });
                    offset += field_ty.cells();
                }
                Ok(TPat::Product { fields: product })
            }
        }
    }

    fn product_fields(
        &mut self,
        pats: &[Pat],
        tys: &[Ty],
        mutable: bool,
        bound: &mut Vec<String>,
    ) -> Result<Vec<ProductField>, CompileError> {
        let mut fields = Vec::new();
        let mut offset = 0i64;
        for (pat, ty) in pats.iter().zip(tys) {
            fields.push(ProductField {
                offset,
                ty: *ty,
                pat: Box::new(self.check_pat_at(pat, *ty, mutable, bound)?),
            });
            offset += ty.cells();
        }
        Ok(fields)
    }

    /// A pattern is irrefutable when it cannot fail: a binding, `_`, or the only constructor of its type.
    fn irrefutable_pat(&self, pat: &TPat, ty: Ty) -> bool {
        match pat {
            TPat::Wild | TPat::Bind(_) => true,
            TPat::Lit(_) => false,
            TPat::Product { fields } => fields
                .iter()
                .all(|field| self.irrefutable_pat(&field.pat, field.ty)),
            TPat::Ctor { tag, fields } => {
                let Ty::Adt(id) = ty else {
                    return false;
                };
                self.adts[id as usize].ctors.len() == 1
                    && *tag == 0
                    && fields
                        .iter()
                        .all(|field| self.irrefutable_pat(&field.pat, field.ty))
            }
        }
    }

    fn eval_consts(&mut self, consts: &[crate::ast::Const]) -> Result<(), CompileError> {
        for item in consts {
            if self.consts.contains_key(&item.name) {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!("duplicate constant `{}`", item.name),
                ));
            }
            if self.ast_fns.iter().any(|func| func.name == item.name) {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!("`{}` is already a function", item.name),
                ));
            }
            let ty = self.apply(&item.ty, &HashMap::new())?;
            if !ty.is_int() && !ty.is_float() && ty != Ty::Bool {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!(
                        "a constant must be a number or bool, found {}",
                        self.show(ty)
                    ),
                ));
            }
            let (got, value) = self.eval_const(&item.init, Some(ty))?;
            if got != ty {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!(
                        "`{}` has type {}, the initializer has type {}",
                        item.name,
                        self.show(ty),
                        self.show(got)
                    ),
                ));
            }
            self.consts.insert(item.name.clone(), (ty, value));
        }
        Ok(())
    }

    fn const_hint(&self, expr: &Expr) -> Option<Ty> {
        match &expr.kind {
            ExprKind::Int(_) | ExprKind::Float(_) => None,
            ExprKind::Bool(_) => Some(Ty::Bool),
            ExprKind::Name(name) => self.consts.get(name).map(|(ty, _)| *ty),
            ExprKind::Unary {
                op: UnaryOp::Neg | UnaryOp::BitNot,
                expr,
            } => self.const_hint(expr),
            ExprKind::Unary {
                op: UnaryOp::Not, ..
            } => Some(Ty::Bool),
            ExprKind::Binary { op, .. } if op.is_compare() || op.is_logic() => Some(Ty::Bool),
            ExprKind::Binary { lhs, rhs, .. } => {
                self.const_hint(lhs).or_else(|| self.const_hint(rhs))
            }
            _ => None,
        }
    }

    fn eval_const(&self, expr: &Expr, expected: Option<Ty>) -> Result<(Ty, i64), CompileError> {
        let span = expr.span.start;
        match &expr.kind {
            ExprKind::Int(value) => {
                let ty = match expected {
                    Some(ty) if ty.is_int() => ty,
                    Some(ty) => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found an integer literal", self.show(ty)),
                        ));
                    }
                    None => Ty::I16,
                };
                if !ty.in_range(*value) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("{value} does not fit in {ty}"),
                    ));
                }
                Ok((ty, *value))
            }
            ExprKind::Float(text) => {
                let value = parse_float_literal(self.source, span, text)?;
                let ty = match expected {
                    Some(ty) if ty.is_float() => ty,
                    Some(ty) => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found a float literal", self.show(ty)),
                        ));
                    }
                    None => Ty::F64,
                };
                let bits = float_literal_bits(ty, value)
                    .map_err(|msg| CompileError::at(self.source, span, msg))?;
                Ok((ty, bits as i64))
            }
            ExprKind::Bool(value) => {
                if let Some(want) = expected {
                    if want != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found bool", self.show(want)),
                        ));
                    }
                }
                Ok((Ty::Bool, i64::from(*value)))
            }
            ExprKind::Name(name) => {
                let Some((ty, value)) = self.consts.get(name).copied() else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("unknown constant `{name}`"),
                    ));
                };
                if let Some(want) = expected {
                    if want != ty {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!(
                                "expected {}, `{name}` has type {}",
                                self.show(want),
                                self.show(ty)
                            ),
                        ));
                    }
                }
                Ok((ty, value))
            }
            ExprKind::Unary {
                op: UnaryOp::Neg,
                expr: inner,
            } => {
                let (ty, value) =
                    self.eval_const(inner, expected.filter(|ty| ty.is_int() || ty.is_float()))?;
                if ty.is_float() {
                    let sign = if ty == Ty::F32 {
                        0x8000_0000u64
                    } else {
                        1u64 << 63
                    };
                    return Ok((ty, (value as u64 ^ sign) as i64));
                }
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("cannot negate {}", self.show(ty)),
                    ));
                }
                let Some(value) = value.checked_neg() else {
                    return Err(CompileError::at(self.source, span, "constant overflow"));
                };
                if !ty.in_range(value) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("{value} does not fit in {ty}"),
                    ));
                }
                Ok((ty, value))
            }
            ExprKind::Unary {
                op: UnaryOp::Not,
                expr: inner,
            } => {
                let (ty, value) = self.eval_const(inner, Some(Ty::Bool))?;
                if ty != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "`!` in a constant expects bool",
                    ));
                }
                Ok((Ty::Bool, i64::from(value == 0)))
            }
            ExprKind::Binary { op, lhs, rhs } if op.is_logic() => {
                if let Some(want) = expected {
                    if want != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found bool", self.show(want)),
                        ));
                    }
                }
                let (left_ty, left) = self.eval_const(lhs, Some(Ty::Bool))?;
                let (right_ty, right) = self.eval_const(rhs, Some(Ty::Bool))?;
                if left_ty != Ty::Bool || right_ty != Ty::Bool {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        "`&&` and `||` expect bool",
                    ));
                }
                let value = match op {
                    BinOp::And => left != 0 && right != 0,
                    BinOp::Or => left != 0 || right != 0,
                    _ => false,
                };
                Ok((Ty::Bool, i64::from(value)))
            }
            ExprKind::Binary { op, lhs, rhs } if op.is_compare() => {
                if let Some(want) = expected {
                    if want != Ty::Bool {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            format!("expected {}, found bool", self.show(want)),
                        ));
                    }
                }
                let operand = self
                    .const_hint(lhs)
                    .filter(|ty| ty.is_int() || ty.is_float())
                    .or_else(|| {
                        self.const_hint(rhs)
                            .filter(|ty| ty.is_int() || ty.is_float())
                    })
                    .unwrap_or_else(|| number_default(lhs, rhs));
                let (left_ty, left) = self.eval_const(lhs, Some(operand))?;
                let (right_ty, right) = self.eval_const(rhs, Some(operand))?;
                if left_ty != operand || right_ty != operand {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "cannot compare {} and {}",
                            self.show(left_ty),
                            self.show(right_ty)
                        ),
                    ));
                }
                if operand.is_float() {
                    let value = float_compare(operand, *op, left as u64, right as u64);
                    return Ok((Ty::Bool, i64::from(value)));
                }
                let value = match op {
                    BinOp::Eq => left == right,
                    BinOp::Ne => left != right,
                    BinOp::Lt => const_less(operand, left, right),
                    BinOp::Le => !const_less(operand, right, left),
                    BinOp::Gt => const_less(operand, right, left),
                    BinOp::Ge => !const_less(operand, left, right),
                    _ => false,
                };
                Ok((Ty::Bool, i64::from(value)))
            }
            ExprKind::Unary {
                op: UnaryOp::BitNot,
                expr: inner,
            } => {
                let (ty, value) = self.eval_const(inner, expected.filter(|ty| ty.is_int()))?;
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("`~` expects an integer, found {}", self.show(ty)),
                    ));
                }
                Ok((ty, fold_bitnot(ty, value)))
            }
            ExprKind::Binary { op, lhs, rhs } if op.is_bitwise() => {
                let ty = expected
                    .filter(|ty| ty.is_int())
                    .or_else(|| self.const_hint(lhs))
                    .or_else(|| self.const_hint(rhs))
                    .unwrap_or_else(|| number_default(lhs, rhs));
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("operator expects an integer, found {}", self.show(ty)),
                    ));
                }
                let (_, left) = self.eval_const(lhs, Some(ty))?;
                let (_, right) = self.eval_const(rhs, Some(ty))?;
                Ok((ty, fold_bitwise(ty, *op, left, right)))
            }
            ExprKind::Binary { op, lhs, rhs } if op.is_arith() => {
                let ty = expected
                    .filter(|ty| ty.is_int() || ty.is_float())
                    .or_else(|| self.const_hint(lhs))
                    .or_else(|| self.const_hint(rhs))
                    .unwrap_or_else(|| number_default(lhs, rhs));
                if ty.is_float() {
                    if *op == BinOp::Rem {
                        return Err(CompileError::at(self.source, span, "`%` expects integers"));
                    }
                    let (_, left) = self.eval_const(lhs, Some(ty))?;
                    let (_, right) = self.eval_const(rhs, Some(ty))?;
                    return Ok((ty, float_arith(ty, *op, left as u64, right as u64) as i64));
                }
                if !ty.is_int() {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("operator expects a number, found {}", self.show(ty)),
                    ));
                }
                let (_, left) = self.eval_const(lhs, Some(ty))?;
                let (_, right) = self.eval_const(rhs, Some(ty))?;
                let value = match op {
                    BinOp::Add => left.checked_add(right),
                    BinOp::Sub => left.checked_sub(right),
                    BinOp::Mul => left.checked_mul(right),
                    BinOp::Div if right == 0 => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            "division by zero in a constant",
                        ))
                    }
                    BinOp::Rem if right == 0 => {
                        return Err(CompileError::at(
                            self.source,
                            span,
                            "division by zero in a constant",
                        ))
                    }
                    BinOp::Div => left.checked_div(right),
                    BinOp::Rem => left.checked_rem(right),
                    _ => None,
                };
                let Some(value) = value else {
                    return Err(CompileError::at(self.source, span, "constant overflow"));
                };
                if !ty.in_range(value) {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("{value} does not fit in {ty}"),
                    ));
                }
                Ok((ty, value))
            }
            _ => Err(CompileError::at(
                self.source,
                span,
                "not a constant expression",
            )),
        }
    }

    fn declare_structs(&mut self, items: &[crate::ast::StructItem]) -> Result<(), CompileError> {
        for item in items {
            if self.struct_names.contains_key(&item.name)
                || Ty::parse_name(&item.name).is_some()
                || item.name == "Vec"
                || item.name == "Slice"
            {
                return Err(CompileError::at(
                    self.source,
                    item.name_span.start,
                    format!("duplicate type `{}`", item.name),
                ));
            }
            let id = self.structs.len() as u32;
            self.struct_names.insert(item.name.clone(), id);
            if item.name_span.end > item.name_span.start {
                self.type_defs.insert(item.name.clone(), item.name_span);
            }
            self.structs.push(StructInfo {
                name: item.name.clone(),
                fields: Vec::new(),
                width: 0,
                repr_c: item.repr_c,
            });
        }
        Ok(())
    }

    fn finish_structs(&mut self, items: &[crate::ast::StructItem]) -> Result<(), CompileError> {
        for item in items {
            let id = self.struct_names[&item.name];
            let mut fields = Vec::new();
            let mut seen = HashMap::new();
            for field in &item.fields {
                if seen.contains_key(&field.name) {
                    return Err(CompileError::at(
                        self.source,
                        field.name_span.start,
                        format!("duplicate field `{}`", field.name),
                    ));
                }
                seen.insert(field.name.clone(), ());
                if escaped_ref(&field.ty) {
                    return Err(CompileError::at(
                        self.source,
                        field.ty.span().start,
                        "a reference cannot be stored in a type",
                    ));
                }
                self.validate_ty(&field.ty, &[])?;
                fields.push((field.name.clone(), self.apply(&field.ty, &HashMap::new())?));
            }
            self.structs[id as usize].fields = fields;
        }
        let mut widths = vec![0u16; self.structs.len()];
        for id in 0..self.structs.len() {
            let mut stack = Vec::new();
            widths[id] = self.measure_struct(id, &mut stack)?;
        }
        let mut tuple_w = vec![0u16; self.tuples.len()];
        for id in 0..self.tuples.len() {
            let mut sum = 0i64;
            for field in self.tuples[id].clone() {
                sum += self.raw_cells(field, &widths, &tuple_w);
            }
            if sum > u16::MAX as i64 {
                return Err(CompileError::at(self.source, 0, "tuple is too large"));
            }
            tuple_w[id] = sum as u16;
        }
        let mut struct_fields = Vec::new();
        for info in &self.structs {
            let mut fields = Vec::new();
            for (name, ty) in &info.fields {
                fields.push((
                    name.clone(),
                    rewrite_layout(*ty, &widths, &tuple_w, &self.arrays),
                ));
            }
            struct_fields.push(fields);
        }
        for (info, fields) in self.structs.iter_mut().zip(struct_fields) {
            info.fields = fields;
        }
        for (id, info) in self.structs.iter_mut().enumerate() {
            info.width = widths[id];
        }
        let mut tuple_fields = Vec::new();
        for fields in &self.tuples {
            tuple_fields.push(
                fields
                    .iter()
                    .map(|ty| rewrite_layout(*ty, &widths, &tuple_w, &self.arrays))
                    .collect::<Vec<_>>(),
            );
        }
        self.tuples = tuple_fields;
        let elems: Vec<Ty> = self
            .arrays
            .iter()
            .map(|info| rewrite_layout(info.elem, &widths, &tuple_w, &self.arrays))
            .collect();
        for (info, elem) in self.arrays.iter_mut().zip(elems) {
            info.elem = elem;
        }
        self.check_repr(items)
    }

    fn check_repr(&self, items: &[crate::ast::StructItem]) -> Result<(), CompileError> {
        for item in items {
            if !item.repr_c {
                continue;
            }
            let id = self.struct_names[&item.name];
            let info = &self.structs[id as usize];
            for (field, (_, ty)) in item.fields.iter().zip(&info.fields) {
                if self.c_layout_field(*ty) {
                    continue;
                }
                return Err(CompileError::at(
                    self.source,
                    field.ty.span().start,
                    format!(
                        "`{}` cannot be stored in a `#[repr(C)]` struct",
                        self.show(*ty)
                    ),
                ));
            }
        }
        Ok(())
    }

    fn c_layout_field(&self, ty: Ty) -> bool {
        if ty.is_int() || ty.is_float() || self.is_repr_struct(ty) {
            return true;
        }
        if let Ty::Ptr(id) = ty {
            return self.c_pointee(self.ptrs[id as usize].inner);
        }
        false
    }

    fn measure_struct(&self, id: usize, stack: &mut Vec<usize>) -> Result<u16, CompileError> {
        if stack.contains(&id) {
            return Err(CompileError::at(
                self.source,
                0,
                format!("struct `{}` contains itself", self.structs[id].name),
            ));
        }
        stack.push(id);
        let mut sum = 0i64;
        for (_, ty) in self.structs[id].fields.clone() {
            sum += self.measure_ty(ty, stack)?;
        }
        stack.pop();
        if sum > u16::MAX as i64 {
            return Err(CompileError::at(
                self.source,
                0,
                format!("struct `{}` is too large", self.structs[id].name),
            ));
        }
        Ok(sum as u16)
    }

    fn measure_ty(&self, ty: Ty, stack: &mut Vec<usize>) -> Result<i64, CompileError> {
        match ty {
            Ty::Struct { id, .. } => Ok(self.measure_struct(id as usize, stack)? as i64),
            Ty::Tuple { id, .. } => {
                let mut sum = 0i64;
                for field in self.tuples[id as usize].clone() {
                    sum += self.measure_ty(field, stack)?;
                }
                Ok(sum)
            }
            Ty::Array { id, len, .. } => {
                Ok(len as i64 * self.measure_ty(self.arrays[id as usize].elem, stack)?)
            }
            other => Ok(other.cells()),
        }
    }

    fn raw_cells(&self, ty: Ty, struct_w: &[u16], tuple_w: &[u16]) -> i64 {
        match ty {
            Ty::Struct { id, .. } => struct_w[id as usize] as i64,
            Ty::Tuple { id, .. } => tuple_w[id as usize] as i64,
            Ty::Array { id, len, .. } => {
                len as i64 * self.raw_cells(self.arrays[id as usize].elem, struct_w, tuple_w)
            }
            other => other.cells(),
        }
    }

    fn struct_width(&self, id: u32) -> u16 {
        self.structs[id as usize].width
    }

    fn intern_tuple(&mut self, span: usize, fields: Vec<Ty>) -> Result<Ty, CompileError> {
        if let Some(index) = self.tuples.iter().position(|got| got == &fields) {
            let width: i64 = self.tuples[index].iter().map(|ty| ty.cells()).sum();
            return Ok(Ty::Tuple {
                id: index as u32,
                width: width as u16,
            });
        }
        let width: i64 = fields.iter().map(|ty| ty.cells()).sum();
        if width > u16::MAX as i64 {
            return Err(CompileError::at(self.source, span, "tuple is too large"));
        }
        let id = self.tuples.len() as u32;
        self.tuples.push(fields);
        Ok(Ty::Tuple {
            id,
            width: width as u16,
        })
    }

    fn check_tuple(
        &mut self,
        span: usize,
        fields: &[Expr],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let expected_fields = match expected {
            Some(Ty::Tuple { id, .. }) => Some(self.tuples[id as usize].clone()),
            Some(want) => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found a tuple", self.show(want)),
                ));
            }
            None => None,
        };
        if let Some(want) = &expected_fields {
            if want.len() != fields.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {} fields, found {}", want.len(), fields.len()),
                ));
            }
        }
        let mut typed = Vec::new();
        for (index, field) in fields.iter().enumerate() {
            let want = expected_fields.as_ref().map(|fields| fields[index]);
            let value = self.check_expr(field, want)?;
            if let Some(want) = want {
                if value.ty != want {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!(
                            "expected {}, found {}",
                            self.show(want),
                            self.show(value.ty)
                        ),
                    ));
                }
            }
            typed.push(value);
        }
        let tys: Vec<Ty> = typed.iter().map(|expr| expr.ty).collect();
        let ty = self.intern_tuple(span, tys)?;
        Ok(TExpr {
            ty,
            kind: TExprKind::Record { fields: typed },
        })
    }

    fn check_struct_lit(
        &mut self,
        span: usize,
        name: &str,
        fields: &[(String, Expr)],
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let Some(id) = self.struct_names.get(name).copied() else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown struct `{name}`"),
            ));
        };
        let info = self.structs[id as usize].fields.clone();
        let width = self.structs[id as usize].width;
        if fields.len() != info.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{name}` expects {} fields, found {}",
                    info.len(),
                    fields.len()
                ),
            ));
        }
        let mut seen = HashMap::new();
        for (field, _) in fields {
            if seen.contains_key(field) {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("duplicate field `{field}`"),
                ));
            }
            seen.insert(field.clone(), ());
        }
        let mut ordered = Vec::new();
        for (field_name, field_ty) in &info {
            let Some((_, expr)) = fields.iter().find(|(found, _)| found == field_name) else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("missing field `{field_name}`"),
                ));
            };
            let value = self.check_expr(expr, Some(*field_ty))?;
            if value.ty != *field_ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!(
                        "expected {}, found {}",
                        self.show(*field_ty),
                        self.show(value.ty)
                    ),
                ));
            }
            ordered.push(value);
        }
        let ty = Ty::Struct { id, width };
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {name}", self.show(want)),
                ));
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Record { fields: ordered },
        })
    }

    fn check_field(
        &mut self,
        span: usize,
        base: &Expr,
        name: &str,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        let base = self.check_expr(base, None)?;
        let (offset, ty) = self.field_at(span, base.ty, name)?;
        if let Some(want) = expected {
            if want != ty {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {}, found {}", self.show(want), self.show(ty)),
                ));
            }
        }
        Ok(TExpr {
            ty,
            kind: TExprKind::Field {
                base: Box::new(base),
                offset,
            },
        })
    }

    fn field_at(&self, span: usize, ty: Ty, name: &str) -> Result<(i64, Ty), CompileError> {
        let Ty::Struct { id, .. } = ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("{} has no field `{name}`", self.show(ty)),
            ));
        };
        let mut offset = 0i64;
        for (field, field_ty) in &self.structs[id as usize].fields {
            if field == name {
                return Ok((offset, *field_ty));
            }
            offset += field_ty.cells();
        }
        Err(CompileError::at(
            self.source,
            span,
            format!("{} has no field `{name}`", self.show(ty)),
        ))
    }

    fn check_field_index(
        &mut self,
        name: &str,
        span: usize,
        fields: &[String],
        index: &Expr,
        value: &Expr,
    ) -> Result<TStmt, CompileError> {
        let Some(id) = self.resolve(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown name `{name}`"),
            ));
        };
        let root = self.bindings[id as usize].ty;
        if !self.place_mutable(id, root) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not mutable"),
            ));
        }
        self.note_use(span, id, true)?;
        let mut ty = root;
        let mut offset = 0i64;
        for field in fields {
            let (at, next) = self.field_at(span, self.referent(ty), field)?;
            offset += at;
            ty = next;
        }
        if let Ty::Ptr(_) = ty {
            let ptr = TExpr {
                ty,
                kind: TExprKind::Field {
                    base: Box::new(TExpr {
                        ty: root,
                        kind: TExprKind::Name(id),
                    }),
                    offset,
                },
            };
            return self.store_through_ptr(span, ptr, Some(index), value);
        }
        if matches!(ty, Ty::Slice(_)) || matches!(self.referent(ty), Ty::Slice(_)) {
            return Err(CompileError::at(self.source, span, "slices are read-only"));
        }
        let elem = if let Ty::Vec(vec_id) = self.referent(ty) {
            self.vec_elems[vec_id as usize]
        } else if let Ty::Array { id: array_id, .. } = ty {
            self.arrays[array_id as usize].elem
        } else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{}` is not a vector or array", fields.join(".")),
            ));
        };
        let index = self.check_expr(index, Some(Ty::Usize))?;
        if index.ty != Ty::Usize {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected usize, found {}", self.show(index.ty)),
            ));
        }
        let value = self.check_expr(value, Some(elem))?;
        if value.ty != elem {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "expected {}, found {}",
                    self.show(elem),
                    self.show(value.ty)
                ),
            ));
        }
        Ok(TStmt::IndexAssign {
            binding: id,
            offset,
            index,
            value,
            elem,
            container: ty,
        })
    }

    fn check_field_assign(
        &mut self,
        name: &str,
        span: usize,
        fields: &[String],
        value: &Expr,
    ) -> Result<TStmt, CompileError> {
        let Some(id) = self.resolve(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown name `{name}`"),
            ));
        };
        let mut ty = self.bindings[id as usize].ty;
        if !self.place_mutable(id, ty) {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not mutable"),
            ));
        }
        self.note_use(span, id, true)?;
        let mut offset = 0i64;
        for field in fields {
            let (at, next) = self.field_at(span, ty, field)?;
            offset += at;
            ty = next;
        }
        let value = self.check_expr(value, Some(ty))?;
        if value.ty != ty {
            return Err(CompileError::at(
                self.source,
                span,
                format!("expected {}, found {}", self.show(ty), self.show(value.ty)),
            ));
        }
        Ok(TStmt::FieldAssign {
            binding: id,
            offset,
            value,
        })
    }

    fn check_for(
        &mut self,
        span: usize,
        pat: &Pat,
        iter: &Expr,
        body: &Block,
        expected: Option<Ty>,
    ) -> Result<TExpr, CompileError> {
        if let Some(want) = expected {
            if want != Ty::Unit {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("expected {want}, `for` has type unit"),
                ));
            }
        }
        if let ExprKind::Range {
            start,
            end,
            inclusive,
        } = &iter.kind
        {
            return self.check_for_range(span, pat, start, end, *inclusive, body);
        }
        let iter = self.check_expr(iter, None)?;
        if let Some((nil, cons, elem)) = self.list_shape(iter.ty) {
            return self.check_for_list(pat, iter, nil, cons, elem, body);
        }
        let elem = self.element_of(iter.ty).ok_or_else(|| {
            CompileError::at(
                self.source,
                span,
                format!("cannot iterate over {}", self.show(iter.ty)),
            )
        })?;
        let snap = self.bind(format!("__snap{}", self.bindings.len()), iter.ty, false);
        let index = self.bind(format!("__idx{}", self.bindings.len()), Ty::Usize, true);
        self.scopes.push(HashMap::new());
        let elem_pat = self.check_pat(pat, elem, false)?;
        if !self.irrefutable_pat(&elem_pat, elem) {
            return Err(CompileError::at(
                self.source,
                pat.span().start,
                "a `for` pattern must be irrefutable",
            ));
        }
        self.loops += 1;
        let body = self.check_block(body, None)?;
        self.loops -= 1;
        self.pop_scope();
        let loaded = TExpr {
            ty: elem,
            kind: TExprKind::Index {
                base: Box::new(TExpr {
                    ty: iter.ty,
                    kind: TExprKind::Name(snap),
                }),
                index: Box::new(TExpr {
                    ty: Ty::Usize,
                    kind: TExprKind::Name(index),
                }),
                elem,
            },
        };
        let extract = match elem_pat {
            TPat::Bind(binding) => TStmt::Let {
                binding,
                init: loaded,
            },
            TPat::Wild => TStmt::Expr(loaded),
            other => TStmt::Destructure {
                pat: other,
                init: loaded,
            },
        };
        let iter_expr = TExpr {
            ty: Ty::Unit,
            kind: TExprKind::ForIter {
                index,
                start: Box::new(TExpr {
                    ty: Ty::Usize,
                    kind: TExprKind::Int(0),
                }),
                end: Box::new(TExpr {
                    ty: Ty::Usize,
                    kind: TExprKind::Len {
                        vec: Box::new(TExpr {
                            ty: iter.ty,
                            kind: TExprKind::Name(snap),
                        }),
                    },
                }),
                inclusive: false,
                extract: Some(Box::new(extract)),
                body: Box::new(body),
            },
        };
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Block(Box::new(TBlock {
                stmts: vec![TStmt::Let {
                    binding: snap,
                    init: iter,
                }],
                tail: Some(iter_expr),
            })),
        })
    }

    fn check_for_range(
        &mut self,
        span: usize,
        pat: &Pat,
        start: &Expr,
        end: &Expr,
        inclusive: bool,
        body: &Block,
    ) -> Result<TExpr, CompileError> {
        let bound = self.common_int(start, end);
        let start = self.check_expr(start, bound)?;
        if !start.ty.is_int() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("a range needs an integer, found {}", self.show(start.ty)),
            ));
        }
        let end_span = end.span.start;
        let end_hint = bound
            .or_else(|| self.peek(end).filter(|ty| ty.is_int()))
            .or(Some(start.ty));
        let end = self.check_expr(end, end_hint)?;
        if end.ty != start.ty {
            return Err(CompileError::at(
                self.source,
                end_span,
                format!(
                    "a range's bounds have types {} and {}",
                    self.show(start.ty),
                    self.show(end.ty)
                ),
            ));
        }
        self.scopes.push(HashMap::new());
        let bound = self.check_pat(pat, start.ty, false)?;
        let index = match bound {
            TPat::Bind(id) => id,
            TPat::Wild => self.bind(format!("__i{}", self.bindings.len()), start.ty, true),
            _ => {
                return Err(CompileError::at(
                    self.source,
                    pat.span().start,
                    "a range binds one name",
                ));
            }
        };
        self.loops += 1;
        let body = self.check_block(body, None)?;
        self.loops -= 1;
        self.pop_scope();
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::ForIter {
                index,
                start: Box::new(start),
                end: Box::new(end),
                inclusive,
                extract: None,
                body: Box::new(body),
            },
        })
    }

    /// An unsuffixed literal adopts the other bound. `0..text.len()` is `usize`.
    /// Two unsuffixed literals use the narrowest signed type that holds both.
    fn common_int(&self, start: &Expr, end: &Expr) -> Option<Ty> {
        let left = self.peek(start).filter(|ty| ty.is_int());
        let right = self.peek(end).filter(|ty| ty.is_int());
        match (left, right) {
            (Some(left), Some(right)) if left == right => Some(left),
            (Some(left), None) => Some(left),
            (None, Some(right)) => Some(right),
            (None, None) => match (literal_int(start), literal_int(end)) {
                (Some(start), Some(end)) => Some(signed_fit(start, end)),
                _ => None,
            },
            _ => None,
        }
    }

    fn list_shape(&self, ty: Ty) -> Option<(u16, u16, Ty)> {
        let Ty::Adt(id) = ty else {
            return None;
        };
        let adt = &self.adts[id as usize];
        if self.schemas[adt.schema].name != "List" {
            return None;
        }
        let mut nil = None;
        let mut cons = None;
        let mut elem = None;
        for (tag, ctor) in adt.ctors.iter().enumerate() {
            if ctor.fields.is_empty() {
                nil = Some(tag as u16);
            }
            if ctor.fields.len() == 2 && ctor.fields[1] == ty {
                cons = Some(tag as u16);
                elem = Some(ctor.fields[0]);
            }
        }
        Some((nil?, cons?, elem?))
    }

    fn check_for_list(
        &mut self,
        pat: &Pat,
        iter: TExpr,
        nil: u16,
        cons: u16,
        elem: Ty,
        body: &Block,
    ) -> Result<TExpr, CompileError> {
        let list_ty = iter.ty;
        let cursor = self.bind(format!("__cur{}", self.bindings.len()), list_ty, true);
        self.scopes.push(HashMap::new());
        let elem_pat = self.check_pat(pat, elem, false)?;
        if !self.irrefutable_pat(&elem_pat, elem) {
            return Err(CompileError::at(
                self.source,
                pat.span().start,
                "a `for` pattern must be irrefutable",
            ));
        }
        let rest = self.bind(format!("__rest{}", self.bindings.len()), list_ty, false);
        self.loops += 1;
        let user = self.check_block(body, None)?;
        self.loops -= 1;
        self.pop_scope();
        let mut stmts = vec![TStmt::Assign {
            binding: cursor,
            value: TExpr {
                ty: list_ty,
                kind: TExprKind::Name(rest),
            },
        }];
        stmts.extend(user.stmts);
        if let Some(tail) = user.tail {
            stmts.push(TStmt::Expr(tail));
        }
        let matched = TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Match {
                scrutinee: Box::new(TExpr {
                    ty: list_ty,
                    kind: TExprKind::Name(cursor),
                }),
                arms: vec![
                    TArm {
                        pat: TPat::Ctor {
                            tag: nil,
                            fields: Vec::new(),
                        },
                        guard: None,
                        body: TBlock {
                            stmts: vec![TStmt::Break],
                            tail: None,
                        },
                    },
                    TArm {
                        pat: TPat::Ctor {
                            tag: cons,
                            fields: vec![
                                FieldPat {
                                    ty: elem,
                                    pat: Box::new(elem_pat),
                                },
                                FieldPat {
                                    ty: list_ty,
                                    pat: Box::new(TPat::Bind(rest)),
                                },
                            ],
                        },
                        guard: None,
                        body: TBlock { stmts, tail: None },
                    },
                ],
            },
        };
        Ok(TExpr {
            ty: Ty::Unit,
            kind: TExprKind::Block(Box::new(TBlock {
                stmts: vec![TStmt::Let {
                    binding: cursor,
                    init: iter,
                }],
                tail: Some(TExpr {
                    ty: Ty::Unit,
                    kind: TExprKind::While {
                        cond: Box::new(TExpr {
                            ty: Ty::Bool,
                            kind: TExprKind::Bool(true),
                        }),
                        body: Box::new(TBlock {
                            stmts: Vec::new(),
                            tail: Some(matched),
                        }),
                    },
                }),
            })),
        })
    }

    fn infer_omitted(&mut self, functions: &mut [Function]) -> Result<(), CompileError> {
        for func in functions.iter_mut() {
            for param in &mut func.params {
                if param.ty.is_none() {
                    if let Pat::Struct { name, span, .. } = &param.pat {
                        param.ty = Some(ParsedTy::Named {
                            span: *span,
                            name: name.clone(),
                            args: Vec::new(),
                        });
                    }
                }
            }
        }
        let pending: Vec<usize> = functions
            .iter()
            .enumerate()
            .filter(|(_, func)| {
                func.type_params.is_empty() && func.params.iter().any(|param| param.ty.is_none())
            })
            .map(|(index, _)| index)
            .collect();
        if pending.is_empty() {
            return Ok(());
        }
        let mut solver = Solver::new();
        let mut sigs: HashMap<String, (Vec<u32>, u32)> = HashMap::new();
        for index in &pending {
            let func = &functions[*index];
            let mut params = Vec::new();
            for param in &func.params {
                let var = match &param.ty {
                    Some(ty) => self.parsed_var(&mut solver, ty, &HashMap::new())?,
                    None => solver.fresh(),
                };
                params.push(var);
            }
            let ret = match &func.ret {
                Some(ty) => self.parsed_var(&mut solver, ty, &HashMap::new())?,
                None => solver.fresh(),
            };
            sigs.insert(func.name.clone(), (params, ret));
        }
        for index in &pending {
            let func = functions[*index].clone();
            let (params, ret) = sigs[&func.name].clone();
            let mut scope = vec![HashMap::new()];
            for (param, var) in func.params.iter().zip(&params) {
                self.infer_pat(&mut solver, &param.pat, *var, &mut scope)?;
            }
            let body =
                self.infer_block(&mut solver, &func.body, &sigs, functions, &mut scope, ret)?;
            solver.unify(body, ret).map_err(|_| {
                CompileError::at(
                    self.source,
                    func.name_span.start,
                    format!("cannot infer the type of `{}`", func.name),
                )
            })?;
        }
        solver.default_numbers();
        for index in pending {
            let (params, ret) = sigs[&functions[index].name].clone();
            let mut free = Vec::new();
            let mut seen = HashMap::new();
            for var in params.iter().copied().chain(std::iter::once(ret)) {
                self.collect_free(&solver, var, &mut free, &mut seen);
            }
            if functions[index].name == "main" && !free.is_empty() {
                return Err(CompileError::at(
                    self.source,
                    functions[index].name_span.start,
                    "main cannot be generic",
                ));
            }
            let mut names = HashMap::new();
            for (slot, var) in free.iter().enumerate() {
                names.insert(*var, type_var_name(slot));
            }
            let func = &mut functions[index];
            func.type_params = free
                .iter()
                .enumerate()
                .map(|(slot, _)| TypeParam {
                    name: type_var_name(slot),
                    span: func.name_span,
                })
                .collect();
            for (param, var) in func.params.iter_mut().zip(&params) {
                param.ty = Some(self.render_var(&solver, *var, &names)?);
            }
            func.ret = Some(self.render_var(&solver, ret, &names)?);
        }
        Ok(())
    }

    fn parsed_var(
        &mut self,
        solver: &mut Solver,
        ty: &ParsedTy,
        subst: &HashMap<String, u32>,
    ) -> Result<u32, CompileError> {
        match ty {
            ParsedTy::Named { span, name, args } => {
                if let Some(var) = subst.get(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("type parameter `{name}` does not take arguments"),
                        ));
                    }
                    return Ok(*var);
                }
                if let Some(prim) = Ty::parse_name(name) {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(solver.concrete(prim));
                }
                if let Some(id) = self.struct_names.get(name).copied() {
                    if !args.is_empty() {
                        return Err(CompileError::at(
                            self.source,
                            span.start,
                            format!("`{name}` does not take type arguments"),
                        ));
                    }
                    return Ok(solver.structure(id));
                }
                if name == "Vec" || name == "Slice" {
                    let concrete = self.apply(ty, &HashMap::new())?;
                    return Ok(solver.concrete(concrete));
                }
                let Some(schema) = self.schemas.iter().position(|schema| schema.name == *name)
                else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown type `{name}`"),
                    ));
                };
                let mut vars = Vec::new();
                for arg in args {
                    vars.push(self.parsed_var(solver, arg, subst)?);
                }
                Ok(solver.adt(schema, vars))
            }
            ParsedTy::Func { params, ret, .. } => {
                let mut vars = Vec::new();
                for param in params {
                    vars.push(self.parsed_var(solver, param, subst)?);
                }
                let ret = self.parsed_var(solver, ret, subst)?;
                Ok(solver.func(vars, ret))
            }
            ParsedTy::Tuple { fields, .. } => {
                let mut vars = Vec::new();
                for field in fields {
                    vars.push(self.parsed_var(solver, field, subst)?);
                }
                Ok(solver.tuple(vars))
            }
            ParsedTy::Ref { span, .. } => Err(CompileError::at(
                self.source,
                span.start,
                "a reference cannot be inferred here",
            )),
            ParsedTy::Ptr { .. } | ParsedTy::CFn { .. } => {
                let concrete = self.apply(ty, &HashMap::new())?;
                Ok(solver.concrete(concrete))
            }
            ParsedTy::Array { span, .. } => {
                let concrete = self.apply(ty, &HashMap::new())?;
                let _ = span;
                Ok(solver.concrete(concrete))
            }
        }
    }

    fn infer_block(
        &mut self,
        solver: &mut Solver,
        block: &Block,
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &mut Vec<HashMap<String, u32>>,
        ret: u32,
    ) -> Result<u32, CompileError> {
        for stmt in &block.stmts {
            self.infer_stmt(solver, stmt, sigs, functions, scope, ret)?;
        }
        match &block.tail {
            Some(expr) => self.infer_expr(solver, expr, sigs, functions, scope, ret, None),
            None => Ok(solver.concrete(Ty::Unit)),
        }
    }

    fn infer_stmt(
        &mut self,
        solver: &mut Solver,
        stmt: &Stmt,
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &mut Vec<HashMap<String, u32>>,
        ret: u32,
    ) -> Result<(), CompileError> {
        match stmt {
            Stmt::Let {
                pattern, ty, init, ..
            } => {
                let expected = match ty {
                    Some(ty) => Some(self.parsed_var(solver, ty, &HashMap::new())?),
                    None => None,
                };
                let init = self.infer_expr(solver, init, sigs, functions, scope, ret, expected)?;
                if let Some(expected) = expected {
                    solver.unify(init, expected).map_err(|_| {
                        CompileError::at(self.source, pattern.span().start, "incompatible types")
                    })?;
                }
                self.infer_pat(solver, pattern, init, scope)
            }
            Stmt::Return { value, .. } => {
                let got = match value {
                    Some(expr) => {
                        self.infer_expr(solver, expr, sigs, functions, scope, ret, Some(ret))?
                    }
                    None => solver.concrete(Ty::Unit),
                };
                solver
                    .unify(got, ret)
                    .map_err(|_| CompileError::at(self.source, 0, "incompatible types"))?;
                Ok(())
            }
            Stmt::Assign { value, .. } => {
                self.infer_expr(solver, value, sigs, functions, scope, ret, None)?;
                Ok(())
            }
            Stmt::Expr(expr) => {
                self.infer_expr(solver, expr, sigs, functions, scope, ret, None)?;
                Ok(())
            }
            Stmt::Break { .. }
            | Stmt::Continue { .. }
            | Stmt::IndexAssign { .. }
            | Stmt::FieldAssign { .. }
            | Stmt::DerefAssign { .. } => Ok(()),
        }
    }

    fn infer_expr(
        &mut self,
        solver: &mut Solver,
        expr: &Expr,
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &mut Vec<HashMap<String, u32>>,
        ret: u32,
        expected: Option<u32>,
    ) -> Result<u32, CompileError> {
        let span = expr.span.start;
        let got = match &expr.kind {
            ExprKind::Int(_) => {
                let var = solver.fresh();
                solver.mark_numeric(var);
                var
            }
            ExprKind::Float(_) => {
                let var = solver.fresh();
                solver.mark_float(var);
                var
            }
            ExprKind::CStr(_) => solver.concrete(self.intern_ptr(false, Ty::I16)),
            ExprKind::Str(_) => solver.concrete(Ty::String),
            ExprKind::Char(_) => solver.concrete(Ty::Char),
            ExprKind::Bool(_) => solver.concrete(Ty::Bool),
            ExprKind::Name(name) => self.infer_name(solver, span, name, sigs, functions, scope)?,
            ExprKind::Unary { op, expr: inner } => {
                let inner = self.infer_expr(solver, inner, sigs, functions, scope, ret, None)?;
                match op {
                    UnaryOp::Not => {
                        let boolean = solver.concrete(Ty::Bool);
                        solver.unify(inner, boolean).map_err(|_| {
                            CompileError::at(self.source, span, "incompatible types")
                        })?;
                        solver.concrete(Ty::Bool)
                    }
                    UnaryOp::BitNot => {
                        solver.mark_numeric(inner);
                        inner
                    }
                    UnaryOp::Neg => {
                        let root = solver.find(inner);
                        let float_info = matches!(solver.info[root as usize], Some(IInfo::Concrete(ty)) if ty.is_float());
                        if !solver.floaty[root as usize] && !float_info {
                            solver.mark_numeric(inner);
                        }
                        inner
                    }
                }
            }
            ExprKind::Binary { op, lhs, rhs } => {
                let left = self.infer_expr(solver, lhs, sigs, functions, scope, ret, None)?;
                let right = self.infer_expr(solver, rhs, sigs, functions, scope, ret, None)?;
                if matches!(op, BinOp::And | BinOp::Or) {
                    let boolean = solver.concrete(Ty::Bool);
                    solver
                        .unify(left, boolean)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                    solver
                        .unify(right, boolean)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                    boolean
                } else {
                    solver
                        .unify(left, right)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                    if op.is_arith() || op.is_bitwise() {
                        let root = solver.find(left);
                        let float_info = matches!(solver.info[root as usize], Some(IInfo::Concrete(ty)) if ty.is_float());
                        if !solver.floaty[root as usize] && !float_info {
                            solver.mark_numeric(left);
                        }
                        left
                    } else {
                        solver.concrete(Ty::Bool)
                    }
                }
            }
            ExprKind::Block(block) | ExprKind::Unsafe(block) => {
                scope.push(HashMap::new());
                let ty = self.infer_block(solver, block, sigs, functions, scope, ret)?;
                scope.pop();
                ty
            }
            ExprKind::If {
                cond,
                then_body,
                else_body,
            } => {
                let cond = self.infer_expr(solver, cond, sigs, functions, scope, ret, None)?;
                let boolean = solver.concrete(Ty::Bool);
                solver
                    .unify(cond, boolean)
                    .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                scope.push(HashMap::new());
                let then_ty = self.infer_block(solver, then_body, sigs, functions, scope, ret)?;
                scope.pop();
                if let Some(else_body) = else_body {
                    let else_ty = self.infer_expr(
                        solver,
                        else_body,
                        sigs,
                        functions,
                        scope,
                        ret,
                        Some(then_ty),
                    )?;
                    solver
                        .unify(then_ty, else_ty)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                    then_ty
                } else {
                    solver.concrete(Ty::Unit)
                }
            }
            ExprKind::Match { scrutinee, arms } => {
                let scrut =
                    self.infer_expr(solver, scrutinee, sigs, functions, scope, ret, None)?;
                let mut result = expected;
                for arm in arms {
                    scope.push(HashMap::new());
                    self.infer_pat(solver, &arm.pat, scrut, scope)?;
                    if let Some(guard) = &arm.guard {
                        let guard =
                            self.infer_expr(solver, guard, sigs, functions, scope, ret, None)?;
                        let boolean = solver.concrete(Ty::Bool);
                        solver.unify(guard, boolean).map_err(|_| {
                            CompileError::at(self.source, span, "incompatible types")
                        })?;
                    }
                    let arm_ty =
                        self.infer_expr(solver, &arm.body, sigs, functions, scope, ret, result)?;
                    scope.pop();
                    if let Some(want) = result {
                        solver.unify(arm_ty, want).map_err(|_| {
                            CompileError::at(self.source, span, "incompatible types")
                        })?;
                    }
                    result = Some(arm_ty);
                }
                result.unwrap_or_else(|| solver.concrete(Ty::Unit))
            }
            ExprKind::Call { callee, args, .. } => {
                self.infer_call(solver, span, callee, args, sigs, functions, scope, ret)?
            }
            ExprKind::Tuple(fields) => {
                let mut vars = Vec::new();
                for field in fields {
                    vars.push(self.infer_expr(solver, field, sigs, functions, scope, ret, None)?);
                }
                solver.tuple(vars)
            }
            ExprKind::StructLit { name, fields } => {
                let Some(id) = self.struct_names.get(name).copied() else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("unknown struct `{name}`"),
                    ));
                };
                let info = self.structs[id as usize].fields.clone();
                for (field_name, field_ty) in &info {
                    let Some((_, expr)) = fields.iter().find(|(found, _)| found == field_name)
                    else {
                        continue;
                    };
                    let got = self.infer_expr(solver, expr, sigs, functions, scope, ret, None)?;
                    let want =
                        self.parsed_var(solver, &self.ty_to_parsed(*field_ty), &HashMap::new())?;
                    solver
                        .unify(got, want)
                        .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                }
                solver.structure(id)
            }
            other => {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("cannot infer parameter types through {other:?}; add parameter types"),
                ));
            }
        };
        if let Some(expected) = expected {
            solver
                .unify(got, expected)
                .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
        }
        Ok(got)
    }

    fn infer_name(
        &mut self,
        solver: &mut Solver,
        span: usize,
        name: &str,
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &[HashMap<String, u32>],
    ) -> Result<u32, CompileError> {
        for map in scope.iter().rev() {
            if let Some(var) = map.get(name) {
                return Ok(*var);
            }
        }
        if let Some((ty, _)) = self.consts.get(name) {
            return Ok(solver.concrete(*ty));
        }
        if self.ctor_schema.contains_key(name) {
            return self.infer_ctor(solver, span, name, &[]);
        }
        if let Some((params, ret)) = sigs.get(name) {
            return Ok(solver.func(params.clone(), *ret));
        }
        if let Some(func) = functions.iter().find(|func| func.name == name) {
            if !func.type_params.is_empty() {
                return Err(CompileError::at(self.source, span, format!("generic function `{name}` needs a concrete signature to be used as a value")));
            }
            let mut params = Vec::new();
            for param in &func.params {
                let Some(ty) = &param.ty else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("cannot infer a type for `{name}`"),
                    ));
                };
                params.push(self.parsed_var(solver, ty, &HashMap::new())?);
            }
            let ret = match &func.ret {
                Some(ty) => self.parsed_var(solver, ty, &HashMap::new())?,
                None => solver.fresh(),
            };
            return Ok(solver.func(params, ret));
        }
        Err(CompileError::at(
            self.source,
            span,
            format!("unknown name `{name}`"),
        ))
    }

    fn infer_call(
        &mut self,
        solver: &mut Solver,
        span: usize,
        callee: &str,
        args: &[Expr],
        sigs: &HashMap<String, (Vec<u32>, u32)>,
        functions: &[Function],
        scope: &mut Vec<HashMap<String, u32>>,
        ret: u32,
    ) -> Result<u32, CompileError> {
        let mut arg_vars = Vec::new();
        for arg in args {
            arg_vars.push(self.infer_expr(solver, arg, sigs, functions, scope, ret, None)?);
        }
        if is_builtin(callee) {
            if let Some(arg) = arg_vars.first() {
                if callee == "putc" {
                    solver.mark_numeric(*arg);
                }
            }
            return Ok(solver.concrete(Ty::Unit));
        }
        for map in scope.iter().rev() {
            if let Some(var) = map.get(callee) {
                let result = solver.fresh();
                let func = solver.func(arg_vars, result);
                solver
                    .unify(*var, func)
                    .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
                return Ok(result);
            }
        }
        if self.ctor_schema.contains_key(callee) {
            return self.infer_ctor(solver, span, callee, &arg_vars);
        }
        if let Some((params, result)) = sigs.get(callee) {
            if params.len() != arg_vars.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` expects {} arguments", params.len()),
                ));
            }
            for (param, arg) in params.iter().zip(arg_vars) {
                solver
                    .unify(*param, arg)
                    .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
            }
            return Ok(*result);
        }
        let Some(func) = functions.iter().find(|func| func.name == callee).cloned() else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown function `{callee}`"),
            ));
        };
        if !func.type_params.is_empty() {
            let mut subst = HashMap::new();
            for param in &func.type_params {
                subst.insert(param.name.clone(), solver.fresh());
            }
            if func.params.len() != arg_vars.len() {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("`{callee}` expects {} arguments", func.params.len()),
                ));
            }
            for (param, arg) in func.params.iter().zip(arg_vars) {
                let Some(ty) = &param.ty else {
                    return Err(CompileError::at(
                        self.source,
                        span,
                        format!("parameter `{}` needs a type", param.name),
                    ));
                };
                let var = self.parsed_var(solver, ty, &subst)?;
                solver
                    .unify(var, arg)
                    .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
            }
            let Some(ty) = &func.ret else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("function `{callee}` needs a return type"),
                ));
            };
            return self.parsed_var(solver, ty, &subst);
        }
        if func.params.len() != arg_vars.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{callee}` expects {} arguments", func.params.len()),
            ));
        }
        for (param, arg) in func.params.iter().zip(arg_vars) {
            let Some(ty) = &param.ty else {
                return Err(CompileError::at(
                    self.source,
                    span,
                    format!("cannot infer a type for `{callee}`"),
                ));
            };
            let var = self.parsed_var(solver, ty, &HashMap::new())?;
            solver
                .unify(var, arg)
                .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
        }
        match &func.ret {
            Some(ty) => self.parsed_var(solver, ty, &HashMap::new()),
            None => Ok(solver.fresh()),
        }
    }

    fn infer_ctor(
        &mut self,
        solver: &mut Solver,
        span: usize,
        name: &str,
        args: &[u32],
    ) -> Result<u32, CompileError> {
        let Some(&(schema, tag)) = self.ctor_schema.get(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown constructor `{name}`"),
            ));
        };
        let params = self.schemas[schema].params.clone();
        let fields = self.schemas[schema].ctors[tag as usize].clone();
        if fields.len() != args.len() {
            return Err(CompileError::at(
                self.source,
                span,
                format!(
                    "`{name}` expects {} arguments, found {}",
                    fields.len(),
                    args.len()
                ),
            ));
        }
        let mut subst = HashMap::new();
        for param in &params {
            subst.insert(param.clone(), solver.fresh());
        }
        for (field, arg) in fields.iter().zip(args) {
            let var = self.parsed_var(solver, field, &subst)?;
            solver
                .unify(var, *arg)
                .map_err(|_| CompileError::at(self.source, span, "incompatible types"))?;
        }
        let arg_vars: Vec<u32> = params.iter().map(|param| subst[param]).collect();
        Ok(solver.adt(schema, arg_vars))
    }

    fn infer_pat(
        &mut self,
        solver: &mut Solver,
        pat: &Pat,
        var: u32,
        scope: &mut Vec<HashMap<String, u32>>,
    ) -> Result<(), CompileError> {
        match pat {
            Pat::Name { name, .. } if name == "_" => Ok(()),
            Pat::Name { name, span } => {
                if self.ctor_schema.contains_key(name) {
                    let ctor = self.infer_ctor(solver, span.start, name, &[])?;
                    return solver.unify(var, ctor).map_err(|_| {
                        CompileError::at(self.source, span.start, "incompatible types")
                    });
                }
                scope.last_mut().unwrap().insert(name.clone(), var);
                Ok(())
            }
            Pat::Ctor { name, span, fields } => {
                let mut field_vars = Vec::new();
                for _ in fields {
                    field_vars.push(solver.fresh());
                }
                let ctor = self.infer_ctor(solver, span.start, name, &field_vars)?;
                solver
                    .unify(var, ctor)
                    .map_err(|_| CompileError::at(self.source, span.start, "incompatible types"))?;
                for (field, field_var) in fields.iter().zip(field_vars) {
                    self.infer_pat(solver, field, field_var, scope)?;
                }
                Ok(())
            }
            Pat::Int { .. } => {
                solver.mark_numeric(var);
                Ok(())
            }
            Pat::Char { span, .. } => {
                let ch = solver.concrete(Ty::Char);
                solver
                    .unify(var, ch)
                    .map_err(|_| CompileError::at(self.source, span.start, "incompatible types"))
            }
            Pat::Bool { span, .. } => {
                let boolean = solver.concrete(Ty::Bool);
                solver
                    .unify(var, boolean)
                    .map_err(|_| CompileError::at(self.source, span.start, "incompatible types"))
            }
            Pat::Tuple { fields, .. } => {
                let mut vars = Vec::new();
                for field in fields {
                    let field_var = solver.fresh();
                    self.infer_pat(solver, field, field_var, scope)?;
                    vars.push(field_var);
                }
                let tuple = solver.tuple(vars);
                solver.unify(var, tuple).map_err(|_| {
                    CompileError::at(self.source, pat.span().start, "incompatible types")
                })
            }
            Pat::Struct { name, span, fields } => {
                let Some(id) = self.struct_names.get(name).copied() else {
                    return Err(CompileError::at(
                        self.source,
                        span.start,
                        format!("unknown struct `{name}`"),
                    ));
                };
                let structured = solver.structure(id);
                solver
                    .unify(var, structured)
                    .map_err(|_| CompileError::at(self.source, span.start, "incompatible types"))?;
                let info = self.structs[id as usize].fields.clone();
                for (field_name, _, field_pat) in fields {
                    if let Some((_, ty)) = info.iter().find(|(found, _)| found == field_name) {
                        let field_var =
                            self.parsed_var(solver, &self.ty_to_parsed(*ty), &HashMap::new())?;
                        self.infer_pat(solver, field_pat, field_var, scope)?;
                    }
                }
                Ok(())
            }
        }
    }

    fn collect_free(
        &self,
        solver: &Solver,
        var: u32,
        free: &mut Vec<u32>,
        seen: &mut HashMap<u32, ()>,
    ) {
        let var = solver.find(var);
        match &solver.info[var as usize] {
            None => {
                if seen.insert(var, ()).is_none() {
                    free.push(var);
                }
            }
            Some(IInfo::Concrete(_)) | Some(IInfo::Struct(_)) => {}
            Some(IInfo::Func { params, ret }) => {
                for param in params.clone() {
                    self.collect_free(solver, param, free, seen);
                }
                self.collect_free(solver, *ret, free, seen);
            }
            Some(IInfo::Adt { args, .. }) => {
                for arg in args.clone() {
                    self.collect_free(solver, arg, free, seen);
                }
            }
            Some(IInfo::Tuple(fields)) => {
                for field in fields.clone() {
                    self.collect_free(solver, field, free, seen);
                }
            }
        }
    }

    fn render_var(
        &self,
        solver: &Solver,
        var: u32,
        names: &HashMap<u32, String>,
    ) -> Result<ParsedTy, CompileError> {
        let var = solver.find(var);
        if let Some(name) = names.get(&var) {
            return Ok(ParsedTy::Named {
                span: Span::empty(),
                name: name.clone(),
                args: Vec::new(),
            });
        }
        let Some(info) = solver.info[var as usize].clone() else {
            return Err(CompileError::at(self.source, 0, "cannot infer a type"));
        };
        match info {
            IInfo::Concrete(ty) => Ok(self.ty_to_parsed(ty)),
            IInfo::Struct(id) => Ok(ParsedTy::Named {
                span: Span::empty(),
                name: self.structs[id as usize].name.clone(),
                args: Vec::new(),
            }),
            IInfo::Func { params, ret } => {
                let mut parsed = Vec::new();
                for param in params {
                    parsed.push(self.render_var(solver, param, names)?);
                }
                Ok(ParsedTy::Func {
                    span: Span::empty(),
                    params: parsed,
                    ret: Box::new(self.render_var(solver, ret, names)?),
                })
            }
            IInfo::Adt { schema, args } => {
                let mut parsed = Vec::new();
                for arg in args {
                    parsed.push(self.render_var(solver, arg, names)?);
                }
                Ok(ParsedTy::Named {
                    span: Span::empty(),
                    name: self.schemas[schema].name.clone(),
                    args: parsed,
                })
            }
            IInfo::Tuple(fields) => {
                let mut parsed = Vec::new();
                for field in fields {
                    parsed.push(self.render_var(solver, field, names)?);
                }
                Ok(ParsedTy::Tuple {
                    span: Span::empty(),
                    fields: parsed,
                })
            }
        }
    }

    fn ty_to_parsed(&self, ty: Ty) -> ParsedTy {
        let span = Span::empty();
        match ty {
            Ty::Adt(id) => {
                let adt = &self.adts[id as usize];
                ParsedTy::Named {
                    span,
                    name: self.schemas[adt.schema].name.clone(),
                    args: adt.args.iter().map(|arg| self.ty_to_parsed(*arg)).collect(),
                }
            }
            Ty::Func(id) => {
                let sig = &self.funcs[id as usize];
                ParsedTy::Func {
                    span,
                    params: sig
                        .params
                        .iter()
                        .map(|param| self.ty_to_parsed(*param))
                        .collect(),
                    ret: Box::new(self.ty_to_parsed(sig.ret)),
                }
            }
            Ty::CFn(id) => {
                let sig = &self.cfns[id as usize];
                ParsedTy::CFn {
                    span,
                    params: sig
                        .params
                        .iter()
                        .map(|param| self.ty_to_parsed(*param))
                        .collect(),
                    ret: Box::new(self.ty_to_parsed(sig.ret)),
                }
            }
            Ty::Vec(id) => ParsedTy::Named {
                span,
                name: "Vec".to_string(),
                args: vec![self.ty_to_parsed(self.vec_elems[id as usize])],
            },
            Ty::Slice(id) => ParsedTy::Named {
                span,
                name: "Slice".to_string(),
                args: vec![self.ty_to_parsed(self.slice_elems[id as usize])],
            },
            Ty::Struct { id, .. } => ParsedTy::Named {
                span,
                name: self.structs[id as usize].name.clone(),
                args: Vec::new(),
            },
            Ty::Tuple { id, .. } => ParsedTy::Tuple {
                span,
                fields: self.tuples[id as usize]
                    .iter()
                    .map(|field| self.ty_to_parsed(*field))
                    .collect(),
            },
            Ty::Ref(id) => {
                let refty = &self.refs[id as usize];
                ParsedTy::Ref {
                    span,
                    mutable: refty.mutable,
                    inner: Box::new(self.ty_to_parsed(refty.inner)),
                }
            }
            Ty::Ptr(id) => {
                let ptr = &self.ptrs[id as usize];
                ParsedTy::Ptr {
                    span,
                    mutable: ptr.mutable,
                    inner: Box::new(self.ty_to_parsed(ptr.inner)),
                }
            }
            Ty::Array { id, len, .. } => ParsedTy::Array {
                span,
                elem: Box::new(self.ty_to_parsed(self.arrays[id as usize].elem)),
                len: Box::new(Expr {
                    span,
                    kind: ExprKind::Int(len as i64),
                }),
            },
            other => ParsedTy::Named {
                span,
                name: other.to_string(),
                args: Vec::new(),
            },
        }
    }

    fn ctor_fields(&self, name: &str, ty: Ty, span: usize) -> Result<(u16, Vec<Ty>), CompileError> {
        let Some(&(schema, tag)) = self.ctor_schema.get(name) else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("unknown constructor `{name}`"),
            ));
        };
        let Ty::Adt(id) = ty else {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not a constructor of {}", self.show(ty)),
            ));
        };
        if self.adts[id as usize].schema != schema {
            return Err(CompileError::at(
                self.source,
                span,
                format!("`{name}` is not a constructor of {}", self.show(ty)),
            ));
        }
        Ok((
            tag,
            self.adts[id as usize].ctors[tag as usize].fields.clone(),
        ))
    }
}

fn number_default(lhs: &Expr, rhs: &Expr) -> Ty {
    if has_float_lit(lhs) || has_float_lit(rhs) {
        Ty::F64
    } else {
        Ty::I16
    }
}

fn has_float_lit(expr: &Expr) -> bool {
    match &expr.kind {
        ExprKind::Float(_) => true,
        ExprKind::Unary { expr, .. } => has_float_lit(expr),
        ExprKind::Binary { lhs, rhs, .. } => has_float_lit(lhs) || has_float_lit(rhs),
        _ => false,
    }
}

fn parse_float_literal(source: &str, span: usize, text: &str) -> Result<f64, CompileError> {
    let value: f64 = text
        .parse()
        .map_err(|_| CompileError::at(source, span, format!("invalid float literal {text}")))?;
    if !value.is_finite() {
        return Err(CompileError::at(
            source,
            span,
            format!("{text} does not fit in f64"),
        ));
    }
    Ok(value)
}

fn float_literal_bits(ty: Ty, value: f64) -> Result<u64, String> {
    match ty {
        Ty::F32 => {
            let narrow = value as f32;
            if !narrow.is_finite() {
                return Err(format!("{value} does not fit in f32"));
            }
            Ok(u64::from(narrow.to_bits()))
        }
        Ty::F64 => Ok(value.to_bits()),
        _ => Err("not a float".to_string()),
    }
}

fn float_value(ty: Ty, bits: u64) -> f64 {
    if ty == Ty::F32 {
        f64::from(f32::from_bits(bits as u32))
    } else {
        f64::from_bits(bits)
    }
}

fn float_arith(ty: Ty, op: BinOp, left: u64, right: u64) -> u64 {
    let value = if ty == Ty::F32 {
        let left = f32::from_bits(left as u32);
        let right = f32::from_bits(right as u32);
        let value = match op {
            BinOp::Add => left + right,
            BinOp::Sub => left - right,
            BinOp::Mul => left * right,
            BinOp::Div => left / right,
            _ => 0.0,
        };
        return u64::from(value.to_bits());
    } else {
        let left = f64::from_bits(left);
        let right = f64::from_bits(right);
        match op {
            BinOp::Add => left + right,
            BinOp::Sub => left - right,
            BinOp::Mul => left * right,
            BinOp::Div => left / right,
            _ => 0.0,
        }
    };
    value.to_bits()
}

fn float_compare(ty: Ty, op: BinOp, left: u64, right: u64) -> bool {
    let left = float_value(ty, left);
    let right = float_value(ty, right);
    match op {
        BinOp::Eq => left == right,
        BinOp::Ne => left != right,
        BinOp::Lt => left < right,
        BinOp::Le => left <= right,
        BinOp::Gt => left > right,
        BinOp::Ge => left >= right,
        _ => false,
    }
}

fn can_cast(from: Ty, to: Ty) -> bool {
    if from == Ty::Char {
        return to == Ty::Char || to.is_int();
    }
    if to == Ty::Char {
        return from.is_int();
    }
    (from.is_int() || from.is_float()) && (to.is_int() || to.is_float())
}

fn unicode_scalar(value: i64) -> bool {
    (0..=0x10FFFF).contains(&value) && !(0xD800..=0xDFFF).contains(&value)
}

fn utf8_bytes(chars: &[u16]) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    for &cell in chars {
        let Some(ch) = char::from_u32(u32::from(cell)) else {
            return Err("string contains a surrogate".to_string());
        };
        let mut buf = [0u8; 4];
        bytes.extend(ch.encode_utf8(&mut buf).as_bytes());
    }
    Ok(bytes)
}

fn fold_cast(from: Ty, to: Ty, kind: &TExprKind) -> Option<Result<TExprKind, String>> {
    match kind {
        TExprKind::Char(scalar) if to == Ty::Char => Some(Ok(TExprKind::Char(*scalar))),
        TExprKind::Char(scalar) if to.is_int() => {
            let value = i64::from(*scalar);
            if !to.in_range(value) {
                return Some(Err(format!("{value} does not fit in {to}")));
            }
            Some(Ok(TExprKind::Int(value)))
        }
        TExprKind::Int(value) if to == Ty::Char => {
            if !unicode_scalar(*value) {
                return Some(Err(format!("{value} is not a Unicode scalar")));
            }
            Some(Ok(TExprKind::Char(*value as u32)))
        }
        TExprKind::Int(value) if from == to && to.is_int() => Some(Ok(TExprKind::Int(*value))),
        TExprKind::Int(value) if to.is_int() => {
            if !to.in_range(*value) {
                return Some(Err(format!("{value} does not fit in {to}")));
            }
            Some(Ok(TExprKind::Int(*value)))
        }
        TExprKind::Int(value) if to.is_float() => {
            Some(Ok(TExprKind::Float(int_to_float_bits(from, to, *value))))
        }
        TExprKind::Float(bits) if from == to => Some(Ok(TExprKind::Float(*bits))),
        TExprKind::Float(bits) if to.is_float() => {
            match float_literal_bits(to, float_value(from, *bits)) {
                Ok(bits) => Some(Ok(TExprKind::Float(bits))),
                Err(msg) => Some(Err(msg)),
            }
        }
        TExprKind::Float(bits) if to.is_int() => match float_to_int(to, from, *bits) {
            Ok(value) => Some(Ok(TExprKind::Int(value))),
            Err(msg) => Some(Err(msg)),
        },
        _ => None,
    }
}

fn int_to_float_bits(from: Ty, to: Ty, value: i64) -> u64 {
    if to == Ty::F32 {
        let narrow = if from.is_signed() {
            value as f32
        } else {
            value as u64 as f32
        };
        u64::from(narrow.to_bits())
    } else {
        let wide = if from.is_signed() {
            value as f64
        } else {
            value as u64 as f64
        };
        wide.to_bits()
    }
}

fn float_to_int(to: Ty, from: Ty, bits: u64) -> Result<i64, String> {
    let value = float_value(from, bits);
    if !value.is_finite() {
        return Err(format!("cannot cast {from} to {to}"));
    }
    let trunc = value.trunc();
    if trunc.abs() >= 9223372036854775808.0 && !(to == Ty::I64 && trunc == -9223372036854775808.0) {
        return Err(format!("float value does not fit in {to}"));
    }
    let value = trunc as i64;
    if !to.in_range(value) {
        return Err(format!("{value} does not fit in {to}"));
    }
    Ok(value)
}

fn escaped_ref(ty: &ParsedTy) -> bool {
    match ty {
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

fn applied_name(name: &str, args: &[Ty], checker: &Checker) -> String {
    if args.is_empty() {
        return name.to_string();
    }
    let parts: Vec<String> = args.iter().map(|ty| checker.show(*ty)).collect();
    format!("{name}<{}>", parts.join(", "))
}

fn field_irrefutable(pat: &TPat) -> bool {
    matches!(pat, TPat::Wild | TPat::Bind(_))
}

fn const_less(ty: Ty, a: i64, b: i64) -> bool {
    if ty.is_signed() {
        a < b
    } else {
        (ty.bits(a) as u64) < (ty.bits(b) as u64)
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

impl<'a> Checker<'a> {
    fn vec_elem_of(&self, expr: &Expr) -> Option<Ty> {
        let ExprKind::Name(name) = &expr.kind else {
            return None;
        };
        let ty = if let Some(id) = self.lookup(name) {
            self.bindings[id as usize].ty
        } else {
            let id = self
                .outer_scopes
                .iter()
                .rev()
                .find_map(|scope| scope.get(name).copied())?;
            self.outer_bindings[id as usize].ty
        };
        self.element_of(ty).or_else(|| {
            let Ty::Ref(id) = ty else {
                return None;
            };
            self.element_of(self.refs[id as usize].inner)
        })
    }

    fn peek(&self, expr: &Expr) -> Option<Ty> {
        match &expr.kind {
            ExprKind::Int(_) | ExprKind::Float(_) => None,
            ExprKind::Bool(_) => Some(Ty::Bool),
            ExprKind::Name(name) => self
                .lookup(name)
                .map(|id| self.bindings[id as usize].ty)
                .or_else(|| {
                    let id = self
                        .outer_scopes
                        .iter()
                        .rev()
                        .find_map(|scope| scope.get(name).copied())?;
                    Some(self.outer_bindings[id as usize].ty)
                })
                .or_else(|| self.consts.get(name).map(|(ty, _)| *ty)),
            ExprKind::Borrow { .. }
            | ExprKind::Try(_)
            | ExprKind::Assoc { .. }
            | ExprKind::Slice { .. }
            | ExprKind::Deref(_) => None,
            ExprKind::Unary {
                op: UnaryOp::Not, ..
            } => Some(Ty::Bool),
            ExprKind::Unary {
                op: UnaryOp::Neg | UnaryOp::BitNot,
                expr,
            } => self.peek(expr),
            ExprKind::Binary { op, .. } if op.is_compare() || op.is_logic() => Some(Ty::Bool),
            ExprKind::Binary { lhs, rhs, .. } => self.peek(lhs).or_else(|| self.peek(rhs)),
            ExprKind::Str(_) => Some(Ty::String),
            ExprKind::Char(_) => Some(Ty::Char),
            ExprKind::CStr(_) | ExprKind::Lambda { .. } => None,
            ExprKind::Call { callee, .. }
                if matches!(callee.as_str(), "from_char" | "from_byte") =>
            {
                Some(Ty::String)
            }
            ExprKind::Call { callee, .. } if is_builtin(callee) => Some(Ty::Unit),
            ExprKind::Call { callee, .. } => self.sigs.get(callee).and_then(|sig| sig.ret),
            ExprKind::Method {
                receiver, method, ..
            } => {
                if method == "len" && self.vec_elem_of(receiver).is_some() {
                    Some(Ty::Usize)
                } else if method == "is_empty" && self.vec_elem_of(receiver).is_some() {
                    Some(Ty::Bool)
                } else if (method == "push" || method == "clear")
                    && self.vec_elem_of(receiver).is_some()
                {
                    Some(Ty::Unit)
                } else {
                    self.sigs.get(method).and_then(|sig| sig.ret)
                }
            }
            ExprKind::Index { base, .. } => self.vec_elem_of(base),
            ExprKind::Array(_) => None,
            ExprKind::If {
                else_body: None, ..
            }
            | ExprKind::While { .. }
            | ExprKind::For { .. } => Some(Ty::Unit),
            ExprKind::Tuple(_)
            | ExprKind::StructLit { .. }
            | ExprKind::Field { .. }
            | ExprKind::Range { .. } => None,
            ExprKind::Match { arms, .. } => arms.first().and_then(|arm| self.peek(&arm.body)),
            ExprKind::If {
                then_body,
                else_body: Some(else_body),
                ..
            } => self.peek_block(then_body).or_else(|| self.peek(else_body)),
            ExprKind::Block(block) | ExprKind::Unsafe(block) => self.peek_block(block),
            ExprKind::Cast { ty, .. } => match ty {
                ParsedTy::Named { name, args, .. } if args.is_empty() => Ty::parse_name(name),
                _ => None,
            },
        }
    }

    fn peek_block(&self, block: &Block) -> Option<Ty> {
        match &block.tail {
            Some(expr) => self.peek(expr),
            None => Some(Ty::Unit),
        }
    }
}

fn shareable(ty: Ty) -> bool {
    ty.is_int() || ty.is_float() || ty == Ty::Bool || ty == Ty::Char
}

fn bit_width(ty: Ty) -> u32 {
    match ty {
        Ty::I8 | Ty::U8 => 8,
        Ty::I16 | Ty::U16 => 16,
        Ty::I32 | Ty::U32 | Ty::Usize => 32,
        _ => 64,
    }
}

fn fold_bitnot(ty: Ty, value: i64) -> i64 {
    let width = bit_width(ty);
    let mask = if width == 64 {
        u64::MAX
    } else {
        (1u64 << width) - 1
    };
    ty.bits((!(value as u64) & mask) as i64)
}

fn fold_bitwise(ty: Ty, op: BinOp, left: i64, right: i64) -> i64 {
    let width = bit_width(ty);
    let mask = if width == 64 {
        u64::MAX
    } else {
        (1u64 << width) - 1
    };
    let a = (left as u64) & mask;
    let b = (right as u64) & mask;
    let count = (b as u32) & (width - 1);
    let bits = match op {
        BinOp::BitAnd => a & b,
        BinOp::BitOr => a | b,
        BinOp::BitXor => a ^ b,
        BinOp::Shl => a.wrapping_shl(count) & mask,
        BinOp::Shr if ty.is_signed() => {
            let shifted = match width {
                8 => ((a as u8) as i8 >> count) as u64,
                16 => ((a as u16) as i16 >> count) as u64,
                32 => ((a as u32) as i32 >> count) as u64,
                _ => ((a as i64) >> count) as u64,
            };
            shifted & mask
        }
        BinOp::Shr => a >> count,
        _ => 0,
    };
    ty.bits(bits as i64)
}

/// Spans of `let mut` bindings that a nested closure assigns.
fn shared_assign_spans(body: &Block) -> HashSet<usize> {
    let mut found = HashSet::new();
    let mut scopes = vec![HashMap::new()];
    walk_block(body, &mut scopes, 0, &mut found);
    found
}

fn walk_block(
    block: &Block,
    scopes: &mut Vec<HashMap<String, (usize, bool, u32)>>,
    depth: u32,
    found: &mut HashSet<usize>,
) {
    scopes.push(HashMap::new());
    for stmt in &block.stmts {
        walk_stmt(stmt, scopes, depth, found);
    }
    if let Some(tail) = &block.tail {
        walk_expr(tail, scopes, depth, found);
    }
    scopes.pop();
}

fn walk_stmt(
    stmt: &Stmt,
    scopes: &mut Vec<HashMap<String, (usize, bool, u32)>>,
    depth: u32,
    found: &mut HashSet<usize>,
) {
    match stmt {
        Stmt::Let {
            pattern,
            mutable,
            init,
            ..
        } => {
            walk_expr(init, scopes, depth, found);
            bind_pat(pattern, *mutable, depth, scopes);
        }
        Stmt::Assign {
            name,
            name_span: _,
            value,
        } => {
            note_assign(scopes, name, depth, found);
            walk_expr(value, scopes, depth, found);
        }
        Stmt::IndexAssign {
            name, index, value, ..
        } => {
            note_assign(scopes, name, depth, found);
            walk_expr(index, scopes, depth, found);
            walk_expr(value, scopes, depth, found);
        }
        Stmt::FieldAssign { name, value, .. } => {
            note_assign(scopes, name, depth, found);
            walk_expr(value, scopes, depth, found);
        }
        Stmt::DerefAssign { ptr, value } => {
            walk_expr(ptr, scopes, depth, found);
            walk_expr(value, scopes, depth, found);
        }
        Stmt::Expr(expr) => walk_expr(expr, scopes, depth, found),
        Stmt::Return {
            value: Some(expr), ..
        } => walk_expr(expr, scopes, depth, found),
        Stmt::Return { value: None, .. } | Stmt::Break { .. } | Stmt::Continue { .. } => {}
    }
}

fn note_assign(
    scopes: &[HashMap<String, (usize, bool, u32)>],
    name: &str,
    depth: u32,
    found: &mut HashSet<usize>,
) {
    let Some((span, mutable, bind_depth)) = scopes
        .iter()
        .rev()
        .find_map(|scope| scope.get(name).copied())
    else {
        return;
    };
    if mutable && bind_depth < depth {
        found.insert(span);
    }
}

fn bind_pat(
    pat: &Pat,
    mutable: bool,
    depth: u32,
    scopes: &mut Vec<HashMap<String, (usize, bool, u32)>>,
) {
    match pat {
        Pat::Name { name, span } if name != "_" => {
            scopes
                .last_mut()
                .unwrap()
                .insert(name.clone(), (span.start, mutable, depth));
        }
        Pat::Ctor { fields, .. } => {
            for field in fields {
                bind_pat(field, mutable, depth, scopes);
            }
        }
        Pat::Tuple { fields, .. } => {
            for field in fields {
                bind_pat(field, mutable, depth, scopes);
            }
        }
        Pat::Struct { fields, .. } => {
            for (_, _, field) in fields {
                bind_pat(field, mutable, depth, scopes);
            }
        }
        Pat::Name { .. } | Pat::Int { .. } | Pat::Bool { .. } | Pat::Char { .. } => {}
    }
}

fn walk_expr(
    expr: &Expr,
    scopes: &mut Vec<HashMap<String, (usize, bool, u32)>>,
    depth: u32,
    found: &mut HashSet<usize>,
) {
    match &expr.kind {
        ExprKind::Int(_)
        | ExprKind::Float(_)
        | ExprKind::Bool(_)
        | ExprKind::Str(_)
        | ExprKind::CStr(_)
        | ExprKind::Char(_)
        | ExprKind::Name(_) => {}
        ExprKind::Unary { expr, .. }
        | ExprKind::Try(expr)
        | ExprKind::Deref(expr)
        | ExprKind::Borrow { expr, .. } => {
            walk_expr(expr, scopes, depth, found);
        }
        ExprKind::Binary { lhs, rhs, .. }
        | ExprKind::Range {
            start: lhs,
            end: rhs,
            ..
        } => {
            walk_expr(lhs, scopes, depth, found);
            walk_expr(rhs, scopes, depth, found);
        }
        ExprKind::Call { args, .. }
        | ExprKind::Assoc { args, .. }
        | ExprKind::Tuple(args)
        | ExprKind::Array(args) => {
            for arg in args {
                walk_expr(arg, scopes, depth, found);
            }
        }
        ExprKind::Method { receiver, args, .. } => {
            walk_expr(receiver, scopes, depth, found);
            for arg in args {
                walk_expr(arg, scopes, depth, found);
            }
        }
        ExprKind::If {
            cond,
            then_body,
            else_body,
        } => {
            walk_expr(cond, scopes, depth, found);
            walk_block(then_body, scopes, depth, found);
            if let Some(else_body) = else_body {
                walk_expr(else_body, scopes, depth, found);
            }
        }
        ExprKind::Block(block) | ExprKind::Unsafe(block) => walk_block(block, scopes, depth, found),
        ExprKind::Match { scrutinee, arms } => {
            walk_expr(scrutinee, scopes, depth, found);
            for arm in arms {
                scopes.push(HashMap::new());
                bind_pat(&arm.pat, false, depth, scopes);
                if let Some(guard) = &arm.guard {
                    walk_expr(guard, scopes, depth, found);
                }
                walk_expr(&arm.body, scopes, depth, found);
                scopes.pop();
            }
        }
        ExprKind::While { cond, body } => {
            walk_expr(cond, scopes, depth, found);
            walk_block(body, scopes, depth, found);
        }
        ExprKind::For { pat, iter, body } => {
            walk_expr(iter, scopes, depth, found);
            scopes.push(HashMap::new());
            bind_pat(pat, false, depth, scopes);
            walk_block(body, scopes, depth, found);
            scopes.pop();
        }
        ExprKind::StructLit { fields, .. } => {
            for (_, field) in fields {
                walk_expr(field, scopes, depth, found);
            }
        }
        ExprKind::Field { base, .. } => walk_expr(base, scopes, depth, found),
        ExprKind::Cast { expr, .. } => walk_expr(expr, scopes, depth, found),
        ExprKind::Lambda { params, body } => {
            scopes.push(HashMap::new());
            for param in params {
                bind_pat(&param.pat, false, depth + 1, scopes);
            }
            walk_expr(body, scopes, depth + 1, found);
            scopes.pop();
        }
        ExprKind::Index { base, index } => {
            walk_expr(base, scopes, depth, found);
            walk_expr(index, scopes, depth, found);
        }
        ExprKind::Slice {
            base, start, end, ..
        } => {
            walk_expr(base, scopes, depth, found);
            walk_expr(start, scopes, depth, found);
            walk_expr(end, scopes, depth, found);
        }
    }
}
