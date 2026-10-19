#[path = "../examples/values.rs"]
mod values;

static DROPS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
#[unsafe(no_mangle)]
extern "C" fn rustcell_report(value: u16) {
    assert_eq!(value, 901);
    DROPS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
}

fn main() {
    assert_eq!(values::enums(), 0);
    assert_eq!(values::drops(), 0);
    assert_eq!(DROPS.load(std::sync::atomic::Ordering::Relaxed), 1);
    println!("Rust host reference: enum and destructor checks passed");
}
