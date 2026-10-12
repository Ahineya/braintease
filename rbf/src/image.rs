//! Versioned transport emitted by `rlink --bf-host` and preserved by `bfm`.
use rvm::devices::GuestMemory;
use serde::Serialize;

pub const LANES: usize = 8;
pub const RAM_LANE: usize = 4;
pub const ORIGIN: usize = 8;

#[derive(Clone, Debug, Serialize)]
pub struct Layout {
    pub version: u16,
    pub bank_size: u16,
    pub memory_banks: usize,
    pub instructions: usize,
    pub entry: usize,
    pub origin: usize,
    pub tape_cells: usize,
}

impl Layout {
    /// A header is mandatory: an arbitrary BF file must never gain MMIO hooks.
    pub fn parse(source: &str, max_tape_cells: usize) -> Result<(Self, &str), String> {
        let source = source.trim_start();
        let end = source.find("END_BFHOST").ok_or(
            "Missing BFHOST header; link with rlink -f macro --standalone --bf-host, then expand with bfm",
        )?;
        let words: Vec<_> = source[..end].split_whitespace().collect();
        let names = [
            "BFHOST",
            "BANK_SIZE",
            "MEMORY_BANKS",
            "INSTRUCTIONS",
            "ENTRY",
            "ORIGIN",
            "TAPE_CELLS",
        ];
        if words.len() != 14
            || names
                .iter()
                .enumerate()
                .any(|(i, name)| words[2 * i] != *name)
        {
            return Err("Malformed BFHOST header".into());
        }
        let mut values = Vec::new();
        for pair in words.chunks_exact(2) {
            values.push(
                pair[1]
                    .parse::<usize>()
                    .map_err(|_| format!("Invalid {}", pair[0]))?,
            );
        }
        if values[0] != 1 {
            return Err(format!("Unsupported BFHOST version {}", values[0]));
        }
        let bank_size = u16::try_from(values[1]).map_err(|_| "BANK_SIZE exceeds 65535")?;
        if values[5] != ORIGIN {
            return Err("BFHOST version 1 requires ORIGIN 8".into());
        }
        let layout = Self {
            version: 1,
            bank_size,
            memory_banks: values[2],
            instructions: values[3],
            entry: values[4],
            origin: values[5],
            tape_cells: values[6],
        };
        layout.validate(max_tape_cells)?;
        let body = &source[end + "END_BFHOST".len()..];
        if body
            .bytes()
            .any(|b| !b.is_ascii_whitespace() && !b"+-<>[],.?!$".contains(&b))
        {
            return Err("Unexpected text after BFHOST header; use the expanded .bf file".into());
        }
        if !body.contains('$') || !body.contains('?') || !body.contains('!') {
            return Err("BFHOST image is missing its CPU/device hooks".into());
        }
        Ok((layout, body))
    }

    pub fn validate(&self, max_tape_cells: usize) -> Result<(), String> {
        if self.version != 1 || self.origin != ORIGIN {
            return Err("Unsupported BFHOST layout".into());
        }
        if self.bank_size < 32 {
            return Err("BANK_SIZE must be at least 32".into());
        }
        if !(1..=65536).contains(&self.memory_banks) {
            return Err("MEMORY_BANKS must be in 1..65536".into());
        }
        // Reserve a representable PC/PCB pair for the appended implicit HALT.
        let code_capacity = usize::from(self.bank_size)
            .checked_mul(65536)
            .ok_or("Code capacity overflow")?;
        if self.instructions == 0
            || self.instructions >= code_capacity
            || self.entry >= self.instructions
        {
            return Err("BFHOST requires a nonempty code image, an entry within it, and space for the implicit HALT in 16-bit code banks".into());
        }
        let code_words = self
            .instructions
            .checked_mul(4)
            .and_then(|words| words.checked_add(76))
            .ok_or("Code tape size overflow")?;
        let min_tape = self
            .memory_cells()
            .max(code_words)
            .checked_add(33)
            .and_then(|words| words.checked_mul(LANES))
            .ok_or("Tape size overflow")?;
        if self.tape_cells < min_tape || self.tape_cells > max_tape_cells {
            return Err(format!("TAPE_CELLS {} must be at least {min_tape} and at most {max_tape_cells}; adjust --max-tape-cells for larger images", self.tape_cells));
        }
        Ok(())
    }

    pub fn memory_cells(&self) -> usize {
        self.memory_banks * usize::from(self.bank_size)
    }

    pub fn ram_address(&self, pointer: usize) -> Result<usize, String> {
        let relative = pointer
            .checked_sub(self.origin)
            .ok_or("Device hook points into leading guard")?;
        if relative % LANES != RAM_LANE || relative / LANES >= self.memory_cells() {
            return Err(format!("Device hook at invalid RAM tape cell {pointer}"));
        }
        Ok(relative / LANES)
    }
}

/// Direct view of the RAM lane. Descriptors never copy the whole BF tape.
pub struct StridedMemory<'a> {
    tape: &'a mut [u16],
    cells: usize,
}

impl<'a> StridedMemory<'a> {
    pub fn new(tape: &'a mut [u16], cells: usize) -> Self {
        assert!(tape.len() >= ORIGIN && cells <= (tape.len() - ORIGIN) / LANES);
        Self { tape, cells }
    }
}

impl GuestMemory for StridedMemory<'_> {
    fn len(&self) -> usize {
        self.cells
    }
    fn read(&self, index: usize) -> u16 {
        assert!(index < self.cells);
        self.tape[ORIGIN + index * LANES + RAM_LANE]
    }
    fn write(&mut self, index: usize, value: u16) {
        assert!(index < self.cells);
        self.tape[ORIGIN + index * LANES + RAM_LANE] = value;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const HEADER: &str = "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 2312 END_BFHOST\n$?!";
    #[test]
    fn header_is_strict_and_resource_bounded() {
        let (layout, body) = Layout::parse(HEADER, 2312).unwrap();
        assert_eq!(layout.memory_cells(), 256);
        assert_eq!(body.trim(), "$?!");
        assert_eq!(layout.ram_address(ORIGIN + 8 * 255 + 4), Ok(255));
        assert!(layout.ram_address(ORIGIN + 8 * 256 + 4).is_err());
        assert!(layout.ram_address(8 * 32 + 1).is_err());
        for text in [
            HEADER.replace("BFHOST 1", "BFHOST 2"),
            HEADER.replace("SIZE 64", "SIZE 0"),
            HEADER.replace("ENTRY 0", "ENTRY 1"),
            HEADER.replace("ORIGIN 8", "ORIGIN 0"),
            HEADER.replace("CELLS 2312", "CELLS 32"),
            HEADER.replace("$?!", "$"),
            format!("{HEADER} comment"),
        ] {
            assert!(Layout::parse(&text, 2312).is_err(), "{text}");
        }
        assert!(Layout::parse(HEADER, 1000).is_err());
        assert!(Layout::parse("+++.", 2048).is_err());
        // Code-bank capacity counts instructions, not their four-cell encoding.
        let non_power_of_two = "BFHOST 1 BANK_SIZE 65 MEMORY_BANKS 4 INSTRUCTIONS 24 ENTRY 5 ORIGIN 8 TAPE_CELLS 2344 END_BFHOST $?!";
        assert!(Layout::parse(non_power_of_two, 2344).is_ok());
    }
    #[test]
    fn memory_view_only_changes_ram_lane() {
        let mut tape = vec![91; 2312];
        let mut memory = StridedMemory::new(&mut tape, 256);
        memory.write(255, 42);
        assert_eq!(memory.read(255), 42);
        assert_eq!(tape[ORIGIN + 2043], 91);
        assert_eq!(tape[ORIGIN + 2045], 91);
    }

    #[test]
    fn multiple_code_banks_and_later_entry_are_valid() {
        let header = "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 129 ENTRY 128 ORIGIN 8 TAPE_CELLS 5000 END_BFHOST $?!";
        let (layout, _) = Layout::parse(header, 5000).unwrap();
        assert_eq!(layout.instructions, 129);
        assert_eq!(layout.entry, 128);
        assert!(Layout::parse(&header.replace("CELLS 5000", "CELLS 4999"), 5000).is_err());
        for count in [64 * 65536, usize::MAX] {
            assert!(Layout::parse(
                &header.replace("INSTRUCTIONS 129", &format!("INSTRUCTIONS {count}")),
                usize::MAX
            )
            .is_err());
        }
    }
}
