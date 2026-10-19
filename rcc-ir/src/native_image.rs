//! On-disk envelope for independently compiled, trusted hosted applications.
//! The payload is a host shared object; compatibility data and function tokens
//! are relocated by the runtime, independently of the host dynamic linker.
use serde::{Deserialize, Serialize};

pub const MAGIC: &[u8; 8] = b"FUSEX001";
pub const ABI: u32 = 2;
pub const LEGACY_ABI: u32 = 1;
pub const MODULE_ABI: u32 = 3;
pub const GLOBAL_BANK: u32 = 32;
pub const GLOBAL_BANKS: u32 = 16;
pub const MAX_IMAGE_BYTES: usize = 64 * 1024 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub abi: u32,
    pub arch: String,
    pub os: String,
    pub entry: u32,
    pub global_cells: u32,
    /// Older packages omit this and place their globals in bank one.
    #[serde(default = "legacy_global_bank")]
    pub global_bank: u32,
    pub errno: u64,
    pub functions: Vec<Function>,
    pub helpers: Vec<Helper>,
    /// (logical address, value, cell count, value is a local function token)
    pub initializers: Vec<(u64, u64, u64, bool)>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub module: Option<ModuleMetadata>,
}

/// Guest linking is independent of the host Mach-O/ELF symbol namespace.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ModuleMetadata {
    /// Empty for an executable; libraries use a versioned SONAME.
    pub name: String,
    pub dependencies: Vec<Dependency>,
    pub exports: Vec<Symbol>,
    pub imports: Vec<Import>,
    pub relocations: Vec<Relocation>,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Dependency {
    pub name: String,
    pub path: String,
    /// Exact interface identity, including the guest calling/data convention.
    pub interface: String,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Symbol {
    pub name: String,
    pub value: u64,
    pub function: bool,
    pub signature: String,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Import {
    pub local: u64,
    pub library: String,
    pub symbol: Symbol,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Relocation {
    /// Dense cell index in the module's data, storing a two-cell pointer.
    pub offset: u32,
    pub target: u64,
    pub function: bool,
    #[serde(default, skip_serializing_if = "is_zero")]
    pub addend: i64,
}
fn is_zero(value: &i64) -> bool {
    *value == 0
}

pub fn interface(module: &ModuleMetadata) -> String {
    use sha2::{Digest, Sha256};
    let mut symbols: Vec<_> = module
        .exports
        .iter()
        .map(|s| (&s.name, s.function, &s.signature))
        .collect();
    symbols.sort();
    let bytes = serde_json::to_vec(&("c16-native-continuations-3", symbols)).unwrap();
    format!("{:x}", Sha256::digest(bytes))
}

pub fn content_id(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    format!("{:x}", Sha256::digest(bytes))
}

/// ABI 3 stores a dense little-endian initializer prefix between JSON and code.
/// Unstored trailing cells are zero. ABI 1/2 retain their original envelope.
pub fn decode(bytes: &[u8]) -> Result<(Manifest, &[u8], &[u8]), String> {
    if bytes.len() < 16 || bytes.len() > MAX_IMAGE_BYTES || &bytes[..8] != MAGIC {
        return Err("invalid executable header".into());
    }
    let json = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    let code = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
    let end = 16usize
        .checked_add(json)
        .filter(|n| *n <= bytes.len())
        .ok_or("invalid manifest length")?;
    let start = bytes
        .len()
        .checked_sub(code)
        .filter(|n| *n >= end)
        .ok_or("invalid native code length")?;
    if code == 0 {
        return Err("invalid native code length".into());
    }
    let m: Manifest =
        serde_json::from_slice(&bytes[16..end]).map_err(|e| format!("invalid manifest: {e}"))?;
    if !matches!(m.abi, LEGACY_ABI | ABI | MODULE_ABI) {
        return Err("incompatible native executable ABI".into());
    }
    m.global_capacity()?;
    if (m.abi != MODULE_ABI && start != end)
        || (start - end) % 2 != 0
        || (start - end) / 2 > m.global_cells as usize
        || (m.abi != MODULE_ABI && m.module.is_some())
    {
        return Err("invalid initializer section".into());
    }
    Ok((m, &bytes[end..start], &bytes[start..]))
}
fn legacy_global_bank() -> u32 {
    1
}

impl Manifest {
    /// Validate a complete data region before any loader memory writes.
    pub fn global_capacity(&self) -> Result<u64, String> {
        let bank_cells = u64::from(crate::target::DataLayout::RIPPLE16.bank_cells);
        let capacity = match (self.abi, self.global_bank) {
            (LEGACY_ABI | ABI | MODULE_ABI, 1) => bank_cells,
            (ABI | MODULE_ABI, GLOBAL_BANK) => bank_cells * u64::from(GLOBAL_BANKS),
            _ => return Err("invalid application global region".into()),
        };
        if self.global_cells == 0 || u64::from(self.global_cells) > capacity {
            return Err("invalid application global size".into());
        }
        Ok(capacity)
    }

    /// Dense index relative to the region. Offset zero in later banks is valid;
    /// only the reserved first cell and noncanonical pointer offsets are rejected.
    pub fn global_index(&self, pointer: u64, width: u64) -> Result<usize, String> {
        self.global_capacity()?;
        let bank_cells = u64::from(crate::target::DataLayout::RIPPLE16.bank_cells);
        let bank = pointer >> 16;
        let offset = pointer & 65535;
        if bank < u64::from(self.global_bank) || offset >= bank_cells || width == 0 {
            return Err("invalid application global address".into());
        }
        let index = (bank - u64::from(self.global_bank)) * bank_cells + offset;
        if index == 0
            || index
                .checked_add(width)
                .is_none_or(|end| end > u64::from(self.global_cells))
        {
            return Err("application global span out of bounds".into());
        }
        Ok(index as usize)
    }
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Function {
    pub token: u32,
    pub symbol: String,
    pub arity: u32,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
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
            | "__rcc_host_math"
            | "__rfl_host_sin"
            | "__rfl_host_cos"
            | "__rcc_f64_floor"
            | "__rcc_f64_sqrt"
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    fn manifest() -> Manifest {
        Manifest {
            abi: ABI,
            arch: String::new(),
            os: String::new(),
            entry: 0,
            global_cells: 128005,
            global_bank: GLOBAL_BANK,
            errno: 0,
            functions: vec![],
            helpers: vec![],
            initializers: vec![],
            module: None,
        }
    }
    #[test]
    fn global_spans_use_dense_cells_and_allow_bank_crossings() {
        let m = manifest();
        assert_eq!(m.global_index((32 << 16) | 63999, 4).unwrap(), 63999);
        assert_eq!(m.global_index(33 << 16, 1).unwrap(), 64000);
        assert_eq!(m.global_index((34 << 16) | 4, 1).unwrap(), 128004);
        for (pointer, width) in [
            (32 << 16, 1),
            ((32 << 16) | 64000, 1),
            (31 << 16, 1),
            ((34 << 16) | 5, 1),
            ((34 << 16) | 4, 2),
            (240 << 16, 1),
            (u64::MAX, 4),
            ((32 << 16) | 1, 0),
        ] {
            assert!(
                m.global_index(pointer, width).is_err(),
                "{pointer:#x}, {width}"
            );
        }
    }
    #[test]
    fn global_layout_rejects_overlaps_and_preserves_legacy_packages() {
        let mut m = manifest();
        for bank in [0, 2, 8, 31, 33, 240, u32::MAX] {
            m.global_bank = bank;
            assert!(m.global_capacity().is_err());
        }
        m.global_bank = GLOBAL_BANK;
        m.global_cells = 16 * 64000;
        assert!(m.global_capacity().is_ok());
        m.global_cells += 1;
        assert!(m.global_capacity().is_err());
        m.abi = LEGACY_ABI;
        m.global_cells = 10;
        assert!(m.global_capacity().is_err());
        m.global_bank = 1;
        assert_eq!(m.global_index(0x10009, 1).unwrap(), 9);
        m.global_cells = 64001;
        assert!(m.global_capacity().is_err());
        m.global_cells = 0;
        assert!(m.global_capacity().is_err());
        let old = r#"{"abi":1,"arch":"aarch64","os":"macos","entry":0,"global_cells":10,"errno":0,"functions":[],"helpers":[],"initializers":[]}"#;
        let m: Manifest = serde_json::from_str(old).unwrap();
        assert_eq!(m.global_bank, 1);
        assert!(m.global_capacity().is_ok());
    }
    #[test]
    fn dense_data_section_is_versioned_and_length_checked() {
        fn package(m: &Manifest, data: &[u8]) -> Vec<u8> {
            let json = serde_json::to_vec(m).unwrap();
            let mut bytes = MAGIC.to_vec();
            bytes.extend_from_slice(&(json.len() as u32).to_le_bytes());
            bytes.extend_from_slice(&1u32.to_le_bytes());
            bytes.extend_from_slice(&json);
            bytes.extend_from_slice(data);
            bytes.push(42);
            bytes
        }
        let mut m = manifest();
        m.abi = MODULE_ABI;
        m.global_cells = 3;
        let bytes = package(&m, &[0, 0, 0x34, 0x12]);
        let (_, data, code) = decode(&bytes).unwrap();
        assert_eq!(data, &[0, 0, 0x34, 0x12]);
        assert_eq!(code, &[42]);
        assert!(decode(&package(&m, &[0])).is_err());
        assert!(decode(&package(&m, &[0; 8])).is_err());
        m.abi = ABI;
        assert!(decode(&package(&m, &[0, 0])).is_err());
        assert!(decode(&package(&m, &[])).is_ok());
        let mut truncated = package(&m, &[]);
        truncated.pop();
        assert!(decode(&truncated).is_err());
    }
}
