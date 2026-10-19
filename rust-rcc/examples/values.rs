//! The same portable Rust checks run with the host compiler and inside both guests.
//! Calls are kept out of line so matches, moves and drops execute at runtime.
#![allow(dead_code)]

#[derive(Clone, Copy)]
enum Message {
    Empty,
    Byte(u8),
    Pair { left: u16, right: u32 },
}

#[inline(never)]
fn message(which: u8, value: u16) -> Message {
    match which {
        0 => Message::Empty,
        1 => Message::Byte(value as u8),
        _ => Message::Pair {
            left: value,
            right: 0x12345678,
        },
    }
}

#[inline(never)]
fn score(value: Message) -> u32 {
    match value {
        Message::Empty => 7,
        Message::Byte(byte) => byte as u32,
        Message::Pair { left, right } => right ^ left as u32,
    }
}

#[repr(i16)]
enum Tagged {
    Negative = -7,
    Byte(u8) = 5,
    Word(u16) = 300,
}

enum Signed {
    Negative = -1,
    Zero = 0,
    Positive = 1,
}
#[inline(never)]
fn signed(which: u8) -> Signed {
    match which {
        0 => Signed::Negative,
        1 => Signed::Zero,
        _ => Signed::Positive,
    }
}
#[inline(never)]
fn signed_score(value: Signed) -> i32 {
    match value {
        Signed::Negative => -17,
        Signed::Zero => 0,
        Signed::Positive => 19,
    }
}

#[inline(never)]
fn nonzero(value: u64) -> Option<core::num::NonZeroU64> {
    core::num::NonZeroU64::new(value)
}
#[inline(never)]
fn nonzero_score(value: Option<core::num::NonZeroU64>) -> u64 {
    match value {
        Some(value) => value.get(),
        None => 7,
    }
}

#[inline(never)]
fn tagged(which: u8) -> Tagged {
    match which {
        0 => Tagged::Negative,
        1 => Tagged::Byte(255),
        _ => Tagged::Word(0xabcd),
    }
}

#[inline(never)]
fn tagged_score(value: Tagged) -> u16 {
    match value {
        Tagged::Negative => 10,
        Tagged::Byte(v) => v as u16,
        Tagged::Word(v) => v,
    }
}

#[inline(never)]
fn maybe(value: u16) -> Option<u16> {
    if value == 0 { None } else { Some(value) }
}

#[inline(never)]
fn checked(value: u16) -> Result<u16, u8> {
    let value = maybe(value).ok_or(9u8)?;
    Ok(value ^ 0x0100)
}

#[inline(never)]
fn bool_option(which: u8) -> Option<Option<bool>> {
    match which {
        0 => None,
        1 => Some(None),
        2 => Some(Some(false)),
        _ => Some(Some(true)),
    }
}

#[inline(never)]
fn bool_score(value: Option<Option<bool>>) -> u16 {
    match value {
        None => 1,
        Some(None) => 2,
        Some(Some(false)) => 3,
        Some(Some(true)) => 4,
    }
}

#[inline(never)]
fn reference<'a>(some: bool, value: &'a u16) -> Option<&'a u16> {
    if some { Some(value) } else { None }
}

#[inline(never)]
fn reference_score(value: Option<&u16>) -> u16 {
    match value {
        Some(value) => *value,
        None => 11,
    }
}

#[inline(never)]
fn text(some: bool) -> Option<&'static str> {
    if some { Some("rust") } else { None }
}

#[inline(never)]
fn text_length(value: Option<&str>) -> usize {
    match value {
        Some(value) => value.len(),
        None => 0,
    }
}

#[repr(C)]
struct Slot<'a> {
    prefix: u16,
    reference: &'a u16,
}

#[inline(never)]
fn slot<'a>(some: bool, value: &'a u16) -> Option<Slot<'a>> {
    if some {
        Some(Slot {
            prefix: 17,
            reference: value,
        })
    } else {
        None
    }
}

#[inline(never)]
fn slot_score(value: Option<Slot<'_>>) -> u16 {
    match value {
        Some(value) => value.prefix ^ *value.reference,
        None => 19,
    }
}

enum Flag {
    Red,
    Green,
    Value(bool),
}
#[inline(never)]
fn flag(which: u8) -> Flag {
    match which {
        0 => Flag::Red,
        1 => Flag::Green,
        2 => Flag::Value(false),
        _ => Flag::Value(true),
    }
}
#[inline(never)]
fn flag_score(value: Flag) -> u16 {
    match value {
        Flag::Red => 10,
        Flag::Green => 20,
        Flag::Value(false) => 30,
        Flag::Value(true) => 40,
    }
}

pub fn enums() -> u16 {
    if score(message(0, 99)) != 7
        || score(message(1, 0x1234)) != 0x34
        || score(message(2, 0x4321)) != 0x12341559
    {
        return 1;
    }
    if tagged_score(tagged(0)) != 10
        || tagged_score(tagged(1)) != 255
        || tagged_score(tagged(2)) != 0xabcd
    {
        return 2;
    }
    if maybe(17).map(|v| v + 1).unwrap_or(3) != 18 || maybe(0).unwrap_or(3) != 3 {
        return 3;
    }
    match checked(0x1234) {
        Ok(0x1334) => {}
        _ => return 4,
    }
    match checked(0) {
        Err(9) => {}
        _ => return 5,
    }
    if bool_score(bool_option(0)) != 1
        || bool_score(bool_option(1)) != 2
        || bool_score(bool_option(2)) != 3
        || bool_score(bool_option(3)) != 4
    {
        return 6;
    }
    let number = 0x8765;
    if reference_score(reference(true, &number)) != number
        || reference_score(reference(false, &number)) != 11
    {
        return 7;
    }
    if core::mem::size_of::<Option<&u16>>() != core::mem::size_of::<&u16>()
        || core::mem::size_of::<Option<bool>>() != 1
    {
        return 8;
    }
    if text_length(text(true)) != 4 || text_length(text(false)) != 0 {
        return 9;
    }
    let increment = number & 15;
    if maybe(17).map(|v| v + increment).unwrap_or(0) != 22 {
        return 10;
    }
    if slot_score(slot(true, &number)) != number ^ 17 || slot_score(slot(false, &number)) != 19 {
        return 11;
    }
    if flag_score(flag(0)) != 10
        || flag_score(flag(1)) != 20
        || flag_score(flag(2)) != 30
        || flag_score(flag(3)) != 40
    {
        return 12;
    }
    if core::mem::discriminant(&message(1, 11)) != core::mem::discriminant(&message(1, 99))
        || core::mem::discriminant(&message(1, 11)) == core::mem::discriminant(&message(0, 11))
    {
        return 13;
    }
    if signed_score(signed(0)) != -17
        || signed_score(signed(1)) != 0
        || signed_score(signed(2)) != 19
    {
        return 14;
    }
    if nonzero_score(nonzero(0x123456789abcdef0)) != 0x123456789abcdef0
        || nonzero_score(nonzero(0)) != 7
    {
        return 15;
    }
    let mut total = 0u16;
    {
        let mut add = |value: u16| {
            total += value;
            total
        };
        if add(2) != 2 || add(3) != 5 {
            return 16;
        }
    }
    if total != 5 {
        return 17;
    }
    0
}

struct Resource<'a> {
    state: &'a mut u32,
    digit: u32,
}
impl Drop for Resource<'_> {
    #[inline(never)]
    fn drop(&mut self) {
        *self.state = *self.state * 10 + self.digit;
    }
}

struct Pair<'a> {
    first: Resource<'a>,
    second: Resource<'a>,
}
enum Owned<'a> {
    Empty,
    Full(Resource<'a>),
}

#[inline(never)]
fn owned<'a>(some: bool, state: &'a mut u32) -> Owned<'a> {
    if some {
        Owned::Full(Resource { state, digit: 6 })
    } else {
        Owned::Empty
    }
}

#[inline(never)]
fn early(state: &mut u32) -> Result<u16, u8> {
    let _guard = Resource { state, digit: 8 };
    let value = checked(0)?;
    Ok(value)
}

#[inline(never)]
fn moved(value: Resource<'_>) {
    core::mem::drop(value);
}

struct Ordered<'a> {
    state: &'a core::cell::Cell<u32>,
    digit: u32,
}
impl Drop for Ordered<'_> {
    #[inline(never)]
    fn drop(&mut self) {
        self.state.set(self.state.get() * 10 + self.digit);
    }
}

// A zero-sized value still has observable drop glue. The reference runner and
// guest harness both check this event, so simply skipping ZSTs cannot pass.
unsafe extern "C" {
    fn rustcell_report(value: u16);
}
struct ZeroSized;
impl Drop for ZeroSized {
    #[inline(never)]
    fn drop(&mut self) {
        unsafe {
            rustcell_report(901);
        }
    }
}

pub fn drops() -> u16 {
    let mut first = 0;
    let mut second = 0;
    {
        let _pair = Pair {
            first: Resource {
                state: &mut first,
                digit: 1,
            },
            second: Resource {
                state: &mut second,
                digit: 2,
            },
        };
    }
    if first != 1 || second != 2 {
        return 1;
    }
    {
        let resource = Resource {
            state: &mut first,
            digit: 3,
        };
        moved(resource);
    }
    if first != 13 {
        return 2;
    }
    core::mem::drop(owned(false, &mut first));
    if first != 13 {
        return 3;
    }
    core::mem::drop(owned(true, &mut first));
    if first != 136 {
        return 4;
    }
    if !early(&mut second).is_err() || second != 28 {
        return 5;
    }
    {
        let mut value = Some(Resource {
            state: &mut first,
            digit: 4,
        });
        let taken = value.take();
        core::mem::drop(value);
        core::mem::drop(taken);
    }
    if first != 1364 {
        return 6;
    }
    {
        let _array = [
            Resource {
                state: &mut first,
                digit: 5,
            },
            Resource {
                state: &mut second,
                digit: 6,
            },
        ];
    }
    if first != 13645 || second != 286 {
        return 7;
    }
    let state = core::cell::Cell::new(0);
    {
        let _tuple = (
            Ordered {
                state: &state,
                digit: 1,
            },
            Ordered {
                state: &state,
                digit: 2,
            },
        );
    }
    if state.get() != 12 {
        return 8;
    }
    {
        let _first = Ordered {
            state: &state,
            digit: 3,
        };
        let _second = Ordered {
            state: &state,
            digit: 4,
        };
    }
    if state.get() != 1243 {
        return 9;
    }
    {
        let tuple = (
            Ordered {
                state: &state,
                digit: 5,
            },
            Ordered {
                state: &state,
                digit: 6,
            },
        );
        let first = tuple.0;
        core::mem::drop(first);
    }
    if state.get() != 124356 {
        return 10;
    }
    core::mem::forget(Ordered {
        state: &state,
        digit: 7,
    });
    if state.get() != 124356 {
        return 11;
    }
    core::mem::drop(ZeroSized);
    0
}

/// Keep a guard in a Rust caller while the callback blocks in the guest kernel.
/// Its drop must run exactly once when execution resumes.
pub fn with_drop<F: FnOnce() -> isize>(action: F) -> isize {
    let state = core::cell::Cell::new(0);
    let result;
    {
        let _guard = Ordered {
            state: &state,
            digit: 7,
        };
        result = action();
    }
    if state.get() != 7 {
        return -100;
    }
    result
}
