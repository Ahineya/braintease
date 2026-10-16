use ripple_asm::{AssemblerOptions, Linker, Opcode, RippleAssembler};

fn assemble(source: &str) -> ripple_asm::ObjectFile {
    RippleAssembler::new(AssemblerOptions::default())
        .assemble(source)
        .unwrap()
}

#[test]
fn one_function_can_exceed_a_code_bank() {
    let object = assemble(&format!("large:\n{}HALT\n", "NOP\n".repeat(40)));
    let image = Linker::new(16).link(vec![object]).unwrap();
    assert_eq!(image.instructions.len(), 41);
    assert_eq!(image.code_bank_count(), 3);
}

#[test]
fn symbolic_branches_beyond_the_object_immediate_range_reach_the_linker() {
    let object = assemble(&format!(
        "large:\nBEQ R0, R0, L_end\n{}L_end:\nBNE A0, R0, large\nHALT\n",
        "NOP\n".repeat(70000)
    ));
    assert_eq!(object.unresolved_references[&0].label, "L_end");
    assert_eq!(object.unresolved_references[&70001].label, "large");
    let image = Linker::new(64000).link(vec![object]).unwrap();
    assert_eq!(image.instructions[0].opcode, Opcode::Jal as u8);
    assert_eq!(image.instructions[0].word1, 0);
    let end = image.labels["L_end"].absolute_address as usize / 4;
    assert_eq!(
        (image.instructions[0].word2, image.instructions[0].word3),
        ((end / 64000) as u16, (end % 64000) as u16)
    );
}

#[test]
fn same_bank_control_flow_keeps_its_original_encoding() {
    let object = assemble("start:\nLI A0, 2\nL_loop:\nADDI A0, A0, -1\nBNE A0, R0, L_loop\nBEQ R0, R0, L_end\nNOP\nL_end:\nHALT\n");
    let original = object.instructions.clone();
    assert_eq!(
        Linker::new(16).link(vec![object]).unwrap().instructions,
        original
    );
}

#[test]
fn relaxation_converges_with_many_crossings_and_boundary_padding() {
    for bank in [3, 8, 37, 65] {
        let mut source = String::from("start:\n");
        for i in 0..80 {
            source.push_str(&format!(
                "L_site_{i}:\nBNE A0, R0, L_site_{}\nNOP\n",
                (i + 39) % 80
            ));
        }
        source.push_str("HALT\n");
        let object = assemble(&source);
        let refs = object.unresolved_references.clone();
        let image = Linker::new(bank).link(vec![object]).unwrap();
        for (index, reference) in refs {
            let site = image.labels[&format!("L_site_{}", index / 2)].absolute_address as usize / 4;
            let target = image.labels[&reference.label].absolute_address as usize / 4;
            let instruction = image.instructions[site];
            if instruction.opcode == Opcode::Bne as u8 {
                assert_eq!(site / bank as usize, target / bank as usize);
                assert_eq!(instruction.word3, (target as i64 - site as i64) as u16);
            } else {
                assert_eq!(instruction.opcode, Opcode::Beq as u8);
                assert_eq!(instruction.word3, 2);
                assert_eq!(site / bank as usize, (site + 2) / bank as usize);
                let jump = image.instructions[site + 1];
                assert_eq!((jump.opcode, jump.word1), (Opcode::Jal as u8, 0));
                assert_eq!(
                    usize::from(jump.word2) * bank as usize + usize::from(jump.word3),
                    target
                );
            }
        }
    }
}

#[test]
fn unconditional_and_impossible_comparisons_do_not_need_two_instructions() {
    for (op, opcode) in [
        ("BEQ", Opcode::Jal),
        ("BGE", Opcode::Jal),
        ("BNE", Opcode::Bne),
        ("BLT", Opcode::Blt),
    ] {
        let source = format!(
            "start:\n{op} A0, A0, L_end\n{}L_end:\nHALT\n",
            "NOP\n".repeat(40)
        );
        let image = Linker::new(8).link(vec![assemble(&source)]).unwrap();
        assert_eq!(image.instructions.len(), 42);
        assert_eq!(image.instructions[0].opcode, opcode as u8);
        if opcode == Opcode::Jal {
            assert_eq!(image.instructions[0].word1, 0);
        }
    }
}

#[test]
fn symbolic_cross_object_branches_are_relocated_without_linking() {
    let entry = assemble("start:\nBEQ A0, A1, target\nHALT\n");
    let pad = assemble(&format!("padding:\n{}HALT\n", "NOP\n".repeat(7)));
    let target = assemble("target:\nHALT\n");
    let image = Linker::new(8).link(vec![entry, pad, target]).unwrap();
    assert_eq!(image.instructions[0].opcode, Opcode::Bne as u8);
    let jump = image.instructions[1];
    assert_eq!((jump.opcode, jump.word1), (Opcode::Jal as u8, 0));
    assert_eq!(
        usize::from(jump.word2) * 8 + usize::from(jump.word3),
        image.labels["target"].absolute_address as usize / 4
    );
}

#[test]
fn expanding_a_small_segment_also_protects_its_boundary_call() {
    let entry =
        assemble("start:\nBNE A0, A1, callee\nNOP\nNOP\nNOP\nNOP\nNOP\nL_call:\nCALL callee\n");
    let padding = assemble("padding:\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nHALT\n");
    let callee = assemble("callee:\nRET\n");
    let image = Linker::new(8).link(vec![entry, padding, callee]).unwrap();
    let call = image.labels["L_call"].absolute_address as usize / 4;
    assert_eq!(
        call % 8,
        0,
        "inverse branch expansion would otherwise put this call at offset 7"
    );
    assert_eq!(image.instructions[call].opcode, Opcode::Jal as u8);
    assert_eq!(image.instructions[call].word1, 3);
}

#[test]
fn reachability_collection_preserves_wide_branch_targets() {
    let entry = assemble("start:\nCALL main\nHALT\n");
    let mut body = assemble(&format!("main:\nCALL large\nRET\ndead:\nCALL missing\nRET\nlarge:\nBEQ R0, R0, L_end\n{}L_end:\nRET\n", "NOP\n".repeat(80)));
    body.rcc = Some(ripple_asm::types::CompilerMetadata {
        code: vec!["main".into(), "dead".into(), "large".into()],
        ..Default::default()
    });
    let image = Linker::new(16).link(vec![entry, body]).unwrap();
    assert!(!image.labels.contains_key("dead"));
    let large = image.labels["large"].absolute_address as usize / 4;
    let end = image.labels["L_end"].absolute_address as usize / 4;
    let jump = image.instructions[large];
    assert_eq!(jump.opcode, Opcode::Jal as u8);
    assert_eq!(usize::from(jump.word2) * 16 + usize::from(jump.word3), end);
}

#[test]
fn numeric_branches_that_expansion_would_invalidate_fail_explicitly() {
    let object = assemble("start:\nBNE A0, R0, 5\nBEQ A0, A1, L_end\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nNOP\nL_end:\nHALT\n");
    let error = Linker::new(8).link(vec![object]).unwrap_err();
    assert!(error
        .iter()
        .any(|e| e.contains("numeric branch") && e.contains("symbolic label")));
}

#[test]
fn explicitly_banked_numeric_jumps_are_not_mistaken_for_local_labels() {
    let object = assemble("start:\nJAL RA, 3, 1\nHALT\n");
    let image = Linker::new(16).link(vec![object]).unwrap();
    assert_eq!(
        (image.instructions[0].word2, image.instructions[0].word3),
        (3, 1)
    );
}

#[test]
fn raw_binary_exports_reject_branches_that_require_linking() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    assert!(assembler
        .assemble_to_binary("start:\nBNE A0, R0, start\nHALT\n")
        .is_ok());
    let error = assembler
        .assemble_to_binary(&format!(
            "start:\nBEQ R0, R0, L_end\n{}L_end:\nHALT\n",
            "NOP\n".repeat(40)
        ))
        .unwrap_err();
    assert!(error[0].contains("rlink"));
}

#[test]
fn direct_macro_exports_link_before_formatting() {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    let output = assembler
        .assemble_to_macro(&format!(
            "start:\nBEQ R0, R0, L_end\n{}L_end:\nHALT\n",
            "NOP\n".repeat(40)
        ))
        .unwrap();
    assert!(output.contains("@OP_JAL"));
    assert!(assembler
        .assemble_to_macro("start:\nCALL missing\nHALT\n")
        .unwrap_err()[0]
        .contains("missing"));
    assert!(ripple_asm::format_to_macro(&assemble("start:\nCALL missing\nHALT\n")).is_err());
}

#[test]
fn undefined_and_duplicate_targets_remain_errors() {
    assert!(Linker::new(8)
        .link(vec![assemble("start:\nBNE A0, R0, missing\nHALT\n")])
        .unwrap_err()[0]
        .contains("Undefined label"));
    assert!(Linker::new(8)
        .link(vec![
            assemble("duplicate:\nHALT\n"),
            assemble("duplicate:\nHALT\n")
        ])
        .unwrap_err()[0]
        .contains("Duplicate label"));
    let object = assemble(".data\ndata: .space 1\n.code\nstart:\nBEQ A0, A1, data\nHALT\n");
    assert!(Linker::new(8).link(vec![object]).unwrap_err()[0].contains("code label"));
}
