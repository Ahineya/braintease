#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct JitStats {
    pub compiled_regions: usize,
    pub retired_regions: usize,
    pub compiled_operations: usize,
    pub native_bytes: usize,
    pub compile_milliseconds: f64,
    pub max_compile_milliseconds: f64,
    pub native_dispatches: u64,
    pub entries: u64,
    pub bounds_fallbacks: u64,
}
