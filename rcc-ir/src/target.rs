//! Target properties shared by frontends, layout and code generation.
//!
//! The first native target deliberately keeps the Ripple data ABI. Native code
//! generation and host math do not imply a host C data model or host pointers.
use crate::ir::IrType;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum Target {
    #[default]
    Ripple,
    Native,
    Wasm,
}

impl Target {
    pub fn parse(name: &str) -> Result<Self, String> {
        match name {
            "ripple" => Ok(Self::Ripple),
            "native" => Ok(Self::Native),
            "wasm" => Ok(Self::Wasm),
            _ => Err(format!(
                "unknown target {name:?}; expected ripple, native or wasm"
            )),
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Ripple => "ripple",
            Self::Native => "native",
            Self::Wasm => "wasm",
        }
    }
    pub fn has(self, capability: &str) -> Result<bool, String> {
        match capability {
            "host_math" => Ok(self == Self::Native),
            "native_float" => Ok(matches!(self, Self::Native | Self::Wasm)),
            _ => Err(format!("unknown target capability {capability:?}")),
        }
    }
    pub fn layout(self) -> DataLayout {
        DataLayout::RIPPLE16
    }
}

/// Explicit compatibility ABI, independent of the host code generator.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DataLayout {
    pub cell_bits: u8,
    pub pointer_bits: u8,
    pub c_int_bits: u8,
    pub c_char_bits: u8,
    pub bank_cells: u32,
}
impl DataLayout {
    pub const RIPPLE16: Self = Self {
        cell_bits: 16,
        pointer_bits: 32,
        c_int_bits: 16,
        c_char_bits: 16,
        bank_cells: 64000,
    };
    pub fn size_in_cells(self, ty: &IrType) -> Result<u64, String> {
        let scalar = match ty {
            IrType::Void | IrType::Function { .. } | IrType::Label => {
                return Err(format!("{ty} has no storage size"))
            }
            IrType::I1 | IrType::I8 | IrType::I16 => 1,
            IrType::I32 | IrType::F32 => 2,
            IrType::I64 | IrType::F64 => 4,
            IrType::FatPtr(_) => u64::from(self.pointer_bits).div_ceil(u64::from(self.cell_bits)),
            IrType::Array { size, element_type } => {
                return self
                    .size_in_cells(element_type)?
                    .checked_mul(*size)
                    .ok_or("array layout overflow".into())
            }
            IrType::Struct { fields, .. } => {
                return fields.iter().try_fold(0u64, |size, field| {
                    size.checked_add(self.size_in_cells(field)?)
                        .ok_or("struct layout overflow".into())
                })
            }
        };
        Ok(scalar)
    }
}

/// V1 native runtime bindings use the compiler's private scalar-carrier ABI.
/// Keeping their arities explicit prevents accidentally calling host libc with
/// logical pointers or silently choosing a signature from an untyped callsite.
pub const NATIVE_SERVICES: &[(&str, usize)] = &[
    // Standalone host platform services; require the host-tui runtime feature.
    ("rfl_posix_errno", 0),
    ("rfl_posix_getpid", 0),
    ("rfl_posix_name_capacity", 0),
    ("rfl_posix_argc", 0),
    ("rfl_posix_argument", 1),
    ("rfl_posix_getenv", 1),
    ("rfl_posix_error_message", 1),
    ("rfl_posix_exit", 1),
    ("rfl_posix_open", 2),
    ("rfl_posix_close", 1),
    ("rfl_posix_dup", 1),
    ("rfl_posix_read", 3),
    ("rfl_posix_write", 3),
    ("rfl_posix_seek", 3),
    ("rfl_posix_unlink", 1),
    ("rfl_posix_rename", 2),
    ("rfl_posix_mkdir", 1),
    ("rfl_posix_rmdir", 1),
    ("rfl_posix_chdir", 1),
    ("rfl_posix_chmod", 2),
    ("rfl_posix_cwd", 0),
    ("rfl_posix_executable", 1),
    ("rfl_posix_metadata", 2),
    ("rfl_posix_opendir", 1),
    ("rfl_posix_closedir", 1),
    ("rfl_posix_readdir", 2),
    ("rfl_posix_system", 1),
    ("rfl_posix_program", 2),
    ("rfl_curses_rows", 0),
    ("rfl_curses_columns", 0),
    ("rfl_curses_clear", 0),
    ("rfl_curses_move", 2),
    ("rfl_curses_style", 1),
    ("rfl_curses_character", 1),
    ("rfl_curses_refresh", 0),
    ("rfl_curses_key", 0),
    ("rfl_curses_timeout", 1),
    ("rfl_curses_cursor", 1),
    ("rfl_curses_suspend", 0),
    ("rfl_curses_resume", 0),
    ("rfl_curses_colors", 0),
    ("rfl_curses_pair", 3),
    ("rfl_curses_lines", 1),
    ("rfl_curses_bracketed", 1),
    ("rfl_curses_reposition", 3),
    ("rfl_curses_height", 1),
    ("rfl_curses_width", 1),
    ("rfl_curses_background", 2),
    ("rfl_curses_text_begin", 4),
    ("rfl_curses_fill", 6),
    ("rfl_curses_border", 6),
    ("rfl_curses_stage", 1),
    ("rfl_curses_present", 0),
    ("rfl_curses_window_key", 1),
    ("rfl_curses_window_cursor", 4),
    ("rfl_curses_scroll", 2),
    ("rfl_curses_region", 3),
    ("rfl_curses_shape", 1),
    ("rfl_curses_begin", 0),
    ("rfl_curses_end", 0),
    ("rfl_curses_screen", 0),
    ("rfl_curses_window", 4),
    ("rfl_curses_delete", 1),
    ("rfl_curses_scratch", 0),
    ("rfl_curses_text_chunk", 3),
    ("rfl_curses_event", 2),
    ("rfl_host_cells", 5),
    ("rfl_host_glyph", 2),
    ("rfl_host_text_run", 4),
    ("tui_theme_new", 2),
    ("tui_theme_free", 1),
    ("tui_theme_define", 5),
    ("tui_theme_apply", 4),
    ("tui_theme_named", 3),
    ("tui_theme_file", 4),
    ("tui_theme_valid_name", 1),
    ("tui_theme_error", 1),
    ("tui_theme_name", 1),
    ("tui_theme_lines", 1),
    ("tui_theme_count", 1),
    ("tui_theme_fg", 2),
    ("tui_theme_bg", 2),
    ("tui_theme_flags", 2),
    ("rcc_hosted_image_begin", 1),
    ("rcc_hosted_image_append", 2),
    ("rcc_hosted_image_finish", 0),
    ("rcc_hosted_image_prepare", 0),
    ("rcc_hosted_image_poll", 0),
    ("rcc_hosted_image_dependency", 2),
    ("rcc_hosted_image_reuse", 0),
    ("rcc_hosted_image_cancel", 0),
    ("rcc_hosted_valid_function", 2),
    ("rcc_hosted_image_load", 2),
    ("rcc_hosted_image_discard", 1),
    ("rcc_hosted_bind_syscall", 1),
    ("rcc_hosted_bind_checkpoint", 1),
    ("rcc_hosted_stopping", 0),
    ("rcc_hosted_lock_image", 2),
    ("rcc_hosted_sync_image", 0),
    ("rcc_hosted_read_cells", 3),
    ("rcc_hosted_write_cells", 3),
    ("rcc_hosted_input", 0),
    ("rcc_hosted_idle", 1),
    ("rcc_hosted_fs", 2),
    ("rcc_hosted_gx", 2),
    ("abs", 1),
    ("labs", 1),
    ("llabs", 1),
    ("remove", 1),
    ("rename", 2),
    ("fopen", 2),
    ("fclose", 1),
    ("fread", 4),
    ("fwrite", 4),
    ("fgetc", 1),
    ("getc", 1),
    ("fseek", 3),
    ("ftell", 1),
    ("feof", 1),
    ("ferror", 1),
    ("fflush", 1),
    ("fputc", 2),
    ("putc", 2),
    ("fputs", 2),
    ("putchar", 1),
    ("puts", 1),
    ("getchar", 0),
    ("malloc", 1),
    ("free", 1),
    ("calloc", 2),
    ("realloc", 2),
    ("memset", 3),
    ("memcpy", 3),
    ("memmove", 3),
    ("strlen", 1),
    ("strcmp", 2),
    ("strncmp", 3),
    ("strcpy", 2),
    ("strncpy", 3),
    ("strcat", 2),
    ("memcmp", 3),
    ("memchr", 3),
    ("strchr", 2),
    ("strrchr", 2),
    ("mmio_read", 1),
    ("mmio_write", 2),
    ("rfl_halt", 0),
    ("exit", 1),
    ("__rcc_host_math", 3),
    ("__rfl_host_sin", 1),
    ("__rfl_host_cos", 1),
    ("__rcc_f64_floor", 1),
    ("__rcc_f64_sqrt", 1),
];
