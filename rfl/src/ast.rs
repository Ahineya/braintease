#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Span {
    pub start: usize,
}

#[derive(Debug, Clone)]
pub struct Module {
    pub adts: Vec<Adt>,
    pub functions: Vec<Function>,
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
}

#[derive(Debug, Clone)]
pub enum ParsedTy {
    Named { span: Span, name: String, args: Vec<ParsedTy> },
    Func { span: Span, params: Vec<ParsedTy>, ret: Box<ParsedTy> },
}

impl ParsedTy {
    pub fn span(&self) -> Span {
        match self {
            ParsedTy::Named { span, .. } | ParsedTy::Func { span, .. } => *span,
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
    Let {
        name: String,
        name_span: Span,
        mutable: bool,
        ty: Option<ParsedTy>,
        init: Expr,
    },
    Assign {
        name: String,
        name_span: Span,
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
    Cast {
        expr: Box<Expr>,
        ty: ParsedTy,
    },
    Lambda {
        params: Vec<Param>,
        body: Box<Expr>,
    },
}

impl Expr {
    /// True when a following statement may omit a semicolon, as in `if (c) { ... }`.
    pub fn ends_with_block(&self) -> bool {
        match &self.kind {
            ExprKind::If { else_body, .. } => else_body.as_ref().map(|e| e.ends_with_block()).unwrap_or(true),
            ExprKind::Block(_) | ExprKind::While { .. } | ExprKind::Match { .. } => true,
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
    pub body: Expr,
}

#[derive(Debug, Clone)]
pub enum Pat {
    Name { name: String, span: Span },
    Ctor { name: String, span: Span, fields: Vec<Pat> },
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
