/// RFL value types and checking-only constructor arguments.
/// Integer literals have already been given a width.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Ty {
    /// Checker-only type constructor identity. Never a runtime value or layout.
    Constructor(u32),
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    /// IEEE-754 binary32. Two cells, operated on by the soft-float helpers.
    F32,
    /// IEEE-754 binary64. Four cells, operated on by the soft-float helpers.
    F64,
    /// Two cells, the same width as `u32`. Indexes and lengths have to cover
    /// every heap object `malloc` can return, and that size is 32 bits.
    Usize,
    Bool,
    /// Unicode scalar. Two cells, the same width as `i32`.
    Char,
    Unit,
    /// Index into the module's algebraic types. Three cells: tag, address, bank.
    /// Nullary values keep a zero address. A payload is a refcounted heap object.
    Adt(u32),
    /// Index into the checker's function-type table. The value is four cells:
    /// function id, frame size, environment address, environment bank.
    Func(u32),
    /// Index into the module's element-type table. The value is a heap pointer:
    /// address cell, bank cell. Null is the empty vector.
    Vec(u32),
    /// Move-only owner of stable payload storage and a cleanup callback.
    Owned(u32),
    /// Move-only opaque owner; retains the original typed destruction glue.
    ErasedOwner,
    /// Copyable opaque owner; last reference runs the original typed cleanup.
    SharedOwner,
    /// Index into the checker's reference table. The value is a pointer to a
    /// local: address cell, bank cell. It cannot outlive that local.
    Ref(u32),
    /// Inline `[T; N]`. `id` finds the element type. `width` is that element's cells.
    Array {
        id: u32,
        len: u16,
        width: u16,
    },
    /// Index into the slice element table. Six cells: buffer address, bank,
    /// 32-bit first element, and 32-bit element count. The buffer is a vector allocation.
    Slice(u32),
    /// Inline struct. `id` indexes the struct table. `width` is the total cell count.
    Struct {
        id: u32,
        width: u16,
    },
    /// Inline tuple. `id` indexes the tuple table. `width` is the total cell count.
    Tuple {
        id: u32,
        width: u16,
    },
    /// Index into the checker's raw-pointer table. Two cells: address and bank.
    /// Not refcounted. Used to talk to C.
    Ptr(u32),
    /// Index into the checker's C function-pointer table. Two cells: the code
    /// address and its bank. No environment, and not an RFL callable.
    CFn(u32),
    /// Immutable UTF-8 text. Two cells: address and bank of a refcounted object.
    /// A null pointer is the empty string. A 32-bit length follows the refcount,
    /// then one octet per cell.
    String,
}

impl Ty {
    pub fn parse_name(name: &str) -> Option<Self> {
        Some(match name {
            "ErasedOwner" => Self::ErasedOwner,
            "SharedOwner" => Self::SharedOwner,
            "i8" => Self::I8,
            "u8" => Self::U8,
            "i16" => Self::I16,
            "u16" => Self::U16,
            "i32" => Self::I32,
            "u32" => Self::U32,
            "i64" => Self::I64,
            "u64" => Self::U64,
            "f32" => Self::F32,
            "f64" => Self::F64,
            "bool" => Self::Bool,
            "char" => Self::Char,
            "String" => Self::String,
            "unit" => Self::Unit,
            "usize" => Self::Usize,
            // C `int` is 16 bits here. `c_char` is a full cell, not `i8`.
            "c_char" | "c_short" | "c_int" => Self::I16,
            "c_uint" => Self::U16,
            "c_long" => Self::I32,
            "c_size_t" => Self::U32,
            _ => return None,
        })
    }

    pub fn is_int(self) -> bool {
        matches!(
            self,
            Self::I8
                | Self::U8
                | Self::I16
                | Self::U16
                | Self::I32
                | Self::U32
                | Self::I64
                | Self::U64
                | Self::Usize
        )
    }

    /// A vector owns a reference to its buffer.
    pub fn is_vec(self) -> bool {
        matches!(self, Self::Vec(_))
    }

    pub fn is_ref(self) -> bool {
        matches!(self, Self::Ref(_))
    }

    pub fn is_slice(self) -> bool {
        matches!(self, Self::Slice(_))
    }

    /// A vector or a slice owns a share of a heap buffer.
    pub fn owns_buffer(self) -> bool {
        self.is_vec() || self.is_slice()
    }

    /// The value owns a share of a refcounted heap object.
    pub fn is_managed(self) -> bool {
        self.owns_buffer()
            || matches!(
                self,
                Self::Adt(_)
                    | Self::Func(_)
                    | Self::String
                    | Self::Owned(_)
                    | Self::ErasedOwner
                    | Self::SharedOwner
            )
    }

    pub fn is_signed(self) -> bool {
        matches!(self, Self::I8 | Self::I16 | Self::I32 | Self::I64)
    }

    pub fn is_float(self) -> bool {
        matches!(self, Self::F32 | Self::F64)
    }

    /// Cells occupied in an activation record.
    pub fn cells(self) -> i64 {
        match self {
            Self::Constructor(_) => unreachable!("type constructor has no runtime layout"),
            Self::Unit => 0,
            Self::Adt(_) => 3,
            Self::Func(_) => 4,
            Self::I32 | Self::U32 | Self::Usize | Self::F32 => 2,
            Self::I64 | Self::U64 | Self::F64 => 4,
            Self::Vec(_)
            | Self::Owned(_)
            | Self::ErasedOwner
            | Self::SharedOwner
            | Self::Ref(_)
            | Self::Ptr(_)
            | Self::CFn(_)
            | Self::String => 2,
            Self::Char => 2,
            // Address, bank, 32-bit start, 32-bit count.
            Self::Slice(_) => 6,
            Self::Array { len, width, .. } => len as i64 * width as i64,
            Self::Struct { width, .. } | Self::Tuple { width, .. } => width as i64,
            _ => 1,
        }
    }

    pub fn in_range(self, value: i64) -> bool {
        match self {
            Self::I8 => (-128..=127).contains(&value),
            Self::U8 => (0..=255).contains(&value),
            Self::I16 => (-32768..=32767).contains(&value),
            Self::U16 => (0..=65535).contains(&value),
            Self::Usize | Self::U32 => (0..=u32::MAX as i64).contains(&value),
            Self::I32 => (i32::MIN as i64..=i32::MAX as i64).contains(&value),
            Self::I64 => true,
            Self::U64 => value >= 0,
            Self::Constructor(_)
            | Self::Bool
            | Self::Char
            | Self::Unit
            | Self::String
            | Self::F32
            | Self::F64
            | Self::Adt(_)
            | Self::Func(_)
            | Self::Vec(_)
            | Self::Owned(_)
            | Self::ErasedOwner
            | Self::SharedOwner
            | Self::Ref(_)
            | Self::Ptr(_)
            | Self::CFn(_)
            | Self::Array { .. }
            | Self::Slice(_)
            | Self::Struct { .. }
            | Self::Tuple { .. } => false,
        }
    }

    /// Low bits stored in the frame. Unsigned and sub-word values are zero-extended.
    pub fn bits(self, value: i64) -> i64 {
        match self {
            Self::I8 | Self::U8 => value & 0xff,
            Self::I16 | Self::U16 | Self::Bool => value & 0xffff,
            Self::I32 | Self::U32 | Self::Usize | Self::Char => value & 0xffff_ffff,
            Self::I64 | Self::U64 => value,
            Self::Constructor(_)
            | Self::Unit
            | Self::String
            | Self::F32
            | Self::F64
            | Self::Adt(_)
            | Self::Func(_)
            | Self::Vec(_)
            | Self::Owned(_)
            | Self::ErasedOwner
            | Self::SharedOwner
            | Self::Ref(_)
            | Self::Ptr(_)
            | Self::CFn(_)
            | Self::Array { .. }
            | Self::Slice(_)
            | Self::Struct { .. }
            | Self::Tuple { .. } => 0,
        }
    }
}

impl std::fmt::Display for Ty {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let name = match self {
            Self::Constructor(_) => "type constructor",
            Self::I8 => "i8",
            Self::U8 => "u8",
            Self::I16 => "i16",
            Self::U16 => "u16",
            Self::I32 => "i32",
            Self::U32 => "u32",
            Self::I64 => "i64",
            Self::U64 => "u64",
            Self::F32 => "f32",
            Self::F64 => "f64",
            Self::Bool => "bool",
            Self::Char => "char",
            Self::String => "String",
            Self::Unit => "unit",
            Self::Usize => "usize",
            Self::Adt(_) => "adt",
            Self::Func(_) => "fn",
            Self::Vec(_) => "vec",
            Self::Owned(_) => "Owned",
            Self::ErasedOwner => "ErasedOwner",
            Self::SharedOwner => "SharedOwner",
            Self::Ref(_) => "ref",
            Self::Array { .. } => "array",
            Self::Slice(_) => "slice",
            Self::Struct { .. } => "struct",
            Self::Tuple { .. } => "tuple",
            Self::Ptr(_) => "ptr",
            Self::CFn(_) => "extern fn",
        };
        f.write_str(name)
    }
}
