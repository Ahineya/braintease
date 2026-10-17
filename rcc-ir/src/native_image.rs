//! On-disk envelope for independently compiled, trusted hosted applications.
//! The payload is a host shared object; compatibility data and function tokens
//! are relocated by the runtime, independently of the host dynamic linker.
use serde::{Deserialize, Serialize};

pub const MAGIC: &[u8; 8] = b"FUSEX001";
pub const ABI: u32 = 1;
pub const MAX_IMAGE_BYTES: usize = 8 * 1024 * 1024;

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub abi: u32,
    pub arch: String,
    pub os: String,
    pub entry: u32,
    pub global_cells: u32,
    pub errno: u64,
    pub functions: Vec<Function>,
    pub helpers: Vec<Helper>,
    /// (logical address, value, cell count, value is a local function token)
    pub initializers: Vec<(u64, u64, u64, bool)>,
}
#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Function {
    pub token: u32,
    pub symbol: String,
    pub arity: u32,
}
#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Helper {
    pub symbol: String,
    pub arity: usize,
}

/// Application I/O, allocation and exit must use guest libc and the kernel.
/// Only cell/string operations and scalar math may use direct host adapters.
pub fn application_service(name: &str) -> bool {
    matches!(
        name,
        "abs"
            | "labs"
            | "llabs"
            | "memset"
            | "memcpy"
            | "memmove"
            | "strlen"
            | "strcmp"
            | "strncmp"
            | "strcpy"
            | "strncpy"
            | "strcat"
            | "memcmp"
            | "memchr"
            | "strchr"
            | "strrchr"
            | "__rfl_host_sin"
            | "__rfl_host_cos"
            | "__rcc_f64_floor"
            | "__rcc_f64_sqrt"
    )
}
