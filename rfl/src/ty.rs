/// Concrete RFL types after checking. Integer literals have already been given a width.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Ty {
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    /// One unsigned cell. Wide enough for every collection length this machine can allocate.
    Usize,
    Bool,
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
    /// Index into the checker's reference table. The value is a pointer to a
    /// local: address cell, bank cell. It cannot outlive that local.
    Ref(u32),
    /// Inline `[T; N]`. `id` finds the element type. `width` is that element's cells.
    Array { id: u32, len: u16, width: u16 },
    /// Index into the slice element table. Four cells: buffer address, bank,
    /// first element, and element count. The buffer is a vector allocation.
    Slice(u32),
    /// Inline struct. `id` indexes the struct table. `width` is the total cell count.
    Struct { id: u32, width: u16 },
    /// Inline tuple. `id` indexes the tuple table. `width` is the total cell count.
    Tuple { id: u32, width: u16 },
}

impl Ty {
    pub fn parse_name(name: &str) -> Option<Self> {
        Some(match name {
            "i8" => Self::I8,
            "u8" => Self::U8,
            "i16" => Self::I16,
            "u16" => Self::U16,
            "i32" => Self::I32,
            "u32" => Self::U32,
            "i64" => Self::I64,
            "u64" => Self::U64,
            "bool" => Self::Bool,
            "unit" => Self::Unit,
            "usize" => Self::Usize,
            _ => return None,
        })
    }

    pub fn is_int(self) -> bool {
        matches!(
            self,
            Self::I8 | Self::U8 | Self::I16 | Self::U16 | Self::I32 | Self::U32 | Self::I64 | Self::U64 | Self::Usize
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
        self.owns_buffer() || matches!(self, Self::Adt(_) | Self::Func(_))
    }

    pub fn is_signed(self) -> bool {
        matches!(self, Self::I8 | Self::I16 | Self::I32 | Self::I64)
    }

    /// Cells occupied in an activation record.
    pub fn cells(self) -> i64 {
        match self {
            Self::Unit => 0,
            Self::Adt(_) => 3,
            Self::Func(_) => 4,
            Self::I32 | Self::U32 => 2,
            Self::I64 | Self::U64 => 4,
            Self::Vec(_) | Self::Ref(_) => 2,
            Self::Slice(_) => 4,
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
            Self::U16 | Self::Usize => (0..=65535).contains(&value),
            Self::I32 => (i32::MIN as i64..=i32::MAX as i64).contains(&value),
            Self::U32 => (0..=u32::MAX as i64).contains(&value),
            Self::I64 => true,
            Self::U64 => value >= 0,
            Self::Bool | Self::Unit | Self::Adt(_) | Self::Func(_) | Self::Vec(_) | Self::Ref(_) | Self::Array { .. } | Self::Slice(_) | Self::Struct { .. } | Self::Tuple { .. } => false,
        }
    }

    /// Low bits stored in the frame. Unsigned and sub-word values are zero-extended.
    pub fn bits(self, value: i64) -> i64 {
        match self {
            Self::I8 | Self::U8 => value & 0xff,
            Self::I16 | Self::U16 | Self::Usize | Self::Bool => value & 0xffff,
            Self::I32 | Self::U32 => value & 0xffff_ffff,
            Self::I64 | Self::U64 => value,
            Self::Unit | Self::Adt(_) | Self::Func(_) | Self::Vec(_) | Self::Ref(_) | Self::Array { .. } | Self::Slice(_) | Self::Struct { .. } | Self::Tuple { .. } => 0,
        }
    }
}

impl std::fmt::Display for Ty {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let name = match self {
            Self::I8 => "i8",
            Self::U8 => "u8",
            Self::I16 => "i16",
            Self::U16 => "u16",
            Self::I32 => "i32",
            Self::U32 => "u32",
            Self::I64 => "i64",
            Self::U64 => "u64",
            Self::Bool => "bool",
            Self::Unit => "unit",
            Self::Usize => "usize",
            Self::Adt(_) => "adt",
            Self::Func(_) => "fn",
            Self::Vec(_) => "vec",
            Self::Ref(_) => "ref",
            Self::Array { .. } => "array",
            Self::Slice(_) => "slice",
            Self::Struct { .. } => "struct",
            Self::Tuple { .. } => "tuple",
        };
        f.write_str(name)
    }
}
