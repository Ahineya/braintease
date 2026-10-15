use crate::ast::{BinOp, UnaryOp};
use crate::ty::Ty;

#[derive(Debug)]
pub struct TModule {
    pub functions: Vec<TFn>,
    /// Concrete algebraic types, indexed by [`Ty::Adt`](crate::ty::Ty::Adt).
    pub adts: Vec<AdtDef>,
    /// Field types of each struct, indexed by [`Ty::Struct`](crate::ty::Ty::Struct).
    pub structs: Vec<Vec<Ty>>,
    /// Parallel to `structs`. True when the struct is `#[repr(C)]`.
    pub repr_c: Vec<bool>,
    /// Field types of each tuple, indexed by [`Ty::Tuple`](crate::ty::Ty::Tuple).
    pub tuples: Vec<Vec<Ty>>,
    /// C functions this module calls. Their bodies are not here.
    pub externs: Vec<ExternFn>,
}

#[derive(Debug, Clone)]
pub struct ExternFn {
    pub name: String,
    pub params: Vec<Ty>,
    pub ret: Ty,
    /// Named parameters, then `...`.
    pub variadic: bool,
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
    /// Parallel to `bindings`. A shared binding's slot holds a box pointer.
    pub shared: Vec<bool>,
    /// Also emitted as a C function of the same name.
    pub c_export: bool,
}

#[derive(Debug, Clone, Copy)]
pub struct Capture {
    /// Binding in the function that creates the closure.
    pub parent: u32,
    /// Binding in this function that receives the capture.
    pub local: u32,
    pub ty: Ty,
    /// The outer binding and this one share a heap cell. Writes on either side
    /// are visible to the other.
    pub shared: bool,
}

#[derive(Debug)]
pub struct TBlock {
    pub stmts: Vec<TStmt>,
    pub tail: Option<TExpr>,
}

#[derive(Debug)]
pub enum TStmt {
    Let {
        binding: u32,
        init: TExpr,
    },
    /// Irrefutable constructor pattern. Bindings inside `pat` receive the fields.
    Destructure {
        pat: TPat,
        init: TExpr,
    },
    Assign {
        binding: u32,
        value: TExpr,
    },
    IndexAssign {
        binding: u32,
        /// Cell offset of the vector or array inside `binding`. Zero for a bare name.
        offset: i64,
        index: TExpr,
        value: TExpr,
        elem: Ty,
        /// The vector, array, or reference that `index` selects from.
        container: Ty,
    },
    /// Store `value` at a cell offset inside a struct binding.
    FieldAssign {
        binding: u32,
        offset: i64,
        value: TExpr,
    },
    /// Store through a raw pointer. `index` selects an element of `stride` cells.
    PtrStore {
        ptr: TExpr,
        index: Option<TExpr>,
        value: TExpr,
        stride: i64,
    },
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
    /// IEEE bits. `f32` uses the low 32 bits.
    Float(u64),
    Bool(bool),
    Name(u32),
    Unary {
        op: UnaryOp,
        expr: Box<TExpr>,
    },
    Binary {
        op: BinOp,
        lhs: Box<TExpr>,
        rhs: Box<TExpr>,
    },
    Call {
        callee: String,
        args: Vec<TExpr>,
    },
    /// A direct call to a C function. The callee is not an RFL frame.
    /// `fixed` is the number of named parameters. Later arguments are variadic.
    Extern {
        callee: String,
        args: Vec<TExpr>,
        fixed: usize,
    },
    /// Static `c"..."` bytes. The value is `*const c_char`.
    CStr(Vec<u16>),
    /// A `String`. Bytes are UTF-8, one octet per cell in the object.
    Str(Vec<u8>),
    /// `left + right` when both sides are strings. A new string.
    StrConcat {
        lhs: Box<TExpr>,
        rhs: Box<TExpr>,
    },
    /// `s[start..end]`. The bounds are byte offsets. The result is a new string.
    StrSlice {
        base: Box<TExpr>,
        start: Box<TExpr>,
        end: Box<TExpr>,
        inclusive: bool,
    },
    /// One character, encoded as UTF-8.
    StrFromChar(Box<TExpr>),
    /// One octet, even when it is not a UTF-8 lead byte.
    StrFromByte(Box<TExpr>),
    /// A `char` scalar.
    Char(u32),
    /// Call a function value. `params` is the callee's parameter list.
    Apply {
        callee: Box<TExpr>,
        args: Vec<TExpr>,
        params: Vec<Ty>,
    },
    /// Address of a C function. The value is a code pointer, not an RFL closure.
    CFn {
        symbol: String,
    },
    /// Call a C function pointer.
    CApply {
        callee: Box<TExpr>,
        args: Vec<TExpr>,
    },
    /// A lambda lifted to `func`. The value is a closure with an empty environment.
    Closure {
        func: String,
    },
    If {
        cond: Box<TExpr>,
        then_body: Box<TBlock>,
        else_body: Option<Box<TExpr>>,
    },
    Block(Box<TBlock>),
    While {
        cond: Box<TExpr>,
        body: Box<TBlock>,
    },
    /// `for` over a range or a random-access collection. `step` is where `continue` lands.
    ForIter {
        index: u32,
        start: Box<TExpr>,
        end: Box<TExpr>,
        inclusive: bool,
        /// Runs at the start of each iteration, before `body`.
        extract: Option<Box<TStmt>>,
        body: Box<TBlock>,
    },
    /// Inline struct or tuple literal. Fields are stored in order.
    Record {
        fields: Vec<TExpr>,
    },
    /// A field of an inline product, `offset` cells from the start of `base`.
    Field {
        base: Box<TExpr>,
        offset: i64,
    },
    Cast {
        expr: Box<TExpr>,
        to: Ty,
    },
    Ctor {
        tag: u16,
        args: Vec<TExpr>,
    },
    Match {
        scrutinee: Box<TExpr>,
        arms: Vec<TArm>,
    },
    /// Decimal integer, or `true`/`false`. `newline` adds a trailing line feed.
    Print {
        value: Box<TExpr>,
        newline: bool,
    },
    /// Text written with `putchar`. Same newline rule as `Print`.
    PrintLit {
        chars: Vec<u16>,
        newline: bool,
    },
    /// One output cell.
    Putc(Box<TExpr>),
    /// A vector literal.
    VecLit {
        elems: Vec<TExpr>,
        elem: Ty,
    },
    /// `base[index]`, returning the element by value.
    Index {
        base: Box<TExpr>,
        index: Box<TExpr>,
        elem: Ty,
    },
    /// An inline `[T; N]` literal.
    ArrayLit {
        elems: Vec<TExpr>,
        elem: Ty,
    },
    /// `base[start..end]`, an owning slice of `elem`.
    Slice {
        base: Box<TExpr>,
        start: Box<TExpr>,
        end: Box<TExpr>,
        inclusive: bool,
        elem: Ty,
    },
    /// Copy an array or a slice into a fresh vector.
    ToVec {
        src: Box<TExpr>,
        elem: Ty,
    },
    /// `vec.len()`.
    Len {
        vec: Box<TExpr>,
    },
    /// `vec.is_empty()`.
    Empty {
        vec: Box<TExpr>,
    },
    /// `place.clear()` on a mutable vector place.
    Clear {
        binding: u32,
        offset: i64,
    },
    /// `place.push(value)` on a mutable vector place.
    Push {
        binding: u32,
        offset: i64,
        value: Box<TExpr>,
        elem: Ty,
    },
    /// `place.pop()`, copying the buffer first when it is shared.
    Pop {
        binding: u32,
        offset: i64,
        elem: Ty,
        some_tag: u16,
        none_tag: u16,
    },
    /// `vec.get(index)`. A missing element is `None`.
    Get {
        vec: Box<TExpr>,
        index: Box<TExpr>,
        elem: Ty,
        some_tag: u16,
        none_tag: u16,
    },
    /// `&binding` or `&mut binding`. The value points at that local.
    Borrow {
        binding: u32,
    },
    /// Load `stride` cells at `ptr`, plus `index` elements when present.
    Deref {
        ptr: Box<TExpr>,
        index: Option<Box<TExpr>>,
        stride: i64,
    },
    /// `ptr` moved by `index` elements of `stride` cells.
    PtrOffset {
        ptr: Box<TExpr>,
        index: Box<TExpr>,
        stride: i64,
    },
    /// Address cell of a raw pointer.
    PtrAddr {
        ptr: Box<TExpr>,
    },
    /// Bank cell of a raw pointer. Negative values are bank tags.
    PtrBank {
        ptr: Box<TExpr>,
    },
    /// The address cell is zero.
    PtrNull {
        ptr: Box<TExpr>,
    },
    /// `expr?`. `fail` is the error payload, or nothing when failure is nullary.
    Try {
        expr: Box<TExpr>,
        ok_tag: u16,
        fail_tag: u16,
        payload: Ty,
        fail: Option<Ty>,
    },
}

#[derive(Debug)]
pub struct TArm {
    pub pat: TPat,
    pub guard: Option<TExpr>,
    pub body: TBlock,
}

#[derive(Debug)]
pub enum TPat {
    Wild,
    Bind(u32),
    Ctor {
        tag: u16,
        fields: Vec<FieldPat>,
    },
    /// Integer or bool literal. Bool uses 1 and 0.
    Lit(i64),
    /// Inline product. `offset` is the cell offset of each field from the start of the value.
    Product {
        fields: Vec<ProductField>,
    },
}

#[derive(Debug)]
pub struct ProductField {
    pub offset: i64,
    pub ty: Ty,
    pub pat: Box<TPat>,
}

#[derive(Debug)]
pub struct FieldPat {
    pub ty: Ty,
    pub pat: Box<TPat>,
}
