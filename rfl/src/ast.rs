#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Span {
    pub start: usize,
}

#[derive(Debug, Clone)]
pub struct Module {
    pub adts: Vec<Adt>,
    pub structs: Vec<StructItem>,
    pub impls: Vec<Impl>,
    pub consts: Vec<Const>,
    pub functions: Vec<Function>,
}

#[derive(Debug, Clone)]
pub struct StructItem {
    pub name: String,
    pub name_span: Span,
    pub fields: Vec<StructField>,
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
    pub span: Span,
    pub type_params: Vec<TypeParam>,
    pub ty: ParsedTy,
    pub items: Vec<ImplItem>,
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
}

#[derive(Debug, Clone)]
pub struct Ctor {
    pub name: String,
    pub name_span: Span,
    pub fields: Vec<ParsedTy>,
}

#[derive(Debug, Clone)]
pub struct TypeParam {
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
    Named { span: Span, name: String, args: Vec<ParsedTy> },
    Func { span: Span, params: Vec<ParsedTy>, ret: Box<ParsedTy> },
    /// `&T` or `&mut T`.
    Ref { span: Span, mutable: bool, inner: Box<ParsedTy> },
    /// `[T; N]`. `len` is a constant expression.
    Array { span: Span, elem: Box<ParsedTy>, len: Box<Expr> },
    /// `(T, U, ...)`.
    Tuple { span: Span, fields: Vec<ParsedTy> },
}

impl ParsedTy {
    pub fn span(&self) -> Span {
        match self {
            ParsedTy::Named { span, .. }
            | ParsedTy::Func { span, .. }
            | ParsedTy::Ref { span, .. }
            | ParsedTy::Array { span, .. }
            | ParsedTy::Tuple { span, .. } => *span,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    pub tail: Option<Expr>,
}

#[derive(Debug, Clone)]
pub enum Stmt {
    /// `let [mut] pattern [: type] = expr`. The pattern must be irrefutable.
    Let {
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
    /// `name[index] = value` on a mutable vector.
    IndexAssign {
        name: String,
        name_span: Span,
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
    Bool(bool),
    /// A string literal. It is only valid as an argument of `print` or `println`.
    Str(Vec<u16>),
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
        /// Explicit `name<i16>(...)` arguments. Empty means infer them.
        type_args: Vec<ParsedTy>,
        args: Vec<Expr>,
    },
    /// `receiver.method(args)`. Resolved as an ordinary call with the receiver first.
    Method {
        receiver: Box<Expr>,
        method: String,
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
        expr: Box<Expr>,
    },
    /// `Type::function(args)`.
    Assoc {
        ty_name: String,
        method: String,
        args: Vec<Expr>,
    },
    /// `expr?` on the prelude `Option` or `Result`.
    Try(Box<Expr>),
}

impl Expr {
    /// True when a following statement may omit a semicolon, as in `if (c) { ... }`.
    pub fn ends_with_block(&self) -> bool {
        match &self.kind {
            ExprKind::If { else_body, .. } => else_body.as_ref().map(|e| e.ends_with_block()).unwrap_or(true),
            ExprKind::Block(_) | ExprKind::While { .. } | ExprKind::Match { .. } | ExprKind::For { .. } => true,
            ExprKind::Lambda { body, .. } => body.ends_with_block(),
            _ => false,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnaryOp {
    Neg,
    Not,
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
    Name { name: String, span: Span },
    Ctor { name: String, span: Span, fields: Vec<Pat> },
    Int { value: i64, span: Span },
    Bool { value: bool, span: Span },
    Tuple { span: Span, fields: Vec<Pat> },
    /// `Point { x, y: pat }`. A missing pattern means the field name is the binding.
    Struct { name: String, span: Span, fields: Vec<(String, Span, Pat)> },
}

impl Pat {
    pub fn span(&self) -> Span {
        match self {
            Pat::Name { span, .. }
            | Pat::Ctor { span, .. }
            | Pat::Int { span, .. }
            | Pat::Bool { span, .. }
            | Pat::Tuple { span, .. }
            | Pat::Struct { span, .. } => *span,
        }
    }
}

impl BinOp {
    pub fn is_compare(self) -> bool {
        matches!(self, Self::Eq | Self::Ne | Self::Lt | Self::Le | Self::Gt | Self::Ge)
    }

    pub fn is_logic(self) -> bool {
        matches!(self, Self::And | Self::Or)
    }

    pub fn is_arith(self) -> bool {
        matches!(self, Self::Add | Self::Sub | Self::Mul | Self::Div | Self::Rem)
    }
}
