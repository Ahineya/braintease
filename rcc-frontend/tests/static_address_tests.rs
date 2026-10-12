use rcc_frontend::{Frontend, BankTag, ir::Value};

#[test]
fn static_addresses_preserve_symbols_and_cell_addends() {
    let module = Frontend::compile_to_ir(r#"
        struct Item { char tag; long count; int cells[3]; };
        extern struct Item items[3];
        int matrix[3][3];
        int *member = &items[2].cells[1];
        int *arrow = (&items[2])->cells + 2;
        int *negative = &items[1].cells[2] - 2;
        int *nested = &matrix[2][1];
        int (*row)[3] = matrix + 1;
        char *cast = (char *)&items[2] - sizeof(struct Item);
        int *selected = 1 ? &matrix[1][2] : &matrix[2][0];
        char *text = "abcdef" + 2;
        struct Item items[3];
    "#, "addresses").expect("address constants must not emit function instructions");
    assert!(module.functions.is_empty());
    for (name, expected_symbol, expected_cells) in [
        ("member", "items", 16), ("arrow", "items", 17),
        ("negative", "items", 9), ("nested", "matrix", 7),
        ("row", "matrix", 3), ("cast", "items", 6),
        ("selected", "matrix", 5),
    ] {
        let value = module.globals.iter().find(|g| g.name == name).unwrap().initializer.as_ref().unwrap();
        let Value::FatPtr(pointer) = value else { panic!("{name}: {value:?}"); };
        assert_eq!(pointer.bank, BankTag::Global);
        let Value::AddressOffset { base, cells } = pointer.addr.as_ref() else { panic!("{name}: {value:?}"); };
        assert_eq!(base.as_ref(), &Value::Global(expected_symbol.into()), "{name}");
        assert_eq!(*cells, expected_cells, "{name}");
    }
    let text = module.globals.iter().find(|g| g.name == "text").unwrap().initializer.as_ref().unwrap();
    assert!(matches!(text, Value::FatPtr(pointer) if matches!(pointer.addr.as_ref(), Value::AddressOffset { base, cells: 2 } if matches!(base.as_ref(), Value::Global(_)))));
}

#[test]
fn self_references_and_aggregate_pointer_elements_are_address_constants() {
    let module = Frontend::compile_to_ir(r#"
        struct Node { int value; struct Node *next; };
        static struct Node nodes[2] = {{11, &nodes[1]}, {22, &nodes[0]}};
        static int buttons[4];
        static int *mousebuttons = &buttons[1];
        int *pointers[3] = { buttons, &buttons[2], &buttons[3] - 1 };
    "#, "self_addresses").unwrap();
    assert!(matches!(module.globals.iter().find(|g| g.name == "nodes").unwrap().initializer, Some(Value::ConstantAggregate(_))));
    assert!(matches!(module.globals.iter().find(|g| g.name == "pointers").unwrap().initializer, Some(Value::ConstantAggregate(_))));
}

#[test]
fn static_pointer_values_are_not_mistaken_for_addresses() {
    assert!(Frontend::compile_to_ir("int a; int *p = &a; int *q = p;", "invalid_address").is_err());
}
