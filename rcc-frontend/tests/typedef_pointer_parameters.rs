use rcc_frontend::Frontend;

#[test]
fn register_parameters_accept_typedefs_and_arrow_access() {
    Frontend::compile_to_ir(r#"
        typedef struct node { int value; struct node *next; } node, *nodeptr;
        typedef nodeptr handle;
        int value(register handle p) { return p->value + p->next->value; }
        int main(void) { node a; node b; a.value=7; b.value=11; a.next=&b; return value(&a); }
    "#, "typedef_pointer.c").expect("standard C register parameters and pointer typedefs must lower");
}
