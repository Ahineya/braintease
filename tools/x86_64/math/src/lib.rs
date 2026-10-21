#![no_std]
mod scalar;

#[cfg(not(test))]
extern "C" {
    fn unsupported_service(message: *const u8) -> !;
}

#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { unsupported_service(b"freestanding scalar math panic\0".as_ptr()) }
}

#[cfg(not(test))]
#[no_mangle]
pub extern "C" fn rcc_native_ext___rcc_host_math(_: usize, op: u64, a: u64, b: u64) -> u64 {
    if let Ok(operation) = u16::try_from(op) {
        if let Ok(value) = scalar::evaluate(operation, a, b) {
            return value;
        }
    }
    unsafe { unsupported_service(b"invalid freestanding scalar math request\0".as_ptr()) }
}

// Run inside the actual relocated kernel before admitting its first math client.
#[cfg(not(test))]
#[no_mangle]
pub extern "C" fn riplix_math_selftest() -> u64 {
    let probes = [
        (0x301, 81f64.to_bits(), 0, 9f64.to_bits()),
        (0x306, 0, 0, 0),
        (0x307, 0, 0, 1f64.to_bits()),
        (0x319, 2f64.to_bits(), 10f64.to_bits(), 1024f64.to_bits()),
        (0x31b, 3f64.to_bits(), 4f64.to_bits(), 5f64.to_bits()),
        (0x204, 0, 0, 0x7ff8000000000000),
        (0x30a, 1f64.to_bits(), 0, 0),
    ];
    for (op, a, b, want) in probes {
        if scalar::evaluate(op, a, b) != Ok(want) {
            return 0;
        }
    }
    1
}

#[cfg(test)]
mod tests {
    use super::scalar::evaluate;
    #[test]
    fn ieee_and_libm_contract() {
        assert_eq!(evaluate(0, 0, 0), Ok(7));
        assert_eq!(evaluate(0x204, 0, 0), Ok(0x7ff8000000000000));
        assert_eq!(evaluate(0x20b, f64::INFINITY.to_bits(), 0), Ok(u64::MAX));
        assert_eq!(evaluate(0x20a, f64::NAN.to_bits(), 0), Ok(0));
        assert_eq!(evaluate(0x301, 4f64.to_bits(), 0), Ok(2f64.to_bits()));
        assert_eq!(
            evaluate(0x319, 2f64.to_bits(), 10f64.to_bits()),
            Ok(1024f64.to_bits())
        );
        assert_eq!(
            evaluate(0x31b, 3f64.to_bits(), 4f64.to_bits()),
            Ok(5f64.to_bits())
        );
        assert_eq!(evaluate(0x306, (-0f64).to_bits(), 0), Ok((-0f64).to_bits()));
        assert_eq!(evaluate(0x31f, 0, (-0f64).to_bits()), Ok((-0f64).to_bits()));
        assert_eq!(evaluate(0x321, 2.5f64.to_bits(), 0), Ok(2f64.to_bits()));
        for code in 1..=33 {
            assert!(evaluate(0x300 + code, 0, 0).is_ok());
        }
        for (op, a, b) in [
            (0, 1, 0),
            (0x101, 1 << 32, 0),
            (0x20d, 1 << 32, 0),
            (0x301, 0, 1),
            (0xffff, 0, 0),
        ] {
            assert!(evaluate(op, a, b).is_err());
        }
    }
}
