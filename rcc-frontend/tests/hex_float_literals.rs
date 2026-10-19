use rcc_frontend::lexer::{FloatSuffix, IntegerSuffix, Lexer, TokenType};

fn literal(source: &str, expected: u64, suffix: FloatSuffix) {
    let tokens = Lexer::new(source).tokenize().unwrap();
    assert_eq!(tokens.len(), 2, "{source}");
    assert_eq!(
        tokens[0].token_type,
        TokenType::FloatLiteral {
            bits: expected,
            suffix
        },
        "{source}"
    );
}

#[test]
fn hexadecimal_float_syntax_and_suffixes() {
    for (source, value) in [
        ("0x1p0", 1.0f64),
        ("0X.8P+1", 1.0),
        ("0x1.p-1", 0.5),
        ("0xA.bp2", 42.75),
        ("0x1e2p0", 482.0),
    ] {
        literal(source, value.to_bits(), FloatSuffix::None);
    }
    literal("0x1.fp+2f", 7.75f32.to_bits() as u64, FloatSuffix::Float);
    literal("0X1P0F", 1.0f32.to_bits() as u64, FloatSuffix::Float);
    literal("0x1p0L", 1.0f64.to_bits(), FloatSuffix::LongDouble);
    literal("0x1p0l", 1.0f64.to_bits(), FloatSuffix::LongDouble);
    let tokens = Lexer::new("0xffUL 0x1f 0x1p0+2").tokenize().unwrap();
    assert_eq!(
        tokens[0].token_type,
        TokenType::IntLiteral {
            value: 255,
            suffix: IntegerSuffix::UnsignedLong,
            hex: true
        }
    );
    assert_eq!(
        tokens[1].token_type,
        TokenType::IntLiteral {
            value: 31,
            suffix: IntegerSuffix::None,
            hex: true
        }
    );
    assert!(matches!(tokens[3].token_type, TokenType::Plus));
}

#[test]
fn hexadecimal_float_rounding_and_range() {
    for (source, expected) in [
        ("0x1p1008", 0x7ef0_0000_0000_0000),
        ("0x1p-1022", 0x0010_0000_0000_0000),
        ("0x1p-1074", 1),
        ("0x1p-1075", 0),
        ("0x1.0000000000000000000001p-1075", 1),
        ("0x1.fffffffffffffp-1076", 0),
        ("0x0.fffffffffffffp-1022", 0x000f_ffff_ffff_ffff),
        ("0x0.fffffffffffff8p-1022", 0x0010_0000_0000_0000),
        ("0x1.fffffffffffffp1023", 0x7fef_ffff_ffff_ffff),
        ("0x1.fffffffffffff8p1023", 0x7ff0_0000_0000_0000),
        ("0x1.00000000000008p0", 0x3ff0_0000_0000_0000),
        ("0x1.0000000000000800001p0", 0x3ff0_0000_0000_0001),
        ("0x1.00000000000018p0", 0x3ff0_0000_0000_0002),
        ("0x1000000000000000000000000p-96", 0x3ff0_0000_0000_0000),
    ] {
        literal(source, expected, FloatSuffix::None);
    }
    for (source, expected) in [
        ("0x1p-126f", 0x0080_0000),
        ("0x1p-149f", 1),
        ("0x1p-150f", 0),
        ("0x1.0000000000001p-150f", 1),
        ("0x1.fffffep127f", 0x7f7f_ffff),
        ("0x1.ffffffp127f", 0x7f80_0000),
        ("0x1.000001p0f", 0x3f80_0000),
        ("0x1.000001000000000000001p0f", 0x3f80_0001),
        ("0x1.000003p0f", 0x3f80_0002),
    ] {
        literal(source, expected, FloatSuffix::Float);
    }
    literal(
        &format!("0x0.{}1p+4004", "0".repeat(1000)),
        1.0f64.to_bits(),
        FloatSuffix::None,
    );
    let enormous = "9".repeat(100);
    literal(&format!("0x0p{enormous}"), 0, FloatSuffix::None);
    literal(&format!("0x1p-{enormous}"), 0, FloatSuffix::None);
    literal(
        &format!("0x1p{enormous}"),
        f64::INFINITY.to_bits(),
        FloatSuffix::None,
    );
}

#[test]
fn malformed_hexadecimal_floats_are_diagnosed() {
    for source in [
        "0x.p1", "0x1.2", "0x1p", "0x1p+", "0x1p-", "0x1pabc", "0x1p0u", "0x1p0ff", "0x1p0.0",
        "0x1p0p1",
    ] {
        assert!(Lexer::new(source).tokenize().is_err(), "accepted {source}");
    }
}
