/// A half-open byte range `[start, end)` in the source.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Span {
    pub start: usize,
    pub end: usize,
}

impl Span {
    pub fn new(start: usize, end: usize) -> Self {
        Self { start, end }
    }

    pub fn empty() -> Self {
        Self { start: 0, end: 0 }
    }

    /// The span from the start of `self` through the end of `other`.
    pub fn to(self, other: Span) -> Span {
        Span {
            start: self.start,
            end: other.end.max(self.end),
        }
    }

    pub fn contains(self, offset: usize) -> bool {
        offset >= self.start && offset < self.end
    }
}

#[derive(Debug, Clone)]
pub struct Module {
    pub adts: Vec<Adt>,
    pub structs: Vec<StructItem>,
    pub impls: Vec<Impl>,
    pub traits: Vec<Trait>,
    /// Lexical visibility and declaration ownership retained after module flattening.
    pub trait_scopes: Vec<TraitScope>,
    pub consts: Vec<Const>,
    pub functions: Vec<Function>,
    /// `extern "C" { fn ...; }` declarations. The bodies live in C.
    pub externs: Vec<ExternDecl>,
    pub imports: Vec<Import>,
}

#[derive(Debug, Clone)]
pub struct Import {
    pub span: Span,
    pub names: Vec<ImportItem>,
    pub path: String,
    pub path_span: Span,
}

#[derive(Debug, Clone)]
pub struct ImportItem {
    pub name: String,
    /// Name used in this file. Equal to `name` when there is no `as`.
    pub alias: String,
    pub span: Span,
    pub alias_span: Span,
}

#[derive(Debug, Clone)]
pub struct StructItem {
    pub name: String,
    pub name_span: Span,
    pub fields: Vec<StructField>,
    /// `#[repr(C)]`. Fields are the C layout, with no padding.
    pub repr_c: bool,
    pub exported: bool,
}

#[derive(Debug, Clone)]
pub struct StructField {
    pub name: String,
    pub name_span: Span,
    pub ty: ParsedTy,
}

/// A named value computed while compiling.
#[derive(Debug, Clone)]
pub struct Const {
    pub name: String,
    pub name_span: Span,
    pub ty: ParsedTy,
    pub init: Expr,
    pub exported: bool,
}

/// `self`, `&self`, or `&mut self`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Recv {
    Value,
    Shared,
    Mut,
}

#[derive(Debug, Clone)]
pub struct Impl {
    pub trait_name: Option<(String, Span)>,
    pub span: Span,
    pub type_params: Vec<TypeParam>,
    pub ty: ParsedTy,
    pub items: Vec<ImplItem>,
    pub associated_types: Vec<AssociatedType>,
}

#[derive(Debug, Clone)]
pub struct ImplItem {
    pub name: String,
    pub name_span: Span,
    pub type_params: Vec<TypeParam>,
    pub receiver: Option<Recv>,
    pub params: Vec<Param>,
    pub ret: Option<ParsedTy>,
    pub body: Block,
}

#[derive(Debug, Clone)]
pub struct Adt {
    pub name: String,
    pub name_span: Span,
    pub params: Vec<TypeParam>,
    pub ctors: Vec<Ctor>,
    pub exported: bool,
}

#[derive(Debug, Clone)]
pub struct Ctor {
    pub name: String,
    pub name_span: Span,
    pub fields: Vec<ParsedTy>,
}

#[derive(Debug, Clone)]
pub struct TypeParam {
    pub bounds: Vec<(String, Span)>,
    pub name: String,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct Function {
    pub name: String,
    pub name_span: Span,
    pub type_params: Vec<TypeParam>,
    pub params: Vec<Param>,
    pub ret: Option<ParsedTy>,
    pub body: Block,
    /// `export extern "C"`. A C wrapper calls this function.
    pub c_export: bool,
    pub exported: bool,
}

/// A C function with no RFL body.
#[derive(Debug, Clone)]
pub struct ExternDecl {
    pub name: String,
    pub name_span: Span,
    pub params: Vec<Param>,
    pub ret: Option<ParsedTy>,
    /// `fn name(named, ...)`. Extra arguments are passed on the stack.
    pub variadic: bool,
}

#[derive(Debug, Clone)]
pub struct Param {
    pub name: String,
    pub name_span: Span,
    pub ty: Option<ParsedTy>,
    /// `Point { x, y }` in a parameter list. A plain name is `Pat::Name`.
    pub pat: Pat,
}

#[derive(Debug, Clone)]
pub enum ParsedTy {
    /// `T::Output` or `<T as Trait>::Output`; normalized before IR lowering.
    Projection {
        span: Span,
        base: Box<ParsedTy>,
        trait_name: Option<(String, Span)>,
        name: String,
        name_span: Span,
    },
    Named {
        span: Span,
        name: String,
        args: Vec<ParsedTy>,
    },
    Func {
        span: Span,
        params: Vec<ParsedTy>,
        ret: Box<ParsedTy>,
    },
    /// `&T` or `&mut T`.
    Ref {
        span: Span,
        mutable: bool,
        inner: Box<ParsedTy>,
    },
    /// `[T; N]`. `len` is a constant expression.
    Array {
        span: Span,
        elem: Box<ParsedTy>,
        len: Box<Expr>,
    },
    /// `*const T` or `*mut T`. Two cells, not refcounted.
    Ptr {
        span: Span,
        mutable: bool,
        inner: Box<ParsedTy>,
    },
    /// `(T, U, ...)`.
    Tuple { span: Span, fields: Vec<ParsedTy> },
    /// `extern "C" fn(...) -> ...`. A code address, with no environment.
    CFn {
        span: Span,
        params: Vec<ParsedTy>,
        ret: Box<ParsedTy>,
    },
}

impl ParsedTy {
    pub fn span(&self) -> Span {
        match self {
            ParsedTy::Projection { span, .. }
            | ParsedTy::Named { span, .. }
            | ParsedTy::Func { span, .. }
            | ParsedTy::Ref { span, .. }
            | ParsedTy::Ptr { span, .. }
            | ParsedTy::Array { span, .. }
            | ParsedTy::Tuple { span, .. }
            | ParsedTy::CFn { span, .. } => *span,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Block {
    /// From the opening `{` through the closing `}`.
    pub span: Span,
    pub stmts: Vec<Stmt>,
    pub tail: Option<Expr>,
}

#[derive(Debug, Clone)]
pub enum Stmt {
    /// `let [mut] pattern [: type] = expr`. The pattern must be irrefutable.
    Let {
        /// From `let` through the terminating semicolon.
        span: Span,
        pattern: Pat,
        mutable: bool,
        ty: Option<ParsedTy>,
        init: Expr,
    },
    Assign {
        name: String,
        name_span: Span,
        value: Expr,
    },
    /// `*ptr = value` or `*ptr` is handled as an expression. This stores through a raw pointer.
    DerefAssign {
        ptr: Expr,
        value: Expr,
    },
    /// `name[index] = value`, or `name.field[index] = value`, on a mutable vector or array.
    IndexAssign {
        name: String,
        name_span: Span,
        /// Empty for `tape[i]`. `tape` then `cell` for `m.tape.cell[i]`.
        fields: Vec<String>,
        index: Expr,
        value: Expr,
    },
    /// `name.field = value` on a mutable struct.
    FieldAssign {
        name: String,
        name_span: Span,
        fields: Vec<String>,
        value: Expr,
    },
    Expr(Expr),
    Return {
        span: Span,
        value: Option<Expr>,
    },
    Break {
        span: Span,
    },
    Continue {
        span: Span,
    },
}

#[derive(Debug, Clone)]
pub struct Expr {
    pub span: Span,
    pub kind: ExprKind,
}

#[derive(Debug, Clone)]
pub enum ExprKind {
    Int(i64),
    /// Decimal spelling. The checker chooses `f32` or `f64`.
    Float(String),
    Bool(bool),
    /// A string literal. It is only valid as an argument of `print` or `println`.
    Str(Vec<u16>),
    /// `c"..."`. A `*const c_char` pointing at static cells plus a trailing zero.
    CStr(Vec<u16>),
    /// A Unicode scalar.
    Char(u32),
    Name(String),
    Unary {
        op: UnaryOp,
        expr: Box<Expr>,
    },
    Binary {
        op: BinOp,
        lhs: Box<Expr>,
        rhs: Box<Expr>,
    },
    Call {
        callee: String,
        /// The callee name as it appears in the source, not the argument list.
        callee_span: Span,
        /// Explicit `name<i16>(...)` arguments. Empty means infer them.
        type_args: Vec<ParsedTy>,
        args: Vec<Expr>,
    },
    /// `receiver.method(args)`. Resolved as an ordinary call with the receiver first.
    Method {
        receiver: Box<Expr>,
        /// Name as written. Builtin and inherent methods use this.
        method: String,
        /// The method name token, not the receiver or the argument list.
        method_span: Span,
        /// Free function to call when no inherent method matches.
        resolved: String,
        args: Vec<Expr>,
    },
    If {
        cond: Box<Expr>,
        then_body: Box<Block>,
        else_body: Option<Box<Expr>>,
    },
    Block(Box<Block>),
    Match {
        scrutinee: Box<Expr>,
        arms: Vec<Arm>,
    },
    While {
        cond: Box<Expr>,
        body: Box<Block>,
    },
    /// `for (pat in iter) body`.
    For {
        pat: Pat,
        iter: Box<Expr>,
        body: Box<Block>,
    },
    /// `(a, b)`.
    Tuple(Vec<Expr>),
    /// `Name { field: expr, ... }`.
    StructLit {
        name: String,
        fields: Vec<(String, Expr)>,
    },
    /// `base.field`.
    Field {
        base: Box<Expr>,
        name: String,
        name_span: Span,
    },
    /// `start..end` or `start..=end`. Legal as the iterator of `for`.
    Range {
        start: Box<Expr>,
        end: Box<Expr>,
        inclusive: bool,
    },
    Cast {
        expr: Box<Expr>,
        ty: ParsedTy,
    },
    Lambda {
        params: Vec<Param>,
        body: Box<Expr>,
    },
    Array(Vec<Expr>),
    Index {
        base: Box<Expr>,
        index: Box<Expr>,
    },
    /// `base[start..end]` or `base[start..=end]`. The result is an owning slice.
    Slice {
        base: Box<Expr>,
        start: Box<Expr>,
        end: Box<Expr>,
        inclusive: bool,
    },
    /// `&expr` or `&mut expr`.
    Borrow {
        mutable: bool,
        /// Compiler-inserted receiver borrowing may keep an rvalue snapshot.
        /// Explicit source borrows still require a supported place.
        temporary: bool,
        expr: Box<Expr>,
    },
    /// `*expr`. The operand is a raw pointer. Legal only in `unsafe`.
    Deref(Box<Expr>),
    /// `Type::function(args)`.
    Assoc {
        ty_name: String,
        method: String,
        args: Vec<Expr>,
    },
    /// `expr?` on the prelude `Option` or `Result`.
    Try(Box<Expr>),
    /// `unsafe { ... }`. The block is ordinary; the flag is for the checker.
    Unsafe(Box<Block>),
}

impl Expr {
    /// True when a following statement may omit a semicolon, as in `if (c) { ... }`.
    pub fn ends_with_block(&self) -> bool {
        match &self.kind {
            ExprKind::If { else_body, .. } => else_body
                .as_ref()
                .map(|e| e.ends_with_block())
                .unwrap_or(true),
            ExprKind::Block(_)
            | ExprKind::Unsafe(_)
            | ExprKind::While { .. }
            | ExprKind::Match { .. }
            | ExprKind::For { .. } => true,
            ExprKind::Lambda { body, .. } => body.ends_with_block(),
            _ => false,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnaryOp {
    Neg,
    Not,
    /// `~`. Bitwise complement of an integer.
    BitNot,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
}

#[derive(Debug, Clone)]
pub struct Arm {
    pub pat: Pat,
    /// Evaluated after the pattern matches. A false guard tries the next arm.
    pub guard: Option<Expr>,
    pub body: Expr,
}

#[derive(Debug, Clone)]
pub enum Pat {
    Name {
        name: String,
        span: Span,
    },
    Ctor {
        name: String,
        span: Span,
        fields: Vec<Pat>,
    },
    Int {
        value: i64,
        span: Span,
    },
    Bool {
        value: bool,
        span: Span,
    },
    Char {
        scalar: u32,
        span: Span,
    },
    Tuple {
        span: Span,
        fields: Vec<Pat>,
    },
    /// `Point { x, y: pat }`. A missing pattern means the field name is the binding.
    Struct {
        name: String,
        span: Span,
        fields: Vec<(String, Span, Pat)>,
    },
}

impl Pat {
    pub fn span(&self) -> Span {
        match self {
            Pat::Name { span, .. }
            | Pat::Ctor { span, .. }
            | Pat::Int { span, .. }
            | Pat::Bool { span, .. }
            | Pat::Char { span, .. }
            | Pat::Tuple { span, .. }
            | Pat::Struct { span, .. } => *span,
        }
    }
}

impl BinOp {
    pub fn is_compare(self) -> bool {
        matches!(
            self,
            Self::Eq | Self::Ne | Self::Lt | Self::Le | Self::Gt | Self::Ge
        )
    }

    pub fn is_logic(self) -> bool {
        matches!(self, Self::And | Self::Or)
    }

    pub fn is_arith(self) -> bool {
        matches!(
            self,
            Self::Add | Self::Sub | Self::Mul | Self::Div | Self::Rem
        )
    }

    pub fn is_bitwise(self) -> bool {
        matches!(
            self,
            Self::BitAnd | Self::BitOr | Self::BitXor | Self::Shl | Self::Shr
        )
    }
}

/// A statically dispatched interface. Signatures deliberately have no bodies.
#[derive(Debug, Clone)]
pub struct Trait {
    pub span: Span,
    pub name: String,
    pub name_span: Span,
    pub exported: bool,
    pub methods: Vec<ImplItem>,
    pub associated_types: Vec<AssociatedType>,
}

/// A required associated type in a trait, or its definition in an impl.
#[derive(Debug, Clone)]
pub struct AssociatedType {
    pub name: String,
    pub name_span: Span,
    pub bounds: Vec<(String, Span)>,
    pub value: Option<ParsedTy>,
}

#[derive(Debug, Clone)]
pub struct TraitScope {
    pub span: Span,
    pub visible: Vec<String>,
    pub owned: Vec<String>,
    pub core: bool,
}
