//! End-to-end save/restore checks use tiny BF transport fixtures, never Doom.
use rvm::constants::*;
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, Output},
    sync::atomic::{AtomicU64, Ordering},
};

static NEXT: AtomicU64 = AtomicU64::new(0);
struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "rbf-checkpoint-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    fn path(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[derive(Default)]
struct Source {
    text: String,
    pointer: usize,
}
impl Source {
    fn at(&mut self, target: usize) {
        self.text.push_str(&if target >= self.pointer {
            ">".repeat(target - self.pointer)
        } else {
            "<".repeat(self.pointer - target)
        });
        self.pointer = target;
    }
    fn value(&mut self, value: u16) {
        self.text.push_str("[-]");
        self.text.push_str(&if value < 32768 {
            "+".repeat(usize::from(value))
        } else {
            "-".repeat(65536 - usize::from(value))
        });
    }
    fn store(&mut self, address: usize, value: u16) {
        self.at(8 + address * 8 + 4);
        self.value(value);
        self.text.push('!');
    }
    fn instruction(&mut self) {
        self.at(8 + 8 * 40);
        self.text.push('$');
    }
    fn write(&self, path: &Path) {
        fs::write(path, format!("BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 2312 END_BFHOST\n[$?!]{}", self.text)).unwrap();
    }
}
fn run(source: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_rbf"))
        .arg(source)
        .args(["--headless", "--timeout", "5"])
        .args(args)
        .output()
        .unwrap()
}
fn text(path: &Path) -> &str {
    path.to_str().unwrap()
}
fn successful(output: &Output) {
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

fn two_frames(path: &Path) {
    let mut source = Source::default();
    source.instruction();
    source.store(HDR_TTY_OUT, 65);
    source.store(HDR_DISP_RESOLUTION, 0x0101);
    source.store(HDR_DISP_MODE, DISP_RGB565);
    source.store(33, 0xf800);
    source.store(HDR_DISP_FLUSH, 1);
    source.instruction();
    source.store(HDR_TTY_OUT, 66);
    source.store(33, 0x001f);
    source.store(HDR_DISP_FLUSH, 1);
    source.write(path);
}

#[test]
fn frame_checkpoint_resumes_in_a_new_process_without_replaying_output_or_boot() {
    let dir = Directory::new();
    let source = dir.path("frames.bf");
    two_frames(&source);
    let state = dir.path("boot.state");
    let split_report = dir.path("split.json");
    let split = run(
        &source,
        &[
            "--save-state",
            text(&state),
            "--save-at-frame",
            "1",
            "--report",
            text(&split_report),
        ],
    );
    successful(&split);
    assert_eq!(split.stdout, b"A");
    assert!(state.exists());
    let first: serde_json::Value =
        serde_json::from_slice(&fs::read(split_report).unwrap()).unwrap();
    assert_eq!(first["frames"], 1);
    assert_eq!(first["stop_reason"], "checkpoint_frame");
    let resumed_frame = dir.path("resumed.rgb565");
    let resumed_report = dir.path("resumed.json");
    let resumed = run(
        &source,
        &[
            "--load-state",
            text(&state),
            "--raw-frame",
            text(&resumed_frame),
            "--capture-frame",
            "2",
            "--report",
            text(&resumed_report),
        ],
    );
    successful(&resumed);
    assert_eq!(resumed.stdout, b"B");
    let plain_frame = dir.path("plain.rgb565");
    let plain_report = dir.path("plain.json");
    let plain = run(
        &source,
        &[
            "--raw-frame",
            text(&plain_frame),
            "--capture-frame",
            "2",
            "--report",
            text(&plain_report),
        ],
    );
    successful(&plain);
    assert_eq!(plain.stdout, b"AB");
    assert_eq!(fs::read(&resumed_frame).unwrap(), [31, 0]);
    assert_eq!(
        fs::read(resumed_frame).unwrap(),
        fs::read(plain_frame).unwrap()
    );
    let actual: serde_json::Value =
        serde_json::from_slice(&fs::read(resumed_report).unwrap()).unwrap();
    let expected: serde_json::Value =
        serde_json::from_slice(&fs::read(plain_report).unwrap()).unwrap();
    for field in [
        "bf_dispatches",
        "ripple_instructions",
        "tick",
        "frames",
        "guest_result",
        "width",
        "height",
        "source_sha256",
        "seed",
    ] {
        assert_eq!(actual[field], expected[field], "{field}");
    }
}

#[test]
fn checkpoint_rejects_changed_image_mode_corruption_and_truncation() {
    let dir = Directory::new();
    let source = dir.path("frames.bf");
    two_frames(&source);
    let state = dir.path("boot.state");
    successful(&run(
        &source,
        &["--save-state", text(&state), "--save-at-frame", "1"],
    ));
    let original = fs::read(&state).unwrap();
    let mode = run(&source, &["--load-state", text(&state), "--no-optimize"]);
    assert!(!mode.status.success());
    assert!(String::from_utf8_lossy(&mode.stderr).contains("optimization mode"));
    let changed = dir.path("changed.bf");
    fs::write(
        &changed,
        format!("{}\n+", fs::read_to_string(&source).unwrap()),
    )
    .unwrap();
    let mismatch = run(&changed, &["--load-state", text(&state)]);
    assert!(!mismatch.status.success());
    assert!(String::from_utf8_lossy(&mismatch.stderr).contains("different BF program"));
    for mut bytes in [original.clone(), original[..original.len() / 2].to_vec()] {
        bytes[24] ^= 1;
        fs::write(&state, bytes).unwrap();
        let bad = run(&source, &["--load-state", text(&state)]);
        assert!(!bad.status.success());
        assert!(String::from_utf8_lossy(&bad.stderr).contains("checksum mismatch"));
    }
}

#[test]
fn dispatch_limit_checkpoint_continues_with_cumulative_counts() {
    let dir = Directory::new();
    let source = dir.path("loop.bf");
    fs::write(&source, "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 2312 END_BFHOST\n[$?!]+[]").unwrap();
    let state = dir.path("loop.state");
    let report = dir.path("loop.json");
    let first = run(
        &source,
        &["--max-dispatches", "17", "--save-state", text(&state)],
    );
    assert!(!first.status.success());
    assert!(state.exists());
    let resumed = run(
        &source,
        &[
            "--load-state",
            text(&state),
            "--max-dispatches",
            "29",
            "--report",
            text(&report),
        ],
    );
    assert!(!resumed.status.success());
    let actual: serde_json::Value = serde_json::from_slice(&fs::read(report).unwrap()).unwrap();
    assert_eq!(actual["bf_dispatches"], 29);
    assert_eq!(actual["stop_reason"], "Brainfuck dispatch limit exceeded");
}

#[test]
fn checkpoint_pause_preserves_uncommitted_disk_status_until_guest_commit() {
    let dir = Directory::new();
    let source = dir.path("dirty.bf");
    let mut bf = Source::default();
    bf.store(HDR_STORE_DATA, 0x1234);
    bf.store(HDR_DISP_RESOLUTION, 0x0101);
    bf.store(HDR_DISP_MODE, DISP_RGB565);
    bf.store(HDR_DISP_FLUSH, 1);
    for commit in [false, true] {
        if commit {
            bf.store(HDR_STORE_CTL, STORE_COMMIT);
        }
        bf.at(8 + HDR_TTY_OUT * 8 + 4);
        bf.value(0);
        bf.at(8 + HDR_STORE_CTL * 8 + 4);
        bf.text.push('?');
        let status = bf.pointer;
        bf.text.push_str("[-");
        bf.at(8 + HDR_TTY_OUT * 8 + 4);
        bf.text.push('+');
        bf.at(status);
        bf.text.push(']');
        bf.at(8 + HDR_TTY_OUT * 8 + 4);
        bf.text.push('!');
    }
    bf.write(&source);
    let disk = dir.path("disk.img");
    fs::write(&disk, []).unwrap();
    let state = dir.path("dirty.state");
    successful(&run(
        &source,
        &[
            "--disk",
            text(&disk),
            "--save-state",
            text(&state),
            "--save-at-frame",
            "1",
        ],
    ));
    assert_eq!(&fs::read(&disk).unwrap()[..2], &[0x34, 0x12]);
    let resumed = run(
        &source,
        &["--disk", text(&disk), "--load-state", text(&state)],
    );
    successful(&resumed);
    let plain_disk = dir.path("plain.img");
    fs::write(&plain_disk, []).unwrap();
    let plain = run(&source, &["--disk", text(&plain_disk)]);
    successful(&plain);
    assert_eq!(plain.stdout, [STORE_DIRTY as u8, 0]);
    assert_eq!(resumed.stdout, plain.stdout);
    assert_eq!(fs::read(disk).unwrap(), fs::read(plain_disk).unwrap());
}

#[test]
fn resumed_script_can_be_relative_to_saved_tick_and_disk_must_match() {
    let dir = Directory::new();
    let source = dir.path("input.bf");
    let mut bf = Source::default();
    bf.store(40, 1);
    bf.store(41, 8);
    bf.store(28, 40);
    bf.store(29, 3); // Tick 1.
    bf.store(HDR_DISP_RESOLUTION, 0x0101);
    bf.store(HDR_DISP_MODE, DISP_RGB565);
    bf.store(33, 0xf800);
    bf.store(HDR_DISP_FLUSH, 1);
    bf.store(29, 3); // Tick 2, where the replacement replay should press UP.
    bf.store(HDR_TTY_OUT, 0); // Establish a zero destination, then transfer key state.
    bf.store(41, 28);
    bf.store(29, 6);
    bf.at(8 + (40 + 12 + 82 / 16) * 8 + 4);
    bf.text.push('?');
    let key = bf.pointer;
    bf.text.push_str("[-");
    bf.at(8 + 4);
    bf.text.push('+');
    bf.at(key);
    bf.text.push(']');
    bf.at(8 + 4);
    bf.text.push('!');
    bf.write(&source);
    let disk = dir.path("disk.img");
    fs::write(&disk, [1, 2, 3, 4]).unwrap();
    let state = dir.path("input.state");
    successful(&run(
        &source,
        &[
            "--disk",
            text(&disk),
            "--save-state",
            text(&state),
            "--save-at-frame",
            "1",
        ],
    ));
    let script = dir.path("up.json");
    fs::write(&script, r#"[{"tick":1,"key":"UP","down":true}]"#).unwrap();
    let args = [
        "--disk",
        text(&disk),
        "--load-state",
        text(&state),
        "--input-script",
        text(&script),
        "--input-script-relative",
    ];
    let resumed = run(&source, &args);
    successful(&resumed);
    assert_eq!(resumed.stdout, [0, 4]);
    let absolute = run(&source, &args[..args.len() - 1]);
    assert!(!absolute.status.success());
    assert!(String::from_utf8_lossy(&absolute.stderr).contains("current tick"));
    fs::write(&disk, [4, 3, 2, 1]).unwrap();
    let changed = run(&source, &args);
    assert!(!changed.status.success());
    assert!(String::from_utf8_lossy(&changed.stderr).contains("disk"));
}

#[test]
fn failed_guest_does_not_replace_a_good_checkpoint_and_halt_before_target_is_explicit() {
    let dir = Directory::new();
    let source = dir.path("bad.bf");
    fs::write(&source, "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 2312 END_BFHOST\n[$?!]?").unwrap();
    let state = dir.path("good.state");
    fs::write(&state, b"previous good checkpoint").unwrap();
    let bad = run(&source, &["--save-state", text(&state)]);
    assert!(!bad.status.success());
    assert_eq!(fs::read(&state).unwrap(), b"previous good checkpoint");
    fs::write(&source, "BFHOST 1 BANK_SIZE 64 MEMORY_BANKS 4 INSTRUCTIONS 1 ENTRY 0 ORIGIN 8 TAPE_CELLS 2312 END_BFHOST\n[$?!]").unwrap();
    let short = run(
        &source,
        &["--save-state", text(&state), "--save-at-frame", "1"],
    );
    assert!(!short.status.success());
    assert!(String::from_utf8_lossy(&short.stderr).contains("halted before checkpoint frame 1"));
}

#[test]
fn checkpoint_options_validate_before_execution_and_protect_source() {
    let dir = Directory::new();
    let source = dir.path("frames.bf");
    two_frames(&source);
    let before = fs::read(&source).unwrap();
    let result = run(
        &source,
        &["--save-state", text(&source), "--save-at-frame", "1"],
    );
    assert!(!result.status.success());
    assert_eq!(fs::read(&source).unwrap(), before);
    for args in [
        vec!["--save-at-frame", "1"],
        vec!["--save-state", "x", "--save-at-frame", "0"],
        vec!["--load-state", "missing", "--seed", "1"],
        vec!["--input-script-relative"],
    ] {
        assert!(!run(&source, &args).status.success());
    }
}
