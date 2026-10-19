//! Browser execution uses the reference interpreter, never native code pages.
use super::VM;
#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct JitStats;
pub(super) struct JitEngine;
impl VM {
    pub fn jit_stats(&self) -> Option<JitStats> {
        None
    }
    pub(crate) fn jit_flush(&mut self) {}
    pub(crate) fn jit_flush_mappings(&mut self) {}
    pub(crate) fn jit_invalidate_index(&mut self, _index: usize) {}
    pub(crate) fn jit_invalidate_range(&mut self, _range: std::ops::Range<usize>) {}
    pub(super) fn run_batch_jit(&mut self, budget: u64) -> Result<u64, String> {
        self.run_batch_interpreted(budget)
    }
}
