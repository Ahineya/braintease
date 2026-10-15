fn run_math(body: &str) {
    let source = format!(
        "{}\nfn close(a: f64, b: f64) -> bool {{ a == a && fabs(a-b) <= 2.0e-10 }}\nfn main() -> i16 {{ {body} 0 }}",
        include_str!("../std/math.rfl")
    );
    let result = rfl::run_program(&source).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(result.status, 0, "{}", result.stdout);
}

#[test]
fn trigonometry_reduces_audio_and_large_angles() {
    let mut body = String::new();
    let mut values = vec![
        0.0,
        -0.0,
        1.0e-300,
        -1.0e-300,
        0.7853981633974483,
        -0.7853981633974483,
        1.0,
        -1.0,
        std::f64::consts::FRAC_PI_2,
        -std::f64::consts::FRAC_PI_2,
        std::f64::consts::PI,
        1048575.9999999999,
        1048576.0,
        -1048576.0,
        1.0e10,
        -1.0e10,
        1.0e20,
        -1.0e20,
        1.0e100,
        -1.0e100,
        1.0e300,
        -1.0e300,
        f64::MAX,
        -f64::MAX,
    ];
    // Quadrant boundaries throughout the entire beep input range, and neighbors.
    for quadrant in [1, 2, 3, 4, 15, 127, 1023, 1759] {
        let x = f64::from(quadrant) * std::f64::consts::FRAC_PI_2;
        values.extend([
            f64::from_bits(x.to_bits() - 1),
            x,
            f64::from_bits(x.to_bits() + 1),
        ]);
    }
    for chunk in values.chunks(8) {
        body.clear();
        for (i, x) in chunk.iter().enumerate() {
            body.push_str(&format!(
                "if (!close(sin({x:.17e}), {:.17e})) {{ return {}; }}\n\
             if (!close(cos({x:.17e}), {:.17e})) {{ return {}; }}\n",
                x.sin(),
                i * 2 + 1,
                x.cos(),
                i * 2 + 2
            ));
        }
        run_math(&body);
    }
    body.clear();
    body.push_str("if (1.0 / sin(-0.0) != -1.0 / 0.0) { return 199; }\n");
    // Ordered comparisons with NaN are false; check NaN explicitly too.
    body.push_str("let nan = sin(1.0 / 0.0); if (nan == nan) { return 200; }\n");
    body.push_str("let nan2 = cos(0.0 / 0.0); if (nan2 == nan2) { return 201; }\n");
    run_math(&body);
}

#[test]
fn square_root_is_correctly_rounded_across_binary64_range() {
    let values = [
        0.0_f64,
        1.0,
        2.0,
        3.0,
        4.0,
        9.0,
        1.0e-300,
        1.0e300,
        f64::MAX,
        f64::MIN_POSITIVE,
        f64::from_bits(1),
        f64::from_bits(2),
        f64::from_bits(3),
        f64::from_bits((1 << 52) - 1),
        f64::from_bits(0x3ff0000000000001),
    ];
    let mut body = String::new();
    for (i, x) in values.iter().enumerate() {
        body.push_str(&format!(
            "if (sqrt({x:.17e}) != {:.17e}) {{ return {}; }}\n",
            x.sqrt(),
            i + 1
        ));
    }
    body.push_str("if (sqrt(1.0 / 0.0) != 1.0 / 0.0) { return 30; }\n");
    body.push_str("if (1.0 / sqrt(-0.0) != -1.0 / 0.0) { return 31; }\n");
    body.push_str("let nan = sqrt(-1.0); if (nan == nan) { return 32; }\n");
    run_math(&body);
}

#[test]
fn floor_ceil_and_round_handle_wide_values_ties_and_signed_zero() {
    let values = [
        0.0_f64,
        -0.0,
        0.49,
        -0.49,
        0.5,
        -0.5,
        0.49999999999999994,
        -0.49999999999999994,
        0.5000000000000001,
        1.5,
        -1.5,
        2147483647.5,
        -2147483648.5,
        3000000000.5,
        -3000000000.5,
        4503599627370495.5,
        4503599627370497.0,
        -4503599627370497.0,
        1.0e100,
    ];
    let mut body = String::new();
    for (i, x) in values.iter().enumerate() {
        for (j, (name, want)) in [
            ("floor", x.floor()),
            ("ceil", x.ceil()),
            ("round", x.round()),
        ]
        .iter()
        .enumerate()
        {
            body.push_str(&format!(
                "if ({name}({x:.17e}) != {want:.17e}) {{ return {}; }}\n",
                i * 3 + j + 1
            ));
        }
    }
    for (i, call) in ["floor(-0.0)", "ceil(-0.1)", "round(-0.1)", "round(-0.0)"]
        .iter()
        .enumerate()
    {
        body.push_str(&format!(
            "if (1.0 / {call} != -1.0 / 0.0) {{ return {}; }}\n",
            i + 100
        ));
    }
    body.push_str("if (1.0 / fabs(-0.0) != 1.0 / 0.0) { return 110; }\n");
    run_math(&body);
}
