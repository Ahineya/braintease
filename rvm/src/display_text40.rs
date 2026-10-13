//! TEXT40 terminal presentation, shared by the native and Brainfuck CPU hosts.
use crate::{constants::*, devices::GuestMemory};
use serde::{Deserialize, Serialize};
use crossterm::{
    cursor,
    event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers},
    queue,
    style::{Color, ResetColor, SetBackgroundColor, SetForegroundColor},
    terminal::{self, ClearType},
};
use std::{
    fs,
    io::{self, IsTerminal, Write},
    path::Path,
    time::Duration,
};

/// Character/attribute cells copied at a completed TEXT40 flush.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Text40Snapshot {
    pub sequence: u64,
    pub cells: [u16; TEXT40_WORDS],
}

impl Text40Snapshot {
    pub fn from_memory<M: GuestMemory + ?Sized>(memory: &M, sequence: u64) -> Self {
        let cells = std::array::from_fn(|index| {
            let address = TEXT40_BASE_WORD + index;
            if address < memory.len() {
                memory.read(address)
            } else {
                0
            }
        });
        Self { sequence, cells }
    }

    /// Match the original terminal renderer's ASCII/space/replacement policy.
    fn character(cell: u16) -> u8 {
        match cell as u8 {
            0 => b' ',
            character @ 32..=126 => character,
            _ => b'.',
        }
    }

    fn colors(cell: u16) -> (Color, Color) {
        let attr = (cell >> 8) as usize;
        let color = |index: usize| {
            let (r, g, b) = THEME_COLORS[index];
            Color::Rgb { r, g, b }
        };
        (color(attr & 15), color(attr >> 4))
    }

    pub fn text(&self) -> String {
        let mut output = String::with_capacity(TEXT40_WORDS + 25);
        for row in self.cells.chunks_exact(40) {
            output.extend(row.iter().map(|&cell| char::from(Self::character(cell))));
            output.push('\n');
        }
        output
    }

    /// Exact snapshots contain 1000 little-endian character/attribute words.
    pub fn write_raw(&self, path: &Path) -> Result<(), String> {
        if let Some(parent) = path.parent().filter(|path| !path.as_os_str().is_empty()) {
            fs::create_dir_all(parent).map_err(|error| error.to_string())?;
        }
        let bytes: Vec<u8> = self
            .cells
            .iter()
            .flat_map(|cell| cell.to_le_bytes())
            .collect();
        fs::write(path, bytes)
            .map_err(|error| format!("Writing TEXT40 snapshot {}: {error}", path.display()))
    }

    pub fn render(&self, output: &mut impl Write) -> io::Result<()> {
        queue!(output, cursor::MoveTo(0, 0))?;
        for (index, &cell) in self.cells.iter().enumerate() {
            let (foreground, background) = Self::colors(cell);
            queue!(
                output,
                SetForegroundColor(foreground),
                SetBackgroundColor(background)
            )?;
            output.write_all(&[Self::character(cell)])?;
            queue!(output, ResetColor)?;
            if index % 40 == 39 && index + 1 < TEXT40_WORDS {
                output.write_all(b"\r\n")?;
            }
        }
        output.flush()
    }
}

/// A terminal has no portable key-up events. Keep the established TEXT40
/// last-key pulse: at most ten legacy key-register reads per received event.
#[derive(Default)]
struct Text40Keys {
    key: Option<u16>,
    reads: u8,
}

impl Text40Keys {
    fn accept(&mut self, code: KeyCode) {
        self.key = match code {
            KeyCode::Up => Some(82),
            KeyCode::Down => Some(81),
            KeyCode::Left => Some(80),
            KeyCode::Right => Some(79),
            KeyCode::Char('z' | 'Z') => Some(29),
            KeyCode::Char('x' | 'X') => Some(27),
            _ => None,
        };
        self.reads = 0;
    }

    fn read(&mut self, key: u16) -> bool {
        self.reads = self.reads.saturating_add(1);
        if self.reads > 10 {
            self.key = None;
        }
        self.key == Some(key)
    }
}

/// Optional terminal ownership around the same snapshots used headlessly.
#[derive(Default)]
pub struct Text40Display {
    interactive: bool,
    active: bool,
    snapshot: Option<Text40Snapshot>,
    keys: Text40Keys,
    input: Vec<u8>,
    quit: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Text40Checkpoint {
    frame: Option<(u64, Vec<u16>)>,
    key: Option<u16>,
    key_reads: u8,
    input: Vec<u8>,
}

impl Text40Display {
    pub fn checkpoint(&self) -> Text40Checkpoint {
        Text40Checkpoint {
            frame: self.snapshot.as_ref().map(|frame| (frame.sequence, frame.cells.to_vec())),
            key: self.keys.key, key_reads: self.keys.reads, input: self.input.clone(),
        }
    }

    pub fn restore_checkpoint(snapshot: Text40Checkpoint) -> Result<Self, String> {
        if snapshot.frame.as_ref().is_some_and(|(_, cells)| cells.len() != TEXT40_WORDS)
            || snapshot.key.is_some_and(|key| ![82, 81, 80, 79, 29, 27].contains(&key))
            || snapshot.key.is_some() && snapshot.key_reads > 10
        {
            return Err("invalid TEXT40 checkpoint".into());
        }
        Ok(Self {
            snapshot: snapshot.frame.map(|(sequence, cells)| Text40Snapshot {
                sequence, cells: cells.try_into().expect("frame length checked above"),
            }),
            keys: Text40Keys { key: snapshot.key, reads: snapshot.key_reads },
            input: snapshot.input,
            ..Self::default()
        })
    }

    /// Reopen the host terminal after loading without resetting guest-visible
    /// frame, queued input, or the remaining reads of a legacy key pulse.
    pub fn resume_interactive(&mut self) -> Result<(), String> {
        self.acquire_terminal()?;
        if self.active {
            if let Some(frame) = &self.snapshot {
                frame.render(&mut io::stderr().lock())
                    .map_err(|error| format!("Restoring TEXT40: {error}"))?;
            }
        }
        Ok(())
    }

    /// Set before entering display mode. Deterministic runs leave this false.
    pub fn set_interactive(&mut self, interactive: bool) {
        self.interactive = interactive;
    }

    pub fn enter(&mut self) -> Result<(), String> {
        self.keys = Text40Keys::default();
        self.snapshot = None;
        self.input.clear();
        self.quit = false;
        self.acquire_terminal()
    }

    fn acquire_terminal(&mut self) -> Result<(), String> {
        if !self.interactive || self.active {
            return Ok(());
        }
        if !io::stdin().is_terminal() || !io::stderr().is_terminal() {
            return Err("Interactive TEXT40 requires terminal stdin and stderr; use --headless for snapshots".into());
        }
        terminal::enable_raw_mode()
            .map_err(|error| format!("Entering TEXT40 terminal: {error}"))?;
        // Mark ownership immediately so any subsequent failure is cleaned up.
        self.active = true;
        let result = (|| {
            let mut output = io::stderr().lock();
            queue!(
                output,
                terminal::EnterAlternateScreen,
                cursor::Hide,
                terminal::Clear(ClearType::All),
                cursor::MoveTo(0, 0)
            )?;
            output.flush()
        })();
        if let Err(error) = result {
            self.leave();
            return Err(format!("Entering TEXT40 screen: {error}"));
        }
        Ok(())
    }

    pub fn leave(&mut self) {
        if self.active {
            let _ = terminal::disable_raw_mode();
            let mut output = io::stderr().lock();
            let _ = queue!(
                output,
                ResetColor,
                cursor::Show,
                terminal::LeaveAlternateScreen
            );
            let _ = output.flush();
            self.active = false;
        }
        self.keys = Text40Keys::default();
        self.snapshot = None;
        self.input.clear();
    }

    pub fn present<M: GuestMemory + ?Sized>(
        &mut self,
        memory: &M,
        sequence: u64,
    ) -> Result<(), String> {
        let snapshot = Text40Snapshot::from_memory(memory, sequence);
        if self.active {
            snapshot
                .render(&mut io::stderr().lock())
                .map_err(|error| format!("Rendering TEXT40: {error}"))?;
        }
        self.snapshot = Some(snapshot);
        Ok(())
    }

    pub fn snapshot(&self) -> Option<&Text40Snapshot> {
        self.snapshot.as_ref()
    }

    pub fn read_key(&mut self, key: u16) -> Result<bool, String> {
        self.poll_input()?;
        Ok(self.keys.read(key))
    }

    /// Bounded, nonblocking polling also keeps Ctrl-C responsive between guest
    /// input reads while raw terminal mode suppresses the operating-system signal.
    pub fn poll_input(&mut self) -> Result<(), String> {
        if !self.active {
            return Ok(());
        }
        if event::poll(Duration::ZERO)
            .map_err(|error| format!("Polling TEXT40 keyboard: {error}"))?
        {
            if let Event::Key(event) =
                event::read().map_err(|error| format!("Reading TEXT40 keyboard: {error}"))?
            {
                self.accept_event(event);
            }
        }
        Ok(())
    }

    fn accept_event(&mut self, event: KeyEvent) {
        if event.kind == KeyEventKind::Release {
            return;
        }
        if event.modifiers.contains(KeyModifiers::CONTROL)
            && matches!(event.code, KeyCode::Char('c' | 'C'))
        {
            self.quit = true;
            return;
        }
        self.keys.accept(event.code);
        match event.code {
            KeyCode::Char(character) => {
                self.input
                    .extend_from_slice(character.encode_utf8(&mut [0; 4]).as_bytes());
            }
            KeyCode::Enter => self.input.push(b'\n'),
            KeyCode::Backspace => self.input.push(8),
            KeyCode::Tab => self.input.push(b'\t'),
            KeyCode::Esc => self.input.push(27),
            _ => {}
        }
    }

    pub fn take_input(&mut self) -> Vec<u8> {
        std::mem::take(&mut self.input)
    }

    pub fn should_quit(&self) -> bool {
        self.quit
    }
}

impl Drop for Text40Display {
    fn drop(&mut self) {
        self.leave();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn checkpoint_restores_text_frame_buffered_input_and_remaining_key_pulse() {
        let mut display = Text40Display::default();
        display.accept_event(KeyEvent::new(KeyCode::Char('z'), KeyModifiers::NONE));
        for _ in 0..4 { assert!(display.read_key(29).unwrap()); }
        let mut memory = vec![0; TEXT40_LAST_WORD + 1];
        memory[TEXT40_BASE_WORD] = 0x1241;
        display.present(&memory, 9).unwrap();
        let encoded = serde_json::to_vec(&display.checkpoint()).unwrap();
        let mut restored = Text40Display::restore_checkpoint(serde_json::from_slice(&encoded).unwrap()).unwrap();
        restored.resume_interactive().unwrap(); // Headless restoration opens nothing.
        assert_eq!(restored.snapshot(), display.snapshot());
        assert_eq!(restored.take_input(), display.take_input());
        for _ in 0..8 { assert_eq!(restored.read_key(29).unwrap(), display.read_key(29).unwrap()); }
        assert!(!restored.active);
        let mut corrupt = display.checkpoint();
        corrupt.frame.as_mut().unwrap().1.pop();
        assert!(Text40Display::restore_checkpoint(corrupt).is_err());
    }

    #[test]
    fn renderer_preserves_attributes_and_printable_character_policy() {
        let mut memory = vec![0; TEXT40_LAST_WORD + 1];
        memory[TEXT40_BASE_WORD..TEXT40_BASE_WORD + 5]
            .copy_from_slice(&[0x1241, 0x3400, 0x561f, 0x787f, 0x9a7e]);
        let snapshot = Text40Snapshot::from_memory(&memory, 4);
        assert_eq!(&snapshot.text()[..5], "A ..~");
        assert_eq!(snapshot.text().lines().count(), 25);
        assert!(snapshot.text().lines().all(|line| line.len() == 40));
        let mut ansi = Vec::new();
        snapshot.render(&mut ansi).unwrap();
        let ansi = String::from_utf8(ansi).unwrap();
        assert_eq!(
            Text40Snapshot::colors(0x1241),
            (
                Color::Rgb {
                    r: 126,
                    g: 37,
                    b: 83
                },
                Color::Rgb {
                    r: 29,
                    g: 43,
                    b: 83
                }
            )
        );
        assert_eq!(ansi.matches("\r\n").count(), 24);
        memory[TEXT40_BASE_WORD] = 0;
        assert_eq!(snapshot.cells[0], 0x1241);
        assert_eq!(
            Text40Snapshot::from_memory(&[0u16; 8][..], 1).cells,
            [0; TEXT40_WORDS]
        );
    }

    #[test]
    fn terminal_keys_match_legacy_pulse_and_last_event_rules() {
        let mut keys = Text40Keys::default();
        keys.accept(KeyCode::Up);
        for _ in 0..10 {
            assert!(keys.read(82));
        }
        assert!(!keys.read(82));
        keys.accept(KeyCode::Char('Z'));
        assert!(keys.read(29));
        assert!(!keys.read(82));
        keys.accept(KeyCode::Left);
        assert!(!keys.read(29));
        assert!(keys.read(80));
        keys.accept(KeyCode::Enter);
        assert!(!keys.read(80));
        for _ in 0..300 {
            assert!(!keys.read(80));
        }
    }

    #[test]
    fn terminal_input_cleanup_and_control_c_are_independent_of_guest_reads() {
        let mut display = Text40Display::default();
        display.accept_event(KeyEvent::new(KeyCode::Char('z'), KeyModifiers::NONE));
        display.accept_event(KeyEvent::new_with_kind(
            KeyCode::Char('z'),
            KeyModifiers::NONE,
            KeyEventKind::Release,
        ));
        assert_eq!(display.take_input(), b"z");
        assert!(display.keys.read(29));
        display.accept_event(KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE));
        display.leave();
        assert!(display.take_input().is_empty());
        assert!(!display.keys.read(29));
        display.accept_event(KeyEvent::new(KeyCode::Char('c'), KeyModifiers::CONTROL));
        assert!(display.should_quit());
        assert!(display.take_input().is_empty());
        // A later headless entry clears shutdown and never acquires a terminal.
        display.enter().unwrap();
        assert!(!display.should_quit());
        assert!(!display.active);
    }
}
