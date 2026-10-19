#[path = "../examples/heap.rs"]
mod heap;
fn main() {
    assert_eq!(heap::check(), 0);
    println!("Rust host reference: heap checks passed");
}
