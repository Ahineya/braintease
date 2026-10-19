//! Native compatibility wrappers around shared whole-program IR linking.
use rcc_ir::{ir::*, target::Target};

pub fn link_modules(modules: &[Module]) -> Result<Module, String> {
    rcc_ir::link::link_modules(modules, Target::Native, &["main"])
}
pub(super) fn retain_reachable(module: &mut Module) {
    rcc_ir::link::retain_reachable(module, &["main"]);
}

pub(super) fn hoist_static_allocas(module: &mut Module) {
    rcc_ir::link::hoist_static_allocas(module);
}
