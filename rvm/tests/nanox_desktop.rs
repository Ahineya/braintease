//! Real RCC Nano-X server, NanoWM, launcher and PTY clients in a JIT guest.
use rvm::{
    display_rgb565::RGB565Display,
    input_events::InputTransition,
    vm::{VMState, VM},
};
use std::{
    io::Write,
    path::PathBuf,
    time::{Duration, Instant},
};
struct Guest {
    vm: VM,
    log: String,
    output: PathBuf,
}
impl Guest {
    fn drive(&mut self) {
        self.vm.run_batch(65536).expect("guest execution");
        let bytes = self.vm.get_output();
        if !bytes.is_empty() {
            self.log.push_str(&String::from_utf8_lossy(&bytes));
            std::fs::write(self.output.join("desktop.log"), &self.log).unwrap();
        }
        assert!(
            !matches!(self.vm.state, VMState::Error(_)),
            "{:?}\n{}",
            self.vm.state,
            self.log
        );
        assert!(
            !self.log.contains("panic:")
                && !self.log.contains("NXCHECK_")
                && !self.log.contains("GsError"),
            "{}",
            self.log
        );
    }
    fn until(&mut self, label: &str, mut f: impl FnMut(&Self) -> bool) {
        let start = Instant::now();
        let mut revision = (u64::MAX, usize::MAX);
        let mut checked = Instant::now() - Duration::from_secs(1);
        std::fs::write(self.output.join("desktop-progress.txt"), label).unwrap();
        loop {
            let current = (self.vm.presented_frames, self.log.len());
            if (current != revision && checked.elapsed() >= Duration::from_millis(20))
                || matches!(self.vm.state, VMState::Halted)
            {
                if f(self) {
                    return;
                }
                revision = current;
                checked = Instant::now();
            }
            if start.elapsed() >= Duration::from_secs(60) {
                self.snapshot("desktop-failure.ppm");
                panic!("{label}\n{}", self.log);
            }
            assert!(
                !matches!(self.vm.state, VMState::Halted),
                "halted: {label}\n{}",
                self.log
            );
            self.drive();
        }
    }
    fn settle(&mut self, ms: u64) {
        let start = Instant::now();
        while start.elapsed() < Duration::from_millis(ms) {
            self.drive();
        }
    }
    fn pixels(&self) -> Vec<u16> {
        self.vm
            .rgb565_display
            .as_ref()
            .unwrap()
            .get_state()
            .lock()
            .unwrap()
            .front_buffer
            .as_ref()
            .clone()
    }
    fn rectangle(&self, x: usize, y: usize, width: usize, height: usize) -> Vec<u16> {
        let pixels = self.pixels();
        (y..y + height)
            .flat_map(|row| pixels[row * 640 + x..row * 640 + x + width].iter().copied())
            .collect()
    }
    fn key(&mut self, key: u16) {
        self.vm.devices.accept_events([
            InputTransition::Key { key, down: true },
            InputTransition::Key { key, down: false },
        ]);
    }
    fn modified_key(&mut self, modifier: u16, key: u16) {
        self.vm.devices.accept_events([InputTransition::Key {
            key: modifier,
            down: true,
        }]);
        self.key(key);
        self.vm.devices.accept_events([InputTransition::Key {
            key: modifier,
            down: false,
        }]);
    }
    fn text(&mut self, s: &str) {
        for ch in s.chars() {
            if ch == '>' || ch == ':' {
                self.modified_key(225, if ch == '>' { 55 } else { 51 });
            } else {
                self.key(if ch == ' ' {
                    44
                } else {
                    rvm::devices::key_code(&ch.to_string()).unwrap()
                });
            }
            // Pace physical transitions through the bounded GX queue, as the host
            // event pump does; a whole synthetic command is an overflow test.
            self.settle(20);
        }
    }
    fn click(&mut self, x: i32, y: i32) {
        self.vm.devices.accept_events([
            InputTransition::Motion { x, y },
            InputTransition::Button {
                button: 0,
                down: true,
                x,
                y,
            },
            InputTransition::Button {
                button: 0,
                down: false,
                x,
                y,
            },
        ]);
    }
    fn right_click(&mut self, x: i32, y: i32) {
        self.vm.devices.accept_events([
            InputTransition::Motion { x, y },
            InputTransition::Button {
                button: 1,
                down: true,
                x,
                y,
            },
            InputTransition::Button {
                button: 1,
                down: false,
                x,
                y,
            },
        ]);
    }
    fn drag(&mut self, x: i32, y: i32, dx: i32, dy: i32) {
        self.vm.devices.accept_events([
            InputTransition::Motion { x, y },
            InputTransition::Button {
                button: 0,
                down: true,
                x,
                y,
            },
        ]);
        self.settle(120);
        self.vm.devices.accept_events([InputTransition::Motion {
            x: x + dx,
            y: y + dy,
        }]);
        self.settle(120);
        self.vm.devices.accept_events([InputTransition::Button {
            button: 0,
            down: false,
            x: x + dx,
            y: y + dy,
        }]);
    }
    fn snapshot(&self, name: &str) {
        let pixels = self.pixels();
        let mut f = std::fs::File::create(self.output.join(name)).unwrap();
        f.write_all(b"P6\n640 480\n255\n").unwrap();
        for p in pixels {
            f.write_all(&[
                ((p >> 11) * 255 / 31) as u8,
                (((p >> 5) & 63) * 255 / 63) as u8,
                ((p & 31) * 255 / 31) as u8,
            ])
            .unwrap();
        }
    }
}

fn roach_asset(heading: usize) -> Vec<u16> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .to_path_buf();
    let path = root.join(format!(
        "ports/nanox/upstream/src/images/demos/nanox/nxroach/roach{:03}.xbm",
        heading * 15
    ));
    let source = std::fs::read_to_string(path).unwrap();
    assert!(source.lines().any(|line| line.ends_with("_width 48")));
    assert!(source.lines().any(|line| line.ends_with("_height 48")));
    let octets: Vec<u16> = source
        .split("0x")
        .skip(1)
        .map(|text| {
            let number: String = text.chars().take_while(|c| c.is_ascii_hexdigit()).collect();
            u16::from_str_radix(&number, 16).unwrap()
        })
        .collect();
    assert_eq!(octets.len(), 288);
    (0..48 * 48)
        .map(|i| (octets[(i / 48) * 6 + (i % 48) / 8] >> (i % 8)) & 1)
        .collect()
}

#[test]
#[ignore = "run via ports/nanox/tools/test_desktop.py --only roach with a disposable image"]
fn roaches_masks_animation_occlusion_and_cleanup() {
    let build = PathBuf::from(std::env::var_os("NANOX_TEST_BUILD").unwrap());
    let image = PathBuf::from(std::env::var_os("NANOX_DESKTOP_IMAGE").unwrap());
    let output = build.join("nanox/roach");
    std::fs::create_dir_all(&output).unwrap();
    let mut vm = VM::with_options(64000, 16384000, Some(image));
    vm.host_input_enabled = false;
    vm.jit_enabled = true;
    vm.devices
        .network
        .configure_host(Some(std::ffi::OsStr::new("off")))
        .unwrap();
    vm.load_binary(&std::fs::read(build.join("bios.bin")).unwrap())
        .unwrap();
    vm.rgb565_display = Some(RGB565Display::new());
    vm.devices
        .accept_events([InputTransition::Motion { x: 620, y: 400 }]);
    vm.push_input_string("nxroach --help\nnxroach -roaches 0; echo BAD_COUNT:$?\nnxroach -roaches 65; echo BIG_COUNT:$?\nnxroach -speed nan; echo BAD_SPEED:$?\nnxroach -speed; echo MISSING_SPEED:$?\nnxroach -rc 0x1000000; echo BAD_COLOUR:$?\nnxroach -seed -1; echo BAD_SEED:$?\nnano-X -p &\ns=$!\nsleep 1\nnxroachcheck\necho ATLAS_STATUS:$?\nkill $s\nwait $s\nNANOX_TRACE=1 startx\necho ROACH_SESSION:$?\nps\nshutdown\n");
    let mut g = Guest {
        vm,
        log: String::new(),
        output,
    };
    let mut atlas: Vec<u16> = (0..322 * 224)
        .map(|i| {
            if ((i % 322) / 8 + (i / 322) / 8) & 1 == 1 {
                0x220c
            } else {
                0x4310
            }
        })
        .collect();
    for heading in 0..24 {
        for (i, bit) in roach_asset(heading).iter().enumerate() {
            let x = (heading % 6) * 56 + i % 48;
            let y = (heading / 6) * 56 + i / 48;
            if x < 322 && *bit != 0 {
                atlas[y * 322 + x] = 0;
            }
        }
    }
    for y in 52..112 {
        for x in 52..112 {
            atlas[y * 322 + x] = 0xf800;
        }
    }
    g.until(
        "all 24 roach masks match original XBM pixels, transparency and clipping",
        |g| g.log.contains("NXROACH_ATLAS_READY") && g.rectangle(16, 16, 322, 224) == atlas,
    );
    for label in [
        "BAD_COUNT:2",
        "BIG_COUNT:2",
        "BAD_SPEED:2",
        "MISSING_SPEED:2",
        "BAD_COLOUR:2",
        "BAD_SEED:2",
    ] {
        assert!(g.log.contains(label), "{label}\n{}", g.log);
    }
    g.snapshot("roach-atlas.ppm");
    g.key(20); // q closes the pixel probe.
    g.until("desktop for live roaches", |g| {
        g.log.contains("ATLAS_STATUS:0")
            && g.log.contains("NXTERM_READY ")
            && !locate(&g.pixels(), "fuzix# ").is_empty()
    });
    g.settle(300);
    let first = locate(&g.pixels(), "fuzix# ")[0];
    g.drag(first.0 + 90, first.1 - 16, 72 - first.0, 80 - first.1);
    g.until("terminal in fixed position", |g| {
        empty_terminal(&g.pixels(), 72, 80)
    });
    let background = g.rectangle(10, 214, 48, 48);
    let red: Vec<u16> = roach_asset(6)
        .iter()
        .zip(&background)
        .map(|(&mask, &colour)| if mask != 0 { 0xf800 } else { colour })
        .collect();
    // Seed 5407 places a north-facing roach at (10,214) with FUZIX's RNG.
    g.text("nxroach -roaches 1 -speed 0 -seed 5407 -rc 255 2> /dev/console");
    g.key(40);
    g.until("stationary red roach on desktop", |g| {
        g.log.contains("NXROACH_READY ") && g.rectangle(10, 214, 48, 48) == red
    });
    g.snapshot("roach-visible.ppm");
    g.drag(162, 64, -64, 0);
    g.until(
        "window covers and hides roach without foreground corruption",
        |g| g.log.contains("NXROACH_VISIBLE 0") && g.rectangle(10, 214, 48, 48) == vec![0; 48 * 48],
    );
    g.snapshot("roach-hidden.ppm");
    g.drag(98, 64, 64, 0);
    g.until("moving window away reveals roach again", |g| {
        g.log.matches("NXROACH_VISIBLE 1").count() >= 2 && g.rectangle(10, 214, 48, 48) == red
    });
    g.right_click(620, 400);
    g.until("right-click stops and erases roach", |g| {
        g.log.matches("NXROACH_EXIT ").count() == 1 && g.rectangle(10, 214, 48, 48) == background
    });
    g.click(100, 180);
    g.settle(200);
    g.text("nxroach -roaches 1 -speed 1 -seed 5407 2> /dev/console");
    g.key(40);
    g.until("animated roach starts", |g| {
        g.log.matches("NXROACH_READY ").count() == 2
            && g.rectangle(0, 100, 70, 300)
                .iter()
                .filter(|&&p| p == 0)
                .count()
                > 50
    });
    let pose = g.rectangle(0, 100, 70, 300);
    let frames = g.log.matches("NXROACH_FRAME ").count();
    g.until("roach scampers across desktop", |g| {
        g.log.matches("NXROACH_FRAME ").count() > frames && g.rectangle(0, 100, 70, 300) != pose
    });
    g.snapshot("roach-moving.ppm");
    g.modified_key(224, 6); // Ctrl-C is delivered by the PTY to the foreground app.
    g.until("Ctrl-C stops and clears animation", |g| {
        g.log.matches("NXROACH_EXIT ").count() == 2 && !g.rectangle(0, 100, 70, 300).contains(&0)
    });
    g.settle(200);
    g.text("nxroach -roaches 1 -speed 0 -seed 5407 2> /dev/console");
    g.key(40);
    g.until("frozen roach waits on events", |g| {
        g.log.matches("NXROACH_READY ").count() == 3
            && g.rectangle(0, 100, 70, 300)
                .iter()
                .filter(|&&p| p == 0)
                .count()
                > 50
    });
    g.modified_key(224, 6);
    g.until("Ctrl-C wakes and clears idle client", |g| {
        g.log.matches("NXROACH_EXIT ").count() == 3 && !g.rectangle(0, 100, 70, 300).contains(&0)
    });
    g.settle(200);
    // The launcher starts ten roaches at the default speed, with a fixed seed
    // so some are visible. Closing the desktop must terminate the infestation.
    g.click(450, 450);
    g.until("launcher starts roaches", |g| {
        g.log.matches("NXROACH_READY ").count() == 4
            && g.rectangle(0, 0, 70, 400)
                .iter()
                .filter(|&&p| p == 0)
                .count()
                > 50
    });
    g.snapshot("roach-desktop.ppm");
    g.vm.devices.accept_events([InputTransition::Close]);
    g.until("roach session shuts down cleanly", |g| {
        matches!(g.vm.state, VMState::Halted)
    });
    let tail = g.log.split("ROACH_SESSION:0").nth(1).expect(&g.log);
    assert!(
        !tail.contains("nxroach") && tail.contains("disk synced; power off."),
        "{}",
        g.log
    );
    println!("Nxroach JIT: all 24 original bitmap masks, transparent/clipped drawing, motion, window hiding/reveal, right-click/Ctrl-C cleanup, launcher and session teardown passed");
}
fn font() -> Vec<u16> {
    include_str!("../../ports/graphics/fonts/rom8x16.inc")
        .split("0x")
        .skip(1)
        .map(|s| u16::from_str_radix(&s[..4], 16).unwrap())
        .collect()
}
fn locate(pixels: &[u16], s: &str) -> Vec<(i32, i32)> {
    if pixels.len() != 640 * 480 {
        return vec![];
    }
    let font = font();
    let w = s.len() * 8;
    let mut found = Vec::new();
    let glyph = s.as_bytes()[0] as usize;
    let (sx, sy) = (0..16)
        .flat_map(|y| (0..8).map(move |x| (x, y)))
        .find(|&(x, y)| font[glyph * 16 + y] & (0x8000 >> x) != 0)
        .unwrap();
    for y in 0..480 - 16 {
        for x in 0..640 - w {
            if pixels[(y + sy) * 640 + x + sx] != 0xad55 {
                continue;
            }
            let ok = s.bytes().enumerate().all(|(i, c)| {
                (0..16).all(|ry| {
                    (0..8).all(|rx| {
                        let want = if font[c as usize * 16 + ry] & (0x8000 >> rx) != 0 {
                            0xad55
                        } else {
                            0
                        };
                        pixels[(y + ry) * 640 + x + i * 8 + rx] == want
                    })
                })
            });
            if ok {
                found.push((x as i32, y as i32));
            }
        }
    }
    found
}
// Cascaded terminals can have identical positions. A substring match could
// see the previous terminal's prompt before the new client has painted, so
// require the complete blank terminal plus its fresh prompt and cursor.
fn empty_terminal(pixels: &[u16], x: i32, y: i32) -> bool {
    let font = font();
    let prompt = b"fuzix# ";
    (0..320).all(|row| {
        (0..512).all(|col| {
            let glyph = row < 16
                && col < prompt.len() * 8
                && font[prompt[col / 8] as usize * 16 + row] & (0x8000 >> (col % 8)) != 0;
            let cursor = row == 15 && (prompt.len() * 8..prompt.len() * 8 + 8).contains(&col);
            pixels[(y as usize + row) * 640 + x as usize + col]
                == if glyph || cursor { 0xad55 } else { 0 }
        })
    })
}
#[test]
#[ignore = "run via ports/nanox/tools/test_desktop.py with a disposable image"]
fn desktop_clients_input_resize_and_recovery() {
    let build = PathBuf::from(std::env::var_os("NANOX_TEST_BUILD").unwrap());
    let image = PathBuf::from(std::env::var_os("NANOX_DESKTOP_IMAGE").unwrap());
    let output = build.join("nanox");
    std::fs::create_dir_all(&output).unwrap();
    let mut vm = VM::with_options(64000, 16384000, Some(image));
    vm.host_input_enabled = false;
    vm.jit_enabled = true;
    vm.devices
        .network
        .configure_host(Some(std::ffi::OsStr::new("off")))
        .unwrap();
    vm.load_binary(&std::fs::read(build.join("bios.bin")).unwrap())
        .unwrap();
    vm.rgb565_display = Some(RGB565Display::new());
    vm.push_input_string("gterm sh -c 'stty -a > /gterm-settings'\ncat /gterm-settings\nnano-X -p &\ns=$!\nsleep 1\nnxcheck\nnxcheck\nkill $s\nwait $s\nNANOX_TRACE=1 startx\necho DESKTOP_STATUS:$?\ncat /desktop-note\ncat /second-note\ncat /term-size\ncat /vi-note\ncat /shell-erase\ncat /canonical-note\nNANOX_TRACE=1 startx\necho REOPEN_STATUS:$?\nshutdown\n");
    let mut g = Guest {
        vm,
        log: String::new(),
        output,
    };
    g.until("first terminal prompt", |g| {
        g.log.matches("NANOX_CLIENT_OK").count() == 2 && !locate(&g.pixels(), "fuzix# ").is_empty()
    });
    assert!(
        g.log
            .split("fuzix# nano-X")
            .next()
            .unwrap()
            .contains("erase = ^?"),
        "standalone gterm must also advertise DEL as VERASE\n{}",
        g.log
    );
    g.settle(500);
    let first = locate(&g.pixels(), "fuzix# ")[0];
    g.snapshot("desktop-first.ppm");
    // Real upstream GUI clients coexist with the terminal. Eyes uses a bitmap
    // window mask, pointer events and blink timers; the clock uses timed waits.
    g.click(250, 450);
    g.until("nxeyes rendered shaped eyes", |g| {
        g.log.contains("NXEYES_READY ")
            && g.rectangle(0, 0, 82, 39)
                .iter()
                .filter(|&&p| p == 0xffff)
                .count()
                > 300
    });
    g.settle(100);
    let pupils = g.rectangle(0, 0, 82, 39);
    g.vm.devices
        .accept_events([InputTransition::Motion { x: 1, y: 200 }]);
    g.until("nxeyes pupils follow pointer", |g| {
        g.rectangle(0, 0, 82, 39) != pupils
    });
    g.drag(20, 20, 100, 0);
    g.until("nxeyes left-button drag", |g| {
        g.rectangle(100, 0, 82, 39)
            .iter()
            .filter(|&&p| p == 0xffff)
            .count()
            > 300
            && !g.rectangle(0, 0, 82, 39).contains(&0xffff)
    });
    g.click(350, 450);
    g.until("nxclock rendered face", |g| {
        g.log.contains("NXCLOCK_READY ")
            && g.log.contains("NXCLOCK_TICK ")
            && g.rectangle(400, 40, 200, 200)
                .iter()
                .filter(|&&p| p == 0x2965)
                .count()
                > 1000
    });
    g.settle(200);
    let clock = g.rectangle(400, 40, 200, 200);
    let ticks = g.log.matches("NXCLOCK_TICK ").count();
    g.until("nxclock hands advance", |g| {
        g.log.matches("NXCLOCK_TICK ").count() > ticks && g.rectangle(400, 40, 200, 200) != clock
    });
    g.until("nxeyes closes and reopens for a blink", |g| {
        g.log.contains("NXEYES_BLINK 1") && g.log.contains("NXEYES_BLINK 0")
    });
    g.snapshot("desktop-apps.ppm");
    g.click(588, 24);
    g.until("nxclock close button", |g| g.log.contains("NXCLOCK_EXIT "));
    g.vm.devices.accept_events([
        InputTransition::Motion { x: 120, y: 20 },
        InputTransition::Button {
            button: 1,
            down: true,
            x: 120,
            y: 20,
        },
        InputTransition::Button {
            button: 1,
            down: false,
            x: 120,
            y: 20,
        },
    ]);
    g.until("nxeyes right-click close", |g| {
        g.log.contains("NXEYES_EXIT ")
    });
    g.click(first.0 + 20, first.1 + 40);
    g.settle(100);
    // A real command crosses host HID -> GX -> Nano-X -> PTY -> shell -> disk.
    g.text("echo nanox-shell > /desktop-note");
    g.key(40);
    g.until("shell command completed", |g| {
        locate(&g.pixels(), "fuzix# ")
            .iter()
            .any(|&(x, y)| x == first.0 && y >= first.1 + 16)
    });
    // Backspace must agree with the slave's VERASE in both a shell and Levee.
    g.text("echo shell-backspacex");
    g.key(42);
    g.settle(50);
    g.text(" > /shell-erase");
    g.key(40);
    g.settle(300);
    g.text("vi /vi-note");
    g.key(40);
    g.until("Levee alternate screen", |g| {
        locate(&g.pixels(), "fuzix# ").is_empty()
    });
    g.text("ihello-vix");
    g.until("Levee inserted text", |g| {
        !locate(&g.pixels(), "hello-vix").is_empty()
    });
    g.key(42);
    g.settle(100);
    g.key(41);
    g.settle(100);
    g.text(":wqx");
    g.key(42);
    g.settle(100);
    g.key(40);
    g.until("Levee saved after insert and command-line backspace", |g| {
        !locate(&g.pixels(), "fuzix# ").is_empty()
    });
    // cat uses canonical line editing rather than the shell's own raw editor.
    g.text("cat > /canonical-note");
    g.key(40);
    g.settle(300);
    g.text("canonicalx");
    g.key(42);
    g.settle(50);
    g.key(40);
    g.settle(100);
    g.modified_key(224, 7); // Ctrl-D: EOF on an empty canonical line.
    g.settle(300);
    // The upstream launcher starts another independent terminal client.
    g.click(50, 450);
    g.until("launcher opened second terminal", |g| {
        g.log
            .split_inclusive('\n')
            .filter(|l| l.ends_with('\n') && l.contains("NXTERM_READY "))
            .count()
            >= 2
    });
    let pid: i32 = g
        .log
        .split_inclusive('\n')
        .filter(|l| l.ends_with('\n') && l.contains("NXTERM_READY "))
        .last()
        .unwrap()
        .split("NXTERM_READY ")
        .nth(1)
        .unwrap()
        .split_whitespace()
        .next()
        .unwrap()
        .parse()
        .unwrap();
    let second = (24 + (pid % 4) * 16, 32 + (pid % 4) * 16);
    g.until("second PTY prompt", |g| {
        empty_terminal(&g.pixels(), second.0, second.1)
    });
    g.text("echo second-shell > /second-note");
    g.key(40);
    g.settle(500);
    let prompts = locate(&g.pixels(), "fuzix# ");
    assert!(!prompts.is_empty(), "second terminal missing\n{}", g.log);
    g.snapshot("desktop-two.ppm");
    // Move and resize through NanoWM's actual non-client mouse handlers.
    g.drag(second.0 + 90, second.1 - 16, 40, 20);
    let moved = (second.0 + 40, second.1 + 20);
    g.until("NanoWM moved terminal", |g| {
        locate(&g.pixels(), "fuzix# ").contains(&moved)
    });
    g.drag(moved.0 + 512, moved.1 + 320, -64, -32);
    g.until("resized terminal notification", |g| {
        g.log.contains(&format!("NXTERM_SIZE {pid} 18 56\r\n"))
    });
    g.settle(300);
    g.text("stty -a > /term-size");
    g.key(40);
    g.until("resized PTY command completed", |g| {
        locate(&g.pixels(), "fuzix# ")
            .iter()
            .any(|&(x, y)| x == moved.0 && y >= moved.1 + 32)
    });
    g.settle(500);
    g.snapshot("desktop-moved.ppm");
    // Close the resized terminal through NanoWM, then close the remaining shell.
    g.click(moved.0 + 436, moved.1 - 16);
    g.until("NanoWM close button", |g| {
        g.log.contains(&format!("NXTERM_EXIT {pid}\r\n"))
    });
    g.click(first.0 + 20, first.1 + 40);
    g.settle(200);
    g.text("exit");
    g.key(40);
    g.until("last terminal closed", |g| {
        g.log.matches("NXTERM_EXIT ").count() >= 2
    });
    g.settle(300);
    g.snapshot("desktop-launcher.ppm");
    g.click(50, 450);
    g.until("terminal reopened from launcher", |g| {
        g.log
            .split_inclusive('\n')
            .filter(|l| l.ends_with('\n') && l.contains("NXTERM_READY "))
            .count()
            >= 3
    });
    g.until("replacement terminal prompt", |g| {
        !locate(&g.pixels(), "fuzix# ").is_empty()
    });
    // Stop this display/session, check persistent files and start a fresh desktop.
    g.vm.devices.accept_events([InputTransition::Close]);
    g.until("desktop returned and reopened", |g| {
        g.log.contains("DESKTOP_STATUS:0")
            && g.log.contains("nanox-shell")
            && g.log.contains("second-shell")
            && g.log.replace("\r", "").contains("18 rows 56 columns")
            && g.log.contains("erase = ^?")
            && g.log.lines().any(|line| line.trim_end() == "hello-vi")
            && g.log
                .lines()
                .any(|line| line.trim_end() == "shell-backspace")
            && g.log.lines().any(|line| line.trim_end() == "canonical")
            && g.log.matches("Nano-X ready").count() >= 3
            && !locate(&g.pixels(), "fuzix# ").is_empty()
    });
    g.vm.devices.accept_events([InputTransition::Close]);
    g.until("clean shutdown", |g| matches!(g.vm.state, VMState::Halted));
    assert!(
        g.log.contains("REOPEN_STATUS:0") && g.log.contains("disk synced; power off."),
        "{}",
        g.log
    );
    println!("Nano-X desktop: independent clients/timers, NanoWM, launcher, shell/Levee backspace, multiple PTYs, movement, clean close/reopen and persistent files passed");
}
