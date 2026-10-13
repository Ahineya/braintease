//! Optional execution diagnostics. Categories are complete; the bounded site
//! table retains the most expensive observed loop sites approximately. All
//! reported site counters are exact since admission, never extrapolated.

use super::{FastOp, Op};
use std::{collections::HashMap, time::Duration};

const KINDS: usize = 13;
const SITE_LIMIT: usize = 4096;

#[derive(Clone, Copy)]
pub(super) enum Kind {
    Search,
    Routed,
    Block,
    Affine,
    Scan,
    Walk,
    Add,
    Move,
    Open,
    Close,
    Input,
    Output,
    Hook,
}

impl Kind {
    pub(super) fn fast(operation: &FastOp) -> Self {
        match operation {
            FastOp::Search(_) => Self::Search,
            FastOp::Routed(_) => Self::Routed,
            FastOp::Block { .. } => Self::Block,
            FastOp::Affine { .. } => Self::Affine,
            FastOp::Scan { .. } => Self::Scan,
            FastOp::Walk { .. } => Self::Walk,
        }
    }

    pub(super) fn literal(operation: Op) -> Self {
        match operation {
            Op::Add(_) => Self::Add,
            Op::Move(_) => Self::Move,
            Op::Open(_) => Self::Open,
            Op::Close(_) => Self::Close,
            Op::Input => Self::Input,
            Op::Output => Self::Output,
            Op::Hook(_) => Self::Hook,
        }
    }

    fn name(self) -> &'static str {
        [
            "search",
            "routed",
            "block",
            "affine",
            "scan",
            "walk",
            "literal_add",
            "literal_move",
            "literal_open",
            "literal_close",
            "input",
            "output",
            "hook",
        ][self as usize]
    }

    fn tracks_sites(self) -> bool {
        // Straight-line initialization can have millions of unique blocks and
        // affine loops. Focus the bounded hotspot table on dynamic navigation;
        // the category totals still include every operation of every kind.
        matches!(self, Self::Search | Self::Routed | Self::Scan | Self::Walk)
    }
}

#[derive(Debug, Clone, Default)]
pub struct OperationProfile {
    pub kind: &'static str,
    pub attempts: u64,
    pub completed: u64,
    pub fallbacks: u64,
    pub elapsed: Duration,
}

impl OperationProfile {
    fn record(&mut self, kind: Kind, elapsed: Duration, completed: bool) {
        self.kind = kind.name();
        self.attempts += 1;
        self.completed += u64::from(completed);
        self.fallbacks += u64::from(!completed);
        self.elapsed += elapsed;
    }
}

#[derive(Debug, Clone)]
pub struct SiteProfile {
    /// Byte offset in the BF body passed to `Program::parse`.
    pub source_offset: usize,
    pub operation: OperationProfile,
}

#[derive(Debug)]
pub struct ProfileReport {
    pub dispatches: u64,
    pub operations: Vec<OperationProfile>,
    /// Expensive dynamic-loop sites, descending by measured operation time.
    /// Bounded retention can discard cold sites; counters are lower bounds on
    /// whole-run per-site totals if a previously discarded site is readmitted.
    pub sites: Vec<SiteProfile>,
    pub site_capacity: usize,
    pub untracked_site_attempts: u64,
    pub evicted_sites: u64,
}

pub struct Profile {
    dispatches: u64,
    operations: [OperationProfile; KINDS],
    sites: HashMap<usize, SiteProfile>,
    untracked_site_attempts: u64,
    evicted_sites: u64,
}

impl Default for Profile {
    fn default() -> Self {
        Self {
            dispatches: 0,
            operations: std::array::from_fn(|_| OperationProfile::default()),
            sites: HashMap::new(),
            untracked_site_attempts: 0,
            evicted_sites: 0,
        }
    }
}

impl Profile {
    pub fn report(&self) -> ProfileReport {
        let mut sites: Vec<_> = self.sites.values().cloned().collect();
        sites.sort_by(|a, b| {
            b.operation
                .elapsed
                .cmp(&a.operation.elapsed)
                .then_with(|| a.source_offset.cmp(&b.source_offset))
        });
        ProfileReport {
            dispatches: self.dispatches,
            operations: self
                .operations
                .iter()
                .filter(|row| row.attempts != 0)
                .cloned()
                .collect(),
            sites,
            site_capacity: SITE_LIMIT,
            untracked_site_attempts: self.untracked_site_attempts,
            evicted_sites: self.evicted_sites,
        }
    }

    pub(super) fn dispatch(&mut self) {
        self.dispatches += 1;
    }

    pub(super) fn record(&mut self, kind: Kind, offset: usize, elapsed: Duration, completed: bool) {
        self.operations[kind as usize].record(kind, elapsed, completed);
        if !kind.tracks_sites() {
            return;
        }
        if let Some(site) = self.sites.get_mut(&offset) {
            site.operation.record(kind, elapsed, completed);
            return;
        }
        if self.sites.len() == SITE_LIMIT {
            self.untracked_site_attempts += 1;
            // Amortize a bounded cold-site replacement rather than scanning
            // thousands of slots on every new instruction during startup.
            if self.untracked_site_attempts % 1024 != 0 {
                return;
            }
            let coldest = self
                .sites
                .values()
                .min_by_key(|site| site.operation.elapsed)
                .map(|site| site.source_offset)
                .unwrap();
            self.sites.remove(&coldest);
            self.evicted_sites += 1;
        }
        let mut operation = OperationProfile::default();
        operation.record(kind, elapsed, completed);
        self.sites.insert(
            offset,
            SiteProfile {
                source_offset: offset,
                operation,
            },
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::{Machine, Program, Stop};

    #[test]
    fn profiling_preserves_every_budget_io_hook_and_tape_state() {
        let program = Program::parse(",>+++<[->+<]>!.[->]$", true, true).unwrap();
        let mut plain = Machine::new(20).unwrap();
        let mut profiled = Machine::new(20).unwrap();
        let mut profile = Profile::default();
        for budget in [0, 1, 2, 1, 3, 1, 2, 100] {
            let want = plain.run(&program, budget);
            let got = profiled.run_profiled(&program, budget, &mut profile);
            assert_eq!(got, want);
            assert_eq!(profiled.tape(), plain.tape());
            assert_eq!(profiled.pointer(), plain.pointer());
            assert_eq!(profiled.dispatches(), plain.dispatches());
            if got == Ok(Stop::Input) {
                plain.provide_input(3).unwrap();
                profiled.provide_input(3).unwrap();
            }
        }
        assert_eq!(profile.report().dispatches, plain.dispatches());
        assert_eq!(
            profile
                .report()
                .operations
                .iter()
                .map(|row| row.attempts)
                .sum::<u64>(),
            plain.dispatches()
        );
    }

    #[test]
    fn failed_fast_paths_are_separate_from_literal_fallbacks() {
        let program = Program::parse("+[->+<]", true, false).unwrap();
        let mut machine = Machine::new(1).unwrap();
        let mut profile = Profile::default();
        assert!(machine.run_profiled(&program, 100, &mut profile).is_err());
        let report = profile.report();
        let affine = report
            .operations
            .iter()
            .find(|row| row.kind == "affine")
            .unwrap();
        assert_eq!(affine.attempts, 1);
        assert_eq!(affine.fallbacks, 1);
        assert_eq!(affine.completed, 0);
        assert!(report
            .operations
            .iter()
            .any(|row| row.kind == "literal_open"));
    }

    #[test]
    fn site_storage_is_bounded_and_reports_losses() {
        let mut profile = Profile::default();
        for offset in 0..SITE_LIMIT + 2048 {
            profile.record(Kind::Scan, offset, Duration::from_nanos(1), true);
        }
        let report = profile.report();
        assert_eq!(report.sites.len(), SITE_LIMIT);
        assert_eq!(report.untracked_site_attempts, 2048);
        assert_eq!(report.evicted_sites, 2);
        assert_eq!(report.operations[0].attempts, (SITE_LIMIT + 2048) as u64);
    }
}
