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
    Bool,
    Unit,
    /// Index into the module's algebraic types. The value is a tag plus a heap pointer.
    Adt(u32),
    /// Index into the checker's function-type table. The value is four cells:
    /// function id, frame size, environment address, environment bank.
    Func(u32),
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
            _ => return None,
        })
    }

    pub fn is_int(self) -> bool {
        matches!(
            self,
            Self::I8 | Self::U8 | Self::I16 | Self::U16 | Self::I32 | Self::U32 | Self::I64 | Self::U64
        )
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
            _ => 1,
        }
    }

    pub fn in_range(self, value: i64) -> bool {
        match self {
            Self::I8 => (-128..=127).contains(&value),
            Self::U8 => (0..=255).contains(&value),
            Self::I16 => (-32768..=32767).contains(&value),
            Self::U16 => (0..=65535).contains(&value),
            Self::I32 => (i32::MIN as i64..=i32::MAX as i64).contains(&value),
            Self::U32 => (0..=u32::MAX as i64).contains(&value),
            Self::I64 => true,
            Self::U64 => value >= 0,
            Self::Bool | Self::Unit | Self::Adt(_) | Self::Func(_) => false,
        }
    }

    /// Low bits stored in the frame. Unsigned and sub-word values are zero-extended.
    pub fn bits(self, value: i64) -> i64 {
        match self {
            Self::I8 | Self::U8 => value & 0xff,
            Self::I16 | Self::U16 | Self::Bool => value & 0xffff,
            Self::I32 | Self::U32 => value & 0xffff_ffff,
            Self::I64 | Self::U64 => value,
            Self::Unit | Self::Adt(_) | Self::Func(_) => 0,
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
            Self::Adt(_) => "adt",
            Self::Func(_) => "fn",
        };
        f.write_str(name)
    }
}
