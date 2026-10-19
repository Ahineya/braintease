use alloc::vec::Vec;
#[cfg(feature = "dynamic")]
#[path = "dynamic.rs"]
mod dynamic;
#[cfg(all(feature = "dynamic", target_os = "fuzix"))]
pub fn blocking() -> u16 {
    dynamic::blocking()
}

pub fn run() -> Result<Vec<u8>, u16> {
    let mut output = Vec::new();
    constants_probe(&mut output)?;
    #[cfg(feature = "dynamic")]
    dynamic::probe(&mut output)?;
    #[cfg(feature = "byteorder")]
    byteorder_probe(&mut output)?;
    #[cfg(feature = "heapless")]
    heapless_probe(&mut output)?;
    #[cfg(feature = "postcard")]
    postcard_probe(&mut output)?;
    Ok(output)
}

fn constants_probe(output: &mut Vec<u8>) -> Result<(), u16> {
    #[repr(C, align(16))]
    struct Tail<T: ?Sized> {
        prefix: u8,
        data: T,
    }
    #[repr(C)]
    struct Outer<T: ?Sized> {
        header: u32,
        tail: T,
    }
    let mut wrapped = Outer {
        header: 123,
        tail: Tail {
            prefix: 9,
            data: [7u16, 0xabcd, 0xff80],
        },
    };
    let view: &mut Outer<Tail<[u16]>> = &mut wrapped;
    view.tail.data[1] = 0x1234;
    if core::mem::size_of_val(view) != core::mem::size_of::<Outer<Tail<[u16; 3]>>>()
        || core::mem::align_of_val(view) != 16
        || view.tail.prefix != 9
        || view.header != 123
        || view.tail.data[1] != 0x1234
    {
        return Err(33);
    }
    output.extend_from_slice(&view.tail.data[1].to_le_bytes());
    static WORDS: [u32; 3] = [0x12345678, 0xabcdef01, 0x87654321];
    static MIDDLE: &u32 = &WORDS[1];
    const LAST: Option<&u32> = Some(&WORDS[2]);
    static TEXTS: [&str; 3] = ["constant", "\u{3bb}\u{1f680}", ""];
    static TAIL: &[u8] = b"\0\xffABC".split_at(1).1;
    struct Node {
        value: u8,
        next: &'static Node,
    }
    static A: Node = Node { value: 7, next: &B };
    static B: Node = Node { value: 9, next: &A };
    #[repr(align(512))]
    struct Aligned([u8; 3]);
    static ALIGNED: Aligned = Aligned([1, 128, 255]);
    if *MIDDLE != WORDS[1]
        || LAST != Some(&WORDS[2])
        || !core::ptr::eq(A.next.next, &A)
        || (&ALIGNED as *const Aligned as usize) % 512 != 0
    {
        return Err(30);
    }
    let end = unsafe { WORDS.as_ptr().add(3) };
    if unsafe { end.offset_from(WORDS.as_ptr()) } != 3 {
        return Err(31);
    }
    output.extend_from_slice(&MIDDLE.to_le_bytes());
    output.extend_from_slice(&LAST.ok_or(32u16)?.to_be_bytes());
    output.extend_from_slice(TAIL);
    for text in TEXTS.iter() {
        output.extend_from_slice(text.as_bytes());
    }
    output.push(A.value);
    output.push(A.next.value);
    output.extend_from_slice(&ALIGNED.0);
    Ok(())
}

#[cfg(feature = "postcard")]
fn postcard_probe(output: &mut Vec<u8>) -> Result<(), u16> {
    use alloc::string::String;
    use serde::Serialize;
    #[derive(Serialize, PartialEq)]
    #[cfg_attr(feature = "postcard-derived", derive(serde::Deserialize))]
    enum Event {
        Idle,
        Position { x: i32, y: i32 },
        Text(String),
        Data(Vec<u8>),
    }
    #[derive(Serialize, PartialEq)]
    #[cfg_attr(feature = "postcard-derived", derive(serde::Deserialize))]
    struct Record<'a> {
        sequence: u32,
        signed: i64,
        enabled: bool,
        label: Option<&'a str>,
        payload: &'a [u8],
        event: Event,
    }
    let payload: Vec<u8> = (0..37u16).map(|i| (i * 29 + 128) as u8).collect();
    for i in 0..32u32 {
        let event = match i % 4 {
            0 => Event::Idle,
            1 => Event::Position {
                x: -(i as i32) * 70000,
                y: i as i32 * 10001,
            },
            2 => Event::Text(String::from("hello \u{3bb}\u{1f680}")),
            _ => Event::Data(payload.clone()),
        };
        let record = Record {
            sequence: i.wrapping_mul(0x1234567),
            signed: if i == 0 {
                i64::MIN
            } else if i == 1 {
                i64::MAX
            } else {
                -(i as i64) * 123456789
            },
            enabled: i % 2 == 0,
            label: if i % 3 == 0 {
                None
            } else {
                Some("binary \u{e9}")
            },
            payload: &payload[..i as usize],
            event,
        };
        let bytes = postcard::to_allocvec(&record).map_err(|_| 20u16)?;
        #[cfg(feature = "postcard-derived")]
        {
            let decoded: Record<'_> = postcard::from_bytes(&bytes).map_err(|_| 21u16)?;
            if decoded != record {
                return Err(22);
            }
            if postcard::from_bytes::<Record<'_>>(&bytes[..bytes.len() - 1]).is_ok() {
                return Err(23);
            }
        }
        output.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
        output.extend_from_slice(&bytes);
        let scalar = postcard::to_allocvec(&record.signed).map_err(|_| 25u16)?;
        if postcard::from_bytes::<i64>(&scalar).map_err(|_| 26u16)? != record.signed {
            return Err(27);
        }
    }
    // A boolean has only two valid encodings; malformed input must be rejected.
    if postcard::from_bytes::<bool>(&[2]).is_ok() || postcard::from_bytes::<u32>(&[0x80; 6]).is_ok()
    {
        return Err(24);
    }
    #[cfg(feature = "postcard-derived")]
    {
        let bad_variant = postcard::from_bytes::<Event>(&[4]).err().ok_or(44u16)?;
        let bad_utf8 = postcard::from_bytes::<&str>(&[1, 255]).err().ok_or(45u16)?;
        output.extend_from_slice(&postcard::to_allocvec(&bad_variant).map_err(|_| 46u16)?);
        output.extend_from_slice(&postcard::to_allocvec(&bad_utf8).map_err(|_| 47u16)?);
    }
    Ok(())
}

#[cfg(feature = "heapless")]
fn heapless_probe(output: &mut Vec<u8>) -> Result<(), u16> {
    let mut values = heapless::Vec::<u32, 8>::new();
    for i in 0..8u32 {
        if values.push(i * 0x1234567 + 0x80000000).is_err() {
            return Err(10);
        }
    }
    if values.push(99) != Err(99) || values.len() != 8 {
        return Err(11);
    }
    output.extend_from_slice(&values.remove(2).to_le_bytes());
    output.extend_from_slice(&values.swap_remove(1).to_le_bytes());
    if values.insert(1, 0xfedcba98).is_err() {
        return Err(12);
    }
    values.truncate(4);
    for value in &values {
        output.extend_from_slice(&value.to_le_bytes());
    }
    let mut queue = heapless::Deque::<u16, 5>::new();
    for i in 0..5u16 {
        if queue.push_back(i + 0xff00).is_err() {
            return Err(13);
        }
    }
    if queue.push_front(3) != Err(3)
        || queue.pop_front() != Some(0xff00)
        || queue.pop_front() != Some(0xff01)
        || queue.push_back(0x1234).is_err()
        || queue.push_back(0x5678).is_err()
    {
        return Err(14);
    }
    let (a, b) = queue.as_slices();
    if a.is_empty() || b.is_empty() {
        return Err(15); // Deliberately exercise the wrapped ring buffer.
    }
    while let Some(value) = queue.pop_front() {
        output.extend_from_slice(&value.to_be_bytes());
    }
    let mut text = heapless::String::<24>::new();
    if text.push_str("hello \u{3bb}\u{1f680}").is_err()
        || text.pop() != Some('\u{1f680}')
        || text.push('\u{e9}').is_err()
        || text.insert(0, '>').is_err()
    {
        return Err(16);
    }
    output.extend_from_slice(text.as_bytes());
    let saved = text.clone();
    if text
        .push_str("this does not fit inside the remaining space")
        .is_ok()
        || text != saved
    {
        return Err(17);
    }
    // Removing, truncating and dropping an inline collection must destroy
    // each owned element exactly once, in the same order as ordinary Rust.
    use core::cell::Cell;
    struct Token<'a>(&'a Cell<u32>, u8);
    impl Drop for Token<'_> {
        fn drop(&mut self) {
            self.0.set(self.0.get() * 10 + self.1 as u32);
        }
    }
    let drops = Cell::new(0u32);
    {
        let mut owned = heapless::Vec::<Token<'_>, 4>::new();
        for id in 1..=4u8 {
            if owned.push(Token(&drops, id)).is_err() {
                return Err(18);
            }
        }
        drop(owned.remove(1));
        owned.truncate(1);
    }
    if drops.get() != 2341 {
        return Err(19);
    }
    output.extend_from_slice(&drops.get().to_le_bytes());
    #[repr(C)]
    struct OwnedTail<'a, T: ?Sized> {
        owner: Token<'a>,
        data: T,
    }
    let mut storage = core::mem::ManuallyDrop::new(OwnedTail {
        owner: Token(&drops, 5),
        data: [1u8, 128, 255],
    });
    let view: &mut OwnedTail<'_, [u8]> = &mut *storage;
    if view.data[2] != 255 {
        return Err(34);
    }
    // Dropping the DST must give its sized owner a thin pointer. ManuallyDrop
    // prevents the original sized binding from destroying the owner again.
    unsafe {
        core::ptr::drop_in_place(view);
    }
    if drops.get() != 23415 {
        return Err(35);
    }
    output.extend_from_slice(&drops.get().to_le_bytes());
    Ok(())
}

#[cfg(feature = "byteorder")]
fn byteorder_probe(output: &mut Vec<u8>) -> Result<(), u16> {
    use byteorder::{BigEndian, ByteOrder, LittleEndian};
    macro_rules! checked {
        ($ty:ty) => {
            for (a, b) in [
                (<$ty>::MIN, -1),
                (<$ty>::MIN, 1),
                (<$ty>::MIN, 0),
                (<$ty>::MAX, 2),
                (<$ty>::MAX, 1),
                (0, <$ty>::MIN),
                (-3, -7),
                (-<$ty>::MAX, 2),
                (1, <$ty>::MIN),
                (-1, <$ty>::MIN),
                (127, 127),
                (-128, -128),
            ] {
                let (value, overflow) = a.overflowing_mul(b);
                output.extend_from_slice(&value.to_le_bytes());
                output.push(overflow as u8);
                output.extend_from_slice(&a.swap_bytes().to_le_bytes());
            }
        };
    }
    checked!(i8);
    checked!(i16);
    checked!(i32);
    checked!(i64);
    // Odd offsets, high bits, signed values and every supported integer width.
    // Runtime inputs prevent rustc from simply folding the entire probe away.
    let input: Vec<u8> = (0..131u32).map(|i| (i * 73 + 129) as u8).collect();
    for i in 0..120usize {
        let mut bytes = [0u8; 17];
        let value = BigEndian::read_u16(&input[i..]);
        LittleEndian::write_u16(&mut bytes[1..], value);
        if LittleEndian::read_u16(&bytes[1..]) != value {
            return Err(1);
        }
        let signed = LittleEndian::read_i32(&input[i..]);
        BigEndian::write_i32(&mut bytes[3..], signed);
        if BigEndian::read_i32(&bytes[3..]) != signed {
            return Err(2);
        }
        let wide = BigEndian::read_u64(&input[i..]);
        LittleEndian::write_u64(&mut bytes[7..], wide);
        if LittleEndian::read_u64(&bytes[7..]) != wide {
            return Err(3);
        }
        output.extend_from_slice(&bytes);
        // Variable-width encodings exercise byte swapping and sign extension.
        for width in 1..=8usize {
            let value = LittleEndian::read_uint(&input[i..], width);
            BigEndian::write_uint(&mut bytes, value, width);
            if BigEndian::read_uint(&bytes, width) != value {
                return Err(4);
            }
            output.extend_from_slice(&bytes[..width]);
            let value = BigEndian::read_int(&input[i..], width);
            LittleEndian::write_int(&mut bytes, value, width);
            if LittleEndian::read_int(&bytes, width) != value {
                return Err(5);
            }
            output.extend_from_slice(&bytes[..width]);
        }
    }
    Ok(())
}
