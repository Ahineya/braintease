//! Stream large host images without constructing a macro AST for every word.
//!
//! Only tape initialization is generated here. The executor is expanded from
//! the same CPU templates as the macro export and still executes ordinary BF.
use crate::{linker::LinkedProgram, macro_formatter::CPU_FOOTER, MacroFormatter};
use bf_macro_expander::{MacroExpander, MacroExpanderOptions};
use std::{collections::HashMap, io::Write};

const IMAGE: &str = "RIPPLEIMAGEINITIALIZATION";
const CLEANUP: &str = "RIPPLEINITIALIZATIONCLEANUP";
const ORIGINAL_CLEANUP: &str = "{if(#PROGRAM_WORDS, {repeat(#PROGRAM_WORDS, @clear @prevword)}, @clear_to_zero_left_lane @nextword)}";

impl MacroFormatter {
    /// Write expanded host BF with compact initialization and bounded exporter
    /// memory. Validation and CPU-template expansion finish before any output.
    pub fn write_linked_program_host_bf<W: Write>(
        &self,
        program: &LinkedProgram,
        memory_banks: u16,
        writer: &mut W,
    ) -> Result<(), String> {
        self.validate_linked_program_host(program, memory_banks)?;
        if CPU_FOOTER.matches(ORIGINAL_CLEANUP).count() != 1 {
            return Err(
                "BF CPU initialization template changed; compact export needs review".into(),
            );
        }
        let mut template = self.standalone_prefix(program, true, Some(memory_banks));
        template.push_str(&format!("@prg(@nop, {{preserve({IMAGE})}})\n"));
        template.push_str(
            &CPU_FOOTER
                .replace(
                    "#define BANK_SIZE 64000",
                    &format!("#define BANK_SIZE {}", program.bank_size),
                )
                .replace(ORIGINAL_CLEANUP, &format!("{{preserve({CLEANUP})}}")),
        );
        let result = MacroExpander::new().expand(
            &template,
            MacroExpanderOptions {
                strip_comments: true,
                collapse_empty_lines: true,
                ..Default::default()
            },
        );
        if !result.errors.is_empty() {
            return Err(format!(
                "BF CPU template expansion failed: {:?}",
                result.errors
            ));
        }
        if result.expanded.matches(IMAGE).count() != 1
            || result.expanded.matches(CLEANUP).count() != 1
        {
            return Err(
                "BF CPU initialization placeholders were not preserved exactly once".into(),
            );
        }
        let (prefix, after_image) = result.expanded.split_once(IMAGE).unwrap();
        let (middle, suffix) = after_image
            .split_once(CLEANUP)
            .ok_or("BF CPU initialization placeholders are out of order")?;
        writer
            .write_all(prefix.as_bytes())
            .map_err(|e| e.to_string())?;
        write_image(program, writer).map_err(|e| e.to_string())?;
        writer
            .write_all(middle.as_bytes())
            .map_err(|e| e.to_string())?;
        // All initialized lane-zero words contain one. The origin contains the
        // setup state; the preceding guard word is zero. Clear through the
        // origin, stop on that guard and return. The next unchanged template
        // operation sets STATE_RUNNING, so its input state is immaterial.
        // No count cell means this works beyond 65535 words as well.
        writer
            .write_all(b"[[-]<<<<<<<<]>>>>>>>>")
            .map_err(|e| e.to_string())?;
        writer
            .write_all(suffix.as_bytes())
            .map_err(|e| e.to_string())?;
        writer.write_all(b"\n").map_err(|e| e.to_string())
    }
}

fn write_image<W: Write>(program: &LinkedProgram, writer: &mut W) -> std::io::Result<()> {
    let mut constants = HashMap::new();
    // program_start reserves nine four-word instructions for the registers.
    for _ in 0..36 {
        write_word(0, true, &mut constants, writer)?;
    }
    let entry = program.entry_point as usize / 4;
    for (index, instruction) in program.instructions.iter().enumerate() {
        // The original @instr starts at its opcode then writes leftwards;
        // emitting the same four words left-to-right avoids those excursions.
        for (word, value) in [
            instruction.word3,
            instruction.word2,
            instruction.word1,
            u16::from(instruction.opcode),
        ]
        .into_iter()
        .enumerate()
        {
            let fast = index < entry || (index == entry && word != 3);
            write_word(value, fast, &mut constants, writer)?;
        }
    }
    // program_end always appends one implicit HALT, with empty fast lanes.
    for _ in 0..4 {
        write_word(0, false, &mut constants, writer)?;
    }
    Ok(())
}

fn write_word<W: Write>(
    value: u16,
    fast: bool,
    constants: &mut HashMap<u16, Vec<u8>>,
    writer: &mut W,
) -> std::io::Result<()> {
    writer.write_all(b"+>>>[-]")?;
    writer.write_all(constants.entry(value).or_insert_with(|| constant(value)))?;
    // Lane 2 is zero scratch during initialization. Lane 4 is guest RAM and
    // may already contain host MMIO values; the constant builder never uses it.
    writer.write_all(if fast { b">>>>+>" } else { b">>>>>" })
}

/// Set a zero target using its zero left neighbor, restoring that scratch cell.
/// Compare literal wrapping increments/decrements with small-base Horner forms.
/// Every generated operation is ordinary, portable 16-bit Brainfuck.
fn constant(value: u16) -> Vec<u8> {
    let count = usize::from(value);
    let negative = 65536 - count;
    let mut best = if count <= negative {
        vec![b'+'; count]
    } else {
        vec![b'-'; negative]
    };
    for base in 2..=16usize {
        let mut digits = Vec::new();
        let mut remaining = count;
        while remaining != 0 {
            digits.push(remaining % base);
            remaining /= base;
        }
        let mut candidate = Vec::new();
        for (index, digit) in digits.iter().rev().enumerate() {
            if index != 0 {
                candidate.extend_from_slice(b"[<");
                candidate.extend(std::iter::repeat(b'+').take(base));
                candidate.extend_from_slice(b">-]<[>+<-]>");
            }
            candidate.extend(std::iter::repeat(b'+').take(*digit));
        }
        if candidate.len() < best.len() {
            best = candidate;
        }
    }
    best
}
