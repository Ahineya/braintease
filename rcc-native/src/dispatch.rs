//! Recover pure equality/range dispatch from portable IR. Known keys take a
//! native switch; misses retain the original CFG. No side effect is skipped.
use super::emit::{result_type, width, Cg};
use cranelift_codegen::ir::InstBuilder;
use cranelift_frontend::Switch;
use rcc_ir::ir::{BasicBlock, Function, Instruction as I, IrBinaryOp as Op, Value};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone)]
struct Predicate {
    result: u32,
    op: Op,
    key: u64,
    truth: bool,
}
#[derive(Clone)]
struct Case {
    key: u64,
    from: u32,
    to: u32,
    path: Vec<Predicate>,
}
#[derive(Clone)]
pub(super) struct Dispatch {
    selector: Value,
    bits: u32,
    cases: Vec<Case>,
    pub backward: bool,
}

fn condition(block: &BasicBlock, root: bool) -> Option<(u32, Op, Value, i64, u32, u32)> {
    let instructions: Vec<_> = block
        .instructions
        .iter()
        .filter(|i| !matches!(i, I::Comment(_) | I::DebugLoc { .. }))
        .collect();
    if instructions.len() < 2 || (!root && instructions.len() != 2) {
        return None;
    }
    let I::BranchCond {
        condition: Value::Temp(test),
        true_label,
        false_label,
    } = instructions[instructions.len() - 1]
    else {
        return None;
    };
    let I::Binary {
        result,
        op: op @ (Op::Eq | Op::Slt | Op::Ult),
        lhs: selector @ Value::Temp(_),
        rhs: Value::Constant(key),
        ..
    } = instructions[instructions.len() - 2]
    else {
        return None;
    };
    if test != result {
        return None;
    }
    Some((
        *result,
        *op,
        selector.clone(),
        *key,
        *true_label,
        *false_label,
    ))
}

fn signed(value: u64, bits: u32) -> i64 {
    ((value << (64 - bits)) as i64) >> (64 - bits)
}
fn matches(value: u64, p: &Predicate, bits: u32) -> bool {
    let comparison = match p.op {
        Op::Eq => value == p.key,
        Op::Ult => value < p.key,
        Op::Slt => signed(value, bits) < signed(p.key, bits),
        _ => unreachable!(),
    };
    comparison == p.truth
}

struct Search<'a> {
    blocks: &'a BTreeMap<u32, &'a BasicBlock>,
    selector: Value,
    bits: u32,
    mask: u64,
    nodes: BTreeSet<u32>,
    active: BTreeSet<u32>,
    cases: Vec<Case>,
    backward: bool,
    cycle_edges: &'a BTreeSet<(u32, u32)>,
}
impl Search<'_> {
    fn visit(&mut self, id: u32, root: bool, path: &mut Vec<Predicate>) -> Option<()> {
        let block = *self.blocks.get(&id)?;
        let Some((result, op, selector, key, yes, no)) = condition(block, root) else {
            return Some(());
        };
        if selector != self.selector {
            return Some(());
        }
        if path.len() >= 64 || self.nodes.len() >= 2048 || !self.active.insert(id) {
            return None;
        }
        self.nodes.insert(id);
        for target in [yes, no] {
            self.blocks.get(&target)?;
            if self.cycle_edges.contains(&(id, target)) {
                self.backward = true;
            }
        }
        let key = key as u64 & self.mask;
        path.push(Predicate {
            result,
            op,
            key,
            truth: true,
        });
        if op == Op::Eq {
            if self.cases.len() >= 512
                || self.cases.iter().any(|c| c.key == key)
                || !path.iter().all(|p| matches(key, p, self.bits))
            {
                return None;
            }
            self.cases.push(Case {
                key,
                from: id,
                to: yes,
                path: path.clone(),
            });
        } else {
            self.visit(yes, false, path)?;
        }
        path.last_mut()?.truth = false;
        self.visit(no, false, path)?;
        path.pop();
        self.active.remove(&id);
        Some(())
    }
}

pub(super) fn analyze(function: &Function) -> BTreeMap<u32, Dispatch> {
    let blocks = function.blocks.iter().map(|b| (b.id, b)).collect();
    let cycle_edges = super::cycles::checkpoint_edges(function);
    let types: BTreeMap<_, _> = function
        .parameters
        .iter()
        .cloned()
        .chain(
            function
                .blocks
                .iter()
                .flat_map(|b| &b.instructions)
                .filter_map(result_type),
        )
        .collect();
    let mut claimed = BTreeSet::new();
    let mut plans = BTreeMap::new();
    for block in &function.blocks {
        if claimed.contains(&block.id) {
            continue;
        }
        let Some((_, _, selector @ Value::Temp(id), _, _, _)) = condition(block, true) else {
            continue;
        };
        let Some(ty) = types.get(&id).filter(|ty| ty.is_integer()) else {
            continue;
        };
        let bits = width(ty);
        let mut search = Search {
            blocks: &blocks,
            selector: selector.clone(),
            bits,
            mask: u64::MAX >> (64 - bits),
            nodes: BTreeSet::new(),
            active: BTreeSet::new(),
            cases: Vec::new(),
            backward: false,
            cycle_edges: &cycle_edges,
        };
        if search.visit(block.id, true, &mut Vec::new()).is_some() && search.cases.len() >= 8 {
            claimed.extend(search.nodes);
            plans.insert(
                block.id,
                Dispatch {
                    selector,
                    bits,
                    cases: search.cases,
                    backward: search.backward,
                },
            );
        }
    }
    plans
}

impl Cg<'_, '_> {
    /// Build fast case edges, then leave the builder in the miss block so the
    /// caller emits its original conditional branch. Restore each skipped
    /// predicate's value before selecting the original predecessor's phi edge.
    pub(super) fn dispatch(&mut self, plan: &Dispatch) -> Result<(), String> {
        let slow = self.b.create_block();
        let mut switch = Switch::new();
        let mut cases = Vec::new();
        for case in &plan.cases {
            let bridge = self.b.create_block();
            switch.set_entry(u128::from(case.key), bridge);
            cases.push((bridge, case));
        }
        let selector = self.value(&plan.selector)?;
        let selector = self.mask(selector, plan.bits);
        switch.emit(&mut self.b, selector, slow);
        for (bridge, case) in cases {
            self.b.switch_to_block(bridge);
            for p in &case.path {
                let value = self.k(i64::from(p.truth));
                self.define(p.result, value)?;
            }
            self.runtime("rcc_native_poll", &[])?;
            let target = self.edge(case.from, case.to)?;
            self.b.ins().jump(target, &[]);
        }
        self.b.switch_to_block(slow);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::IrType;

    fn sample() -> Function {
        let mut f = Function::new("dispatch".into(), IrType::I16);
        f.parameters = vec![(1, IrType::I32)];
        for key in 0..8u32 {
            let mut block = BasicBlock::new(key);
            block.instructions = vec![
                I::Binary {
                    result: key + 2,
                    op: Op::Eq,
                    lhs: Value::Temp(1),
                    rhs: Value::Constant(key.into()),
                    result_type: IrType::I16,
                },
                I::BranchCond {
                    condition: Value::Temp(key + 2),
                    true_label: key + 20,
                    false_label: key + 1,
                },
            ];
            f.blocks.push(block);
            let mut target = BasicBlock::new(key + 20);
            target
                .instructions
                .push(I::Return(Some(Value::Constant(key.into()))));
            f.blocks.push(target);
        }
        let mut miss = BasicBlock::new(8);
        miss.instructions.push(I::Return(Some(Value::Constant(99))));
        f.blocks.push(miss);
        f
    }

    #[test]
    fn recognizes_dispatch_but_never_skips_a_mutating_comparison_block() {
        let mut f = sample();
        let plans = analyze(&f);
        assert_eq!(plans.len(), 1);
        assert_eq!(plans[&0].cases.len(), 8);
        assert_eq!(plans[&0].cases[7].path.len(), 8);
        f.blocks
            .iter_mut()
            .find(|b| b.id == 4)
            .unwrap()
            .instructions
            .insert(
                0,
                I::Store {
                    ptr: Value::Temp(1),
                    value: Value::Constant(1),
                    value_type: IrType::I16,
                    volatile: false,
                },
            );
        assert!(
            analyze(&f).is_empty(),
            "a store ends the pure dispatch subtree"
        );
    }
    #[test]
    fn emitted_known_keys_take_one_poll_and_misses_keep_the_original_path() {
        use rcc_ir::{ir::Module, target::Target};
        use std::process::Command;
        let dir = std::env::temp_dir().join(format!(
            "native-dispatch-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        struct Cleanup(std::path::PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = std::fs::remove_dir_all(&self.0);
            }
        }
        let _cleanup = Cleanup(dir.clone());
        let mut module = Module::new("dispatch-check".into());
        module.target = Target::Native;
        let mut function = sample();
        function.blocks.retain(|b| b.id < 20);
        for block in &mut function.blocks {
            if let Some(I::BranchCond { true_label, .. }) = block.instructions.last_mut() {
                *true_label = 20;
            }
        }
        // All cases join through phi nodes. Check both the original leaf
        // predecessor and skipped true/false predicate values, rather than
        // merely returning case constants from separate leaf blocks.
        let mut join = BasicBlock::new(20);
        for (result, incoming) in [
            (
                100,
                (0..8).map(|i| (Value::Constant(i.into()), i)).collect(),
            ),
            (101, (0..8).map(|i| (Value::Temp(i + 2), i)).collect()),
            (102, (0..8).map(|i| (Value::Temp(2), i)).collect()),
        ] {
            join.instructions.push(I::Phi {
                result,
                incoming,
                result_type: IrType::I16,
            });
        }
        for (result, op, lhs, rhs) in [
            (103, Op::Mul, Value::Temp(101), Value::Constant(1000)),
            (104, Op::Mul, Value::Temp(102), Value::Constant(100)),
            (105, Op::Add, Value::Temp(100), Value::Temp(103)),
            (106, Op::Add, Value::Temp(105), Value::Temp(104)),
        ] {
            join.instructions.push(I::Binary {
                result,
                op,
                lhs,
                rhs,
                result_type: IrType::I16,
            });
        }
        join.instructions.push(I::Return(Some(Value::Temp(106))));
        function.blocks.push(join);
        module.add_function(function);
        let mut main = Function::new("main".into(), IrType::I64);
        let mut block = BasicBlock::new(0);
        block
            .instructions
            .push(I::Return(Some(Value::Function("dispatch".into()))));
        main.blocks.push(block);
        module.add_function(main);
        // Also exercise resumable dispatch/phi emission for the independent
        // x64 target. Host execution below is an oracle only for the host ISA.
        let mut cross = module.clone();
        cross
            .functions
            .iter_mut()
            .find(|f| f.name == "main")
            .unwrap()
            .parameters
            .push((1, IrType::I32));
        let (elf, _) =
            crate::compile_freestanding_application(&cross, "x86_64-unknown-none").unwrap();
        assert_eq!(&elf[..6], b"\x7fELF\x02\x01");
        assert_eq!(&elf[18..20], &[62, 0]);
        let object = dir.join("probe.o");
        std::fs::write(&object, crate::compile_object(&module).unwrap()).unwrap();
        let source = dir.join("oracle.c");
        std::fs::write(
            &source,
            r#"
            #include <stdint.h>
            static unsigned polls;
            extern uint64_t rcc_user_dispatch(void *, uint64_t);
            uint64_t rcc_native_poll(void *ctx) { (void)ctx; ++polls; return 0; }
            uint64_t rcc_native_register(void *ctx,uint64_t key,uint64_t address) {
                (void)ctx;(void)key;(void)address;return 0;
            }
            uint64_t rcc_native_start(uint64_t entry) {
                (void)entry;
                for (unsigned i=0;i<8;++i) {
                    polls=0;
                    if(rcc_user_dispatch(0,i)!=1000+i+(i==0?100:0) || polls!=1) return 1;
                }
                polls=0;
                if(rcc_user_dispatch(0,99)!=99 || polls!=8) return 2;
                return 0;
            }
        "#,
        )
        .unwrap();
        let executable = dir.join("probe");
        let mut cc = Command::new("cc");
        cc.args(["-std=c11", "-O2"])
            .arg(source)
            .arg(object)
            .arg("-o")
            .arg(&executable);
        if cfg!(target_os = "macos") {
            let sdk = Command::new("xcrun")
                .args(["--sdk", "macosx", "--show-sdk-path"])
                .output()
                .unwrap();
            cc.arg("-isysroot")
                .arg(String::from_utf8(sdk.stdout).unwrap().trim());
        }
        let result = cc.output().unwrap();
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        assert!(Command::new(executable).status().unwrap().success());
    }
}
