use rcc_frontend::{Frontend, Type, TopLevelItem};
use rcc_frontend::ir::{Instruction, Module};

fn memory_counts(module: &Module, name: &str) -> (usize, usize, usize, usize) {
    let function = module.functions.iter().find(|f| f.name == name).unwrap();
    let mut counts = (0, 0, 0, 0);
    for instruction in function.blocks.iter().flat_map(|b| &b.instructions) {
        match instruction {
            Instruction::Load { volatile: true, .. } => counts.0 += 1,
            Instruction::Store { volatile: true, .. } => counts.1 += 1,
            Instruction::Load { volatile: false, .. } => counts.2 += 1,
            Instruction::Store { volatile: false, .. } => counts.3 += 1,
            _ => {}
        }
    }
    counts
}

#[test]
fn volatile_qualifier_belongs_to_the_correct_declarator_layer() {
    let ast = Frontend::parse_source(r#"
        volatile unsigned long first;
        unsigned volatile long second;
        unsigned long volatile third;
        volatile int *target;
        int *volatile object;
        volatile int *volatile both;
        volatile static int storage;
    "#).unwrap();
    let types: Vec<_> = ast.items.iter().flat_map(|item| match item {
        TopLevelItem::Declarations(decls) => decls.iter().map(|d| &d.decl_type).collect::<Vec<_>>(),
        _ => Vec::new(),
    }).collect();
    assert_eq!(types.len(), 7);
    for ty in &types[..3] { assert_eq!(**ty, Type::UnsignedLong.with_volatile()); }
    assert!(!types[3].is_volatile());
    assert!(types[3].pointer_target().unwrap().is_volatile());
    assert!(types[4].is_volatile());
    assert!(!types[4].pointer_target().unwrap().is_volatile());
    assert!(types[5].is_volatile());
    assert!(types[5].pointer_target().unwrap().is_volatile());
    assert!(types[6].is_volatile());
}

#[test]
fn volatile_reads_and_writes_survive_lvalue_lowering() {
    let module = Frontend::compile_to_ir(r#"
        volatile int shared;
        int ordinary;
        int globals(void) { shared = 4; return shared + shared + ordinary; }
        int local(void) { volatile int value = 1; value = 2; return value; }
        int indirect(volatile int *p, int *q) { *p = 7; return *p + *q; }
        int pointer_object(int *q) { int *volatile p = q; return *p; }
        int cast_target(int *p) { return *(volatile int *)p; }
        int choose_target(int choose, int *normal, volatile int *device) {
            return *(choose ? normal : device);
        }
        int increment(void) { return shared++; }
        int compound(void) { return shared += 3; }
        void discarded(void) { shared; (void)shared; }
    "#, "volatile_scalar").unwrap();
    assert_eq!(memory_counts(&module, "globals"), (2, 1, 1, 0));
    assert_eq!(memory_counts(&module, "local"), (1, 2, 0, 0));
    assert_eq!(memory_counts(&module, "indirect").0, 1);
    assert_eq!(memory_counts(&module, "indirect").1, 1);
    assert!(memory_counts(&module, "indirect").2 >= 3);
    assert_eq!(memory_counts(&module, "pointer_object").0, 1);
    assert_eq!(memory_counts(&module, "pointer_object").1, 1);
    assert_eq!(memory_counts(&module, "cast_target").0, 1);
    assert_eq!(memory_counts(&module, "choose_target").0, 1);
    assert_eq!(memory_counts(&module, "increment"), (1, 1, 0, 0));
    assert_eq!(memory_counts(&module, "compound"), (1, 1, 0, 0));
    assert_eq!(memory_counts(&module, "discarded"), (2, 0, 0, 0));
}

#[test]
fn typedef_arrays_and_aggregate_members_retain_volatile() {
    let module = Frontend::compile_to_ir(r#"
        typedef volatile int Register;
        typedef int Registers[2];
        Register named;
        volatile Registers array;
        struct Device { int value; int words[2]; };
        volatile struct Device device;
        struct Mixed { volatile int status; int ordinary; } mixed;
        int typedef_read(void) { return named; }
        int array_read(void) { return array[1]; }
        int member_read(void) { return device.value + device.words[1]; }
        int pointer_member(volatile struct Device *p) { return p->value; }
        int mixed_read(void) { return mixed.status + mixed.ordinary; }
        void member_write(void) { device.value = 3; device.words[1] = 4; }
        int initialized(void) { volatile struct Device d = {1, {2, 3}}; return d.value; }
        void copy_out(struct Device *out) { *out = device; }
        void copy_in(struct Device *in) { device = *in; }
        int consume(struct Device value) { return value.value; }
        int pass_aggregate(void) { return consume(device); }
        struct Device return_aggregate(void) { return device; }
    "#, "volatile_aggregates").unwrap();
    assert_eq!(memory_counts(&module, "typedef_read").0, 1);
    assert_eq!(memory_counts(&module, "array_read").0, 1);
    assert_eq!(memory_counts(&module, "member_read").0, 2);
    assert_eq!(memory_counts(&module, "pointer_member").0, 1);
    assert_eq!(memory_counts(&module, "mixed_read"), (1, 0, 1, 0));
    assert_eq!(memory_counts(&module, "member_write").1, 2);
    assert_eq!(memory_counts(&module, "initialized").1, 3);
    assert_eq!(memory_counts(&module, "copy_out").0, 3);
    assert_eq!(memory_counts(&module, "copy_out").1, 0);
    assert_eq!(memory_counts(&module, "copy_in").0, 0);
    assert_eq!(memory_counts(&module, "copy_in").1, 3);
    assert_eq!(memory_counts(&module, "pass_aggregate").0, 3);
    assert_eq!(memory_counts(&module, "return_aggregate").0, 3);
}

#[test]
fn memory_instruction_serialization_preserves_volatile_and_reads_legacy_ir() {
    let module = Frontend::compile_to_ir("volatile int x; int f(void) { x = 1; return x; }", "volatile_json").unwrap();
    for instruction in module.functions[0].blocks.iter().flat_map(|b| &b.instructions) {
        let kind = match instruction {
            Instruction::Load { volatile: true, .. } => "Load",
            Instruction::Store { volatile: true, .. } => "Store",
            _ => continue,
        };
        let mut serialized = serde_json::to_value(instruction).unwrap();
        let restored: Instruction = serde_json::from_value(serialized.clone()).unwrap();
        assert_eq!(&restored, instruction);
        serialized[kind].as_object_mut().unwrap().remove("volatile");
        let legacy: Instruction = serde_json::from_value(serialized).unwrap();
        assert!(matches!(legacy,
            Instruction::Load { volatile: false, .. } | Instruction::Store { volatile: false, .. }));
    }
}
