//! Serialize opt-in diagnostics from the generic Brainfuck engine.
use crate::runner::RunReport;
use bf_wasm::engine::{OperationProfile, Profile};
use serde_json::{json, Value};
use std::path::Path;

fn operation(value: &OperationProfile) -> Value {
    json!({
        "kind": value.kind,
        "attempts": value.attempts,
        "completed": value.completed,
        "fallbacks": value.fallbacks,
        "seconds": value.elapsed.as_secs_f64(),
    })
}

/// Source offsets refer to the expanded file, including its BFHOST header.
/// Profiling times exclude parsing, host callbacks and profiler bookkeeping.
pub fn write(
    path: &Path,
    profile: &Profile,
    run: &RunReport,
    body_offset: usize,
) -> Result<(), String> {
    let profile = profile.report();
    let operations: Vec<_> = profile.operations.iter().map(operation).collect();
    let sites: Vec<_> = profile
        .sites
        .iter()
        .map(|site| {
            let mut value = operation(&site.operation);
            value["source_offset"] = json!(body_offset + site.source_offset);
            value
        })
        .collect();
    let report = json!({
        "profile_version": 1,
        "source_sha256": run.source_sha256,
        "success": run.success,
        "stop_reason": run.stop_reason,
        "elapsed_seconds": run.elapsed_seconds,
        "bf_dispatches": run.bf_dispatches,
        "ripple_instructions": run.ripple_instructions,
        "profiled_dispatches": profile.dispatches,
        "operations": operations,
        "sites": sites,
        "site_capacity": profile.site_capacity,
        "untracked_site_attempts": profile.untracked_site_attempts,
        "evicted_sites": profile.evicted_sites,
        "notes": "Opt-in instrumentation adds overhead. Operation times exclude dispatch/profiler bookkeeping and host callbacks; offsets are zero-based bytes in the expanded BF file. Sites cover dynamic navigation loops and may be evicted; operation totals remain complete. Use unprofiled runs for speed comparisons.",
    });
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }
    let bytes = serde_json::to_vec_pretty(&report).map_err(|error| error.to_string())?;
    std::fs::write(path, bytes)
        .map_err(|error| format!("Writing BF profile {}: {error}", path.display()))
}
