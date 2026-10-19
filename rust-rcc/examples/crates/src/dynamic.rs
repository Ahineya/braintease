use alloc::{boxed::Box, vec::Vec};
use core::cell::Cell;

trait Left {
    fn left(&self) -> u32;
}
trait Right {
    fn right(&self) -> u32;
}
trait Fetch {
    type Item;
    fn fetch(&mut self, input: Self::Item) -> Option<Self::Item>;
}
trait Calculate: Left + Right {
    fn apply(&self, a: u64, b: u32) -> (u64, u32);
    fn bump(&mut self, value: u32);
    fn default_method(&self) -> u32 {
        self.left() + self.right()
    }
}
struct First(u32);
struct Second(u32);
impl Fetch for First {
    type Item = u32;
    fn fetch(&mut self, input: u32) -> Option<u32> {
        Some(self.0 + input)
    }
}
impl Left for First {
    fn left(&self) -> u32 {
        self.0
    }
}
impl Right for First {
    fn right(&self) -> u32 {
        self.0 + 7
    }
}
impl Calculate for First {
    fn apply(&self, a: u64, b: u32) -> (u64, u32) {
        (a + self.0 as u64, b ^ self.0)
    }
    fn bump(&mut self, value: u32) {
        self.0 += value;
    }
}
impl Left for Second {
    fn left(&self) -> u32 {
        self.0 * 3
    }
}
impl Right for Second {
    fn right(&self) -> u32 {
        self.0 * 5
    }
}
impl Calculate for Second {
    fn apply(&self, a: u64, b: u32) -> (u64, u32) {
        (a ^ self.0 as u64, b + self.0)
    }
    fn bump(&mut self, value: u32) {
        self.0 += value * 2;
    }
}
fn transform(a: u64, b: u32) -> (u64, u32) {
    (((a << 16) | (a >> 48)), b.wrapping_mul(13))
}
fn other(a: u64, b: u32) -> (u64, u32) {
    (a ^ 0x123456789abcdef0, b.wrapping_add(7))
}
static FUNCTIONS: [fn(u64, u32) -> (u64, u32); 2] = [transform, other];
static OBJECT: First = First(23);
static STATIC_DYN: &(dyn Calculate + Sync) = &OBJECT;

pub fn probe(output: &mut Vec<u8>) -> Result<(), u16> {
    let mut first = First(17);
    let mut second = Second(31);
    for choice in 0..6u32 {
        let object: &mut dyn Calculate = if choice % 2 == 0 {
            &mut first
        } else {
            &mut second
        };
        object.bump(choice);
        let (a, b) = object.apply(0xfedcba9876543210, choice * 30001);
        output.extend_from_slice(&a.to_le_bytes());
        output.extend_from_slice(&b.to_le_bytes());
        output.extend_from_slice(&object.default_method().to_le_bytes());
        let right: &dyn Right = object;
        output.extend_from_slice(&right.right().to_le_bytes());
        if core::mem::size_of_val(right) != 4 || core::mem::align_of_val(right) != 4 {
            return Err(40);
        }
    }
    output.extend_from_slice(&STATIC_DYN.default_method().to_le_bytes());
    let method: fn(&(dyn Calculate + 'static)) -> u32 = <dyn Calculate>::default_method;
    output.extend_from_slice(&method(STATIC_DYN).to_le_bytes());
    let fetch: &mut dyn Fetch<Item = u32> = &mut first;
    output.extend_from_slice(&fetch.fetch(7).ok_or(49u16)?.to_le_bytes());
    for (index, function) in FUNCTIONS.iter().enumerate() {
        let (a, b) = function(0x1234567887654321, index as u32);
        output.extend_from_slice(&a.to_le_bytes());
        output.extend_from_slice(&b.to_le_bytes());
    }
    let closure: fn(u32) -> u32 = |x| x.wrapping_add(0xf0000000);
    output.extend_from_slice(&closure(0x12345678).to_le_bytes());
    const OPTIONAL: Option<fn(u64, u32) -> (u64, u32)> = Some(transform);
    let (a, b) = OPTIONAL.ok_or(48u16)?(0x1234567887654321, 9);
    output.extend_from_slice(&a.to_le_bytes());
    output.extend_from_slice(&b.to_le_bytes());
    let captured = 0x10203040u32;
    let call = |x: u32| x ^ captured;
    let dynamic: &dyn Fn(u32) -> u32 = &call;
    output.extend_from_slice(&dynamic(0xfedcba98).to_le_bytes());
    let mut value = 7u32;
    let mut call = |delta| {
        value += delta;
        value
    };
    let dynamic: &mut dyn FnMut(u32) -> u32 = &mut call;
    output.extend_from_slice(&dynamic(13).to_le_bytes());
    output.extend_from_slice(&dynamic(17).to_le_bytes());
    if value != 37 {
        return Err(41);
    }

    struct Resource<'a>(&'a Cell<u32>, u32);
    impl Left for Resource<'_> {
        fn left(&self) -> u32 {
            self.1
        }
    }
    impl Drop for Resource<'_> {
        fn drop(&mut self) {
            self.0.set(self.0.get() + self.1);
        }
    }
    let drops = Cell::new(0);
    {
        let object: Box<dyn Left> = Box::new(Resource(&drops, 19));
        if object.left() != 19
            || core::mem::size_of_val(&*object) != core::mem::size_of::<Resource<'_>>()
        {
            return Err(42);
        }
        drop(object);
        let no_drop: Box<dyn Left> = Box::new(First(1));
        drop(no_drop);
    }
    if drops.get() != 19 {
        return Err(43);
    }
    output.extend_from_slice(&drops.get().to_le_bytes());
    Ok(())
}

#[cfg(target_os = "fuzix")]
pub fn blocking() -> u16 {
    trait Reader {
        fn read(&mut self, bytes: &mut [u8]) -> Result<usize, u16>;
        fn calls(&self) -> u32;
        fn bump(&mut self);
    }
    struct Console(u32);
    impl Reader for Console {
        fn read(&mut self, bytes: &mut [u8]) -> Result<usize, u16> {
            let count = rustcell::io::read_stdin(bytes).map_err(|_| 50u16)?;
            self.0 += 1;
            Ok(count)
        }
        fn calls(&self) -> u32 {
            self.0
        }
        fn bump(&mut self) {
            self.0 += 1;
        }
    }
    fn read(reader: &mut dyn Reader, bytes: &mut [u8]) -> Result<usize, u16> {
        reader.read(bytes)
    }
    fn fork() -> i16 {
        unsafe { rustcell_fork() }
    }
    unsafe extern "C" {
        fn rustcell_fork() -> i16;
        fn rustcell_wait(pid: i16) -> i16;
    }
    let mut reader: Box<dyn Reader> = Box::new(Console(0));
    let mut bytes = alloc::vec![0x33u8; 8];
    let callback: fn(&mut dyn Reader, &mut [u8]) -> Result<usize, u16> = read;
    if rustcell::io::write_stdout(b"DYNAMIC_WAITING\n").is_err() {
        return 51;
    }
    if callback(&mut *reader, &mut bytes[..1]) != Ok(1) || bytes[0] != b'D' || reader.calls() != 1 {
        return 52;
    }
    if rustcell::io::write_stdout(b"DYNAMIC_IO_OK\n").is_err() {
        return 53;
    }
    let launch: fn() -> i16 = fork;
    let pid = launch();
    if pid == 0 {
        bytes[2] = 0xf0;
        reader.bump();
        if reader.calls() != 2 {
            return 54;
        }
        let _ = rustcell::io::write_stdout(b"DYNAMIC_CHILD_OK\n");
        return 37;
    }
    if pid < 0 || unsafe { rustcell_wait(pid) } != 37 || reader.calls() != 1 || bytes[2] != 0x33 {
        return 55;
    }
    if rustcell::io::write_stdout(b"DYNAMIC_FORK_OK\n").is_err() {
        return 56;
    }
    0
}
