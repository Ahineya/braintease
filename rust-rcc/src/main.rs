#![feature(rustc_private)]

extern crate rustc_abi;
extern crate rustc_driver;
extern crate rustc_hir;
extern crate rustc_interface;
extern crate rustc_middle;
extern crate rustc_span;

mod layout;
mod lower;
mod memory;

use rustc_driver::{Callbacks, Compilation};
use rustc_interface::interface;
use rustc_middle::ty::TyCtxt;

struct Driver {
    output: std::path::PathBuf,
    native: bool,
}
impl Callbacks for Driver {
    fn after_analysis<'tcx>(&mut self, _: &interface::Compiler, tcx: TyCtxt<'tcx>) -> Compilation {
        let result = (|| -> Result<(), String> {
            let entry = tcx
                .mir_keys(())
                .iter()
                .find(|id| {
                    tcx.def_kind(id.to_def_id()) == rustc_hir::def::DefKind::Fn
                        && tcx.item_name(id.to_def_id()).as_str() == "rust_entry"
                })
                .ok_or("provide fn rust_entry() -> u16")?
                .to_def_id();
            let signature = tcx.fn_sig(entry).instantiate_identity().skip_binder();
            if !signature.inputs().is_empty() || signature.output() != tcx.types.u16 {
                return Err("entry must have signature fn rust_entry() -> u16".into());
            }
            let module = lower::Backend::new(tcx, self.native).compile(entry)?;
            std::fs::write(&self.output, rcc_ir::emit_module(&module))
                .map_err(|e| e.to_string())?;
            eprintln!(
                "Rust → RCC: {} functions, {} byte-data objects → {}",
                module.functions.len(),
                module.globals.len(),
                self.output.display()
            );
            Ok(())
        })();
        if let Err(error) = result {
            tcx.dcx().fatal(format!("rust-rcc: {error}"));
        }
        Compilation::Stop
    }
}

fn main() -> std::process::ExitCode {
    let mut args = Vec::new();
    let mut output = None;
    let mut native = false;
    let mut input = std::env::args();
    while let Some(argument) = input.next() {
        match argument.as_str() {
            "--rcc-output" => output = input.next().map(std::path::PathBuf::from),
            "--rcc-native" => native = true,
            _ => args.push(argument),
        }
    }
    let Some(output) = output else {
        eprintln!(
            "use python3 rust-rcc/compile.py SOURCE --target fuzix|fuse-x --emit-ir -o OUTPUT.ir"
        );
        return std::process::ExitCode::FAILURE;
    };
    rustc_driver::catch_with_exit_code(|| {
        rustc_driver::run_compiler(&args, &mut Driver { output, native });
    })
}
