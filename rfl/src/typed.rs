use crate::ast::{BinOp, UnaryOp};
use crate::ty::Ty;

#[derive(Debug)]
pub struct TModule {
    pub functions: Vec<TFn>,
}

#[derive(Debug, Clone)]
pub struct AdtDef {
    pub name: String,
    /// Schema this monomorph was built from.
    pub schema: usize,
    /// Concrete arguments of a generic schema. Empty for an ordinary type.
    pub args: Vec<Ty>,
    pub ctors: Vec<CtorDef>,
}

#[derive(Debug, Clone)]
pub struct CtorDef {
    pub fields: Vec<Ty>,
}

#[derive(Debug)]
pub struct TFn {
    pub name: String,
    /// Binding indexes of the parameters, in order.
    pub params: Vec<u32>,
    pub ret: Ty,
    /// Every `let` and parameter, in the order bindings were created.
    pub bindings: Vec<Ty>,
    pub body: TBlock,
    /// The frame reserves two cells after the parameters for an environment pointer.
    /// Every function has the slot, so a named function and a lambda use one call protocol.
    pub closure: bool,
    /// Values copied into the environment when the closure is created.
    pub captures: Vec<Capture>,
}

#[derive(Debug, Clone, Copy)]
pub struct Capture {
    /// Binding in the function that creates the closure.
    pub parent: u32,
    /// Immutable binding in this function that receives the copy.
    pub local: u32,
    pub ty: Ty,
}

#[derive(Debug)]
pub struct TBlock {
    pub stmts: Vec<TStmt>,
    pub tail: Option<TExpr>,
}

#[derive(Debug)]
pub enum TStmt {
    Let { binding: u32, init: TExpr },
    Assign { binding: u32, value: TExpr },
    Expr(TExpr),
    Return(Option<TExpr>),
    Break,
    Continue,
}

#[derive(Debug)]
pub struct TExpr {
    pub ty: Ty,
    pub kind: TExprKind,
}

#[derive(Debug)]
pub enum TExprKind {
    Int(i64),
    Bool(bool),
    Name(u32),
    Unary { op: UnaryOp, expr: Box<TExpr> },
    Binary { op: BinOp, lhs: Box<TExpr>, rhs: Box<TExpr> },
    Call { callee: String, args: Vec<TExpr> },
    /// Call a function value. `params` is the callee's parameter list.
    Apply { callee: Box<TExpr>, args: Vec<TExpr>, params: Vec<Ty> },
    /// A lambda lifted to `func`. The value is a closure with an empty environment.
    Closure { func: String },
    If { cond: Box<TExpr>, then_body: Box<TBlock>, else_body: Option<Box<TExpr>> },
    Block(Box<TBlock>),
    While { cond: Box<TExpr>, body: Box<TBlock> },
    Cast { expr: Box<TExpr>, to: Ty },
    Ctor { tag: u16, args: Vec<TExpr> },
    Match { scrutinee: Box<TExpr>, arms: Vec<TArm> },
    /// Decimal integer, or `true`/`false`. `newline` adds a trailing line feed.
    Print { value: Box<TExpr>, newline: bool },
    /// Text written with `putchar`. Same newline rule as `Print`.
    PrintLit { chars: Vec<u16>, newline: bool },
    /// One output cell.
    Putc(Box<TExpr>),
}

#[derive(Debug)]
pub struct TArm {
    pub pat: TPat,
    pub body: TBlock,
}

#[derive(Debug)]
pub enum TPat {
    Wild,
    Bind(u32),
    Ctor { tag: u16, fields: Vec<FieldPat> },
}

#[derive(Debug)]
pub struct FieldPat {
    pub ty: Ty,
    pub pat: Box<TPat>,
}
