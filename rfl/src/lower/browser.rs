//! One compiler-owned callback root. Managed layouts never escape into C/JS.
use super::*;
const ROOT: &str = "__rfl_browser_root";
pub(super) fn intrinsic(name: &str) -> bool {
    matches!(name, "__browser_root_get" | "__browser_root_replace")
}
pub(super) fn global(module: &mut Module, built: &Built) {
    if built
        .procs
        .iter()
        .flat_map(|p| &p.segs)
        .flat_map(|s| &s.insns)
        .any(|i| matches!(i, Insn::Extern { name, .. } if intrinsic(name)))
    {
        module.add_global(GlobalVariable {
            name: ROOT.into(),
            var_type: IrType::Array {
                element_type: Box::new(IrType::I16),
                size: 4,
            },
            is_constant: false,
            initializer: Some(Value::ConstantArray(vec![0; 4])),
            linkage: Linkage::Internal,
            symbol_id: None,
        });
    }
}
pub(super) fn exchange(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    input: Option<usize>,
) -> Result<(), CompileError> {
    let root = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Global(ROOT.into())),
        bank: BankTag::Global,
    });
    let ty = proc.slots[dest].ty;
    let old = (0..4)
        .map(|i| load_cell(builder, &root, i, IrType::I16))
        .collect::<Result<Vec<_>, _>>()?;
    if let Some(input) = input {
        // Snapshot/retain before touching either destination. Publish the new
        // root before any old destination destructor can reenter stdlib code.
        let new = (0..4)
            .map(|i| load_cell(builder, frame, proc.slots[input].offset + i, IrType::I16))
            .collect::<Result<Vec<_>, _>>()?;
        retain_words(builder, &new, ty)?;
        for (i, value) in new.into_iter().enumerate() {
            store_cell(builder, &root, i as i64, value)?;
        }
        // Ownership of the old root's existing share transfers to the result.
    } else {
        retain_words(builder, &old, ty)?;
    }
    release_managed(builder, proc, frame, dest)?;
    for (i, value) in old.into_iter().enumerate() {
        store_cell(builder, frame, proc.slots[dest].offset + i as i64, value)?;
    }
    Ok(())
}
