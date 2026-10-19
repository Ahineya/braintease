#[path = "support/fp.rs"]
mod support;

#[test]
fn omitted_return_types_follow_generic_tails_instead_of_guessing_i16() {
    support::parity(
        r#"
import {share,with_shared,Shared} from "std/shared";
fn identity<T>(value:T)->T{value}
fn text(){identity("inferred")}
fn store(slot:Shared<i16>){
 match Some(42){
  Some(value)=>with_shared(slot,once fn(view:&mut i16)=>{*view=value;}),
  None=>{},
 }
}

fn wide(){let value:u32=70000;identity(value)}
fn optional()->Option<i16>{Some(42)}
fn inferred_option(){let value=optional()?;optional()}
fn main(){let slot=share(0);store(slot);println(text());println(wide());
 with_shared(slot,once fn(view:&mut i16)=>{println(*view);});println(inferred_option());}
"#,
        "inferred\n70000\n42\nSome(42)\n",
    );
}

#[test]
fn generic_callbacks_receive_solved_inputs_before_inferring_their_output() {
    support::parity(
        include_str!("fixtures/callback-inference.rfl"),
        "Some(42)\n",
    );
}

#[test]
fn solved_owned_struct_and_wide_inputs_preserve_moves_and_declared_types() {
    support::parity(
        r#"
 import {map,map_once} from "std/fp";
 struct Point{value:u32}
 fn main(){
 println(map(Some(Point{value:70000}),fn(p)=>p.value+1));
 let wide:u32=70000;println(map(Some(wide),fn(n)=>n+1));
 let value=map_once(Some(owned(42,fn(n:&mut i16)=>{println("closed");})),once fn(n)=>*n.borrow());println(value);
 }"#,
        "Some(70001)\nSome(70001)\nclosed\nSome(42)\n",
    );
    let f = support::Fixture::new(
        "import {map} from \"std/fp\";fn main(){let wide:u32=70000;map(Some(wide),fn(n:i16)=>n);}",
    );
    let error = rfl::check_file(&f.0).unwrap_err();
    assert!(error.detail.contains("expected"), "{error}");
}

#[test]
fn known_element_types_contextualize_unary_variants_empty_values_and_vectors() {
    support::parity(
        include_str!("fixtures/callback-source-context.rfl"),
        "Some(70001)\nNone\n[70001]\n",
    );
}

#[test]
fn inferred_results_still_validate_main_and_explicit_callback_types() {
    for source in [
        "fn id<T>(value:T)->T{value} fn main(){id(\"wrong main result\")}",
        "fn id<T>(value:T)->T{value} fn bad()->i16{id(\"wrong result\")} fn main(){}",
        "fn accept<T>(f:fn()->T)->T{f()} fn main(){let n:i16=accept(fn()=>{});}",
    ] {
        let fixture = support::Fixture::new(source);
        assert!(rfl::check_file(&fixture.0).is_err(), "accepted {source}");
    }
}
