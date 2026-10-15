//! Explicit error for portable builds without a native code generator.
#[path = "stats.rs"]
mod stats;
use super::{Error, Machine, Program, Stop};
pub use stats::JitStats;

pub struct Jit<'program> {
    _program: &'program Program,
    stats: JitStats,
}
impl<'program> Jit<'program> {
    pub fn new(_program: &'program Program) -> Result<Self, Error> {
        Err(Error::Jit(
            "BF JIT requires the jit feature and an aarch64 or x86_64 host".into(),
        ))
    }
    pub fn stats(&self) -> &JitStats {
        &self.stats
    }
    pub fn run(&mut self, _machine: &mut Machine, _budget: u64) -> Result<Stop, Error> {
        Err(Error::Jit("BF JIT is unavailable in this build".into()))
    }
}
