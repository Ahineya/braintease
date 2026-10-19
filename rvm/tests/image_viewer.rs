//! Opt-in real image viewer acceptance; runner supplies a disposable disk.
use rvm::{
    display_rgb565::RGB565Display,
    input_events::InputTransition,
    vm::{VMState, VM},
};
use std::{
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
            std::fs::write(self.output.join("rvm.log"), &self.log).unwrap();
        }
        assert!(
            !matches!(self.vm.state, VMState::Error(_)),
            "{:?}\n{}",
            self.vm.state,
            self.log
        );
        for bad in ["panic:", "GsError", "Drawing failed", "NXR_BUFFER_FAILED"] {
            assert!(!self.log.contains(bad), "{}", self.log);
        }
    }
    fn until(&mut self, marker: &str, start: usize) {
        let end = Instant::now() + Duration::from_secs(120);
        while !self.log[start..].contains(marker) {
            assert!(
                Instant::now() < end && !matches!(self.vm.state, VMState::Halted),
                "waiting for {marker}\n{}",
                self.log
            );
            self.drive();
        }
    }
    fn command(&mut self, text: &str, marker: &str) {
        let at = self.log.len();
        self.vm.push_input_string(text);
        self.until(marker, at);
    }
    fn settle(&mut self, ms: u64) {
        let end = Instant::now() + Duration::from_millis(ms);
        while Instant::now() < end {
            self.drive();
        }
    }
    fn key(&mut self, key: u16) -> usize {
        let at = self.log.len();
        self.vm.devices.accept_events([
            InputTransition::Key { key, down: true },
            InputTransition::Key { key, down: false },
        ]);
        at
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
    fn wait_pixels(&mut self, test: impl Fn(&[u16]) -> bool) {
        let end = Instant::now() + Duration::from_secs(45);
        while !test(&self.pixels()) {
            if Instant::now() >= end {
                self.snapshot("failure.rgb565");
                panic!("pixels did not arrive\n{}", self.log);
            }
            self.drive();
        }
    }
    fn snapshot(&self, name: &str) {
        std::fs::write(
            self.output.join(name),
            self.pixels()
                .iter()
                .flat_map(|v| v.to_le_bytes())
                .collect::<Vec<_>>(),
        )
        .unwrap();
    }
    fn check_image(&mut self, alpha: bool, dw: usize, dh: usize) {
        let rgba =
            std::fs::read(
                self.output
                    .join(if alpha { "alpha.rgba" } else { "colour.rgba" }),
            )
            .unwrap();
        let left = 24 + (592 - dw) / 2;
        let top = 32 + 28 + (348 - dh) / 2;
        self.wait_pixels(|p| {
            (0..dh).step_by(7).all(|y| {
                (0..dw).step_by(11).all(|x| {
                    let at = ((y * 96 / dh) * 128 + x * 128 / dw) * 4;
                    let fg = rgb(rgba[at], rgba[at + 1], rgba[at + 2]);
                    let a = u32::from(rgba[at + 3]);
                    let bg = if (x / 16 + y / 16) & 1 == 1 {
                        rgb(150, 158, 166)
                    } else {
                        rgb(112, 120, 128)
                    };
                    let expected = if alpha {
                        [(11, 31), (5, 63), (0, 31)]
                            .iter()
                            .fold(0u16, |sum, &(shift, mask)| {
                                sum | (((((u32::from(fg) >> shift) & mask) * a
                                    + ((u32::from(bg) >> shift) & mask) * (255 - a)
                                    + 127)
                                    / 255) as u16)
                                    << shift
                            })
                    } else {
                        fg
                    };
                    p[(top + y) * 640 + left + x] == expected
                })
            })
        });
    }
}
fn rgb(r: u8, g: u8, b: u8) -> u16 {
    (u16::from(r) >> 3) << 11 | (u16::from(g) >> 2) << 5 | (u16::from(b) >> 3)
}
#[test]
#[ignore = "run through ports/nanox/tools/test_viewer.py --target ripple"]
fn image_viewer_pixels_controls_and_cleanup() {
    let output = PathBuf::from(std::env::var_os("NXVIEW_TEST_OUTPUT").unwrap());
    let build = PathBuf::from(std::env::var_os("NXVIEW_TEST_BUILD").unwrap());
    let mut vm = VM::with_options(
        64000,
        16384000,
        Some(PathBuf::from(std::env::var_os("NXVIEW_TEST_DISK").unwrap())),
    );
    vm.jit_enabled = true;
    vm.host_input_enabled = false;
    vm.devices
        .network
        .configure_host(Some(std::ffi::OsStr::new("off")))
        .unwrap();
    vm.load_binary(&std::fs::read(build.join("bios.bin")).unwrap())
        .unwrap();
    vm.rgb565_display = Some(RGB565Display::new());
    let mut g = Guest {
        vm,
        log: String::new(),
        output,
    };
    g.until("riplix# ", 0);
    g.command(
        "NANOX_TRACE=1; export NANOX_TRACE\nnano-X -p &\ns=$!\n",
        "Nano-X ready",
    );
    g.command("nxview /pictures/colour.png /pictures/alpha.png /pictures/wide.bmp /pictures/palette.gif /pictures/photo.jpg /pictures/bad.png /pictures/huge.png /pictures/oversize.bmp &\np=$!\n","NXVIEW_LOADED /pictures/colour.png");
    // GX acquisition initializes its pointer, so move it after the display opens.
    g.vm.devices
        .accept_events([InputTransition::Motion { x: 620, y: 460 }]);
    g.check_image(false, 128, 96);
    g.snapshot("colour.rgb565");
    let at = g.key(48);
    g.until("NXVIEW_LOADED /pictures/alpha.png", at);
    g.check_image(true, 128, 96);
    g.snapshot("alpha.rgb565");
    let at = g.key(46);
    g.until("NXVIEW_FRAME 592 400 161 120", at);
    g.check_image(true, 161, 120);
    let at = g.key(30);
    g.until("NXVIEW_FRAME 592 400 128 96", at);
    g.check_image(true, 128, 96);
    let at = g.key(48);
    g.until("NXVIEW_LOADED /pictures/wide.bmp", at);
    g.until("NXVIEW_FRAME 592 400 512 256", at);
    let at = g.key(30);
    g.until("NXVIEW_FRAME 592 400 512 256 0 0", at);
    let at = g.key(46);
    g.until("NXVIEW_FRAME 592 400 645 322", at);
    let at = g.key(79);
    g.until("NXVIEW_FRAME 592 400 645 322 53 0", at);
    g.wait_pixels(|p| {
        let x = 40usize;
        let y = 120usize;
        let sx = (x + 53) * 512 / 645;
        let sy = (y - (28 + (348 - 322) / 2)) * 256 / 322;
        p[(32 + y) * 640 + 24 + x]
            == rgb(
                (sx * 11 % 256) as u8,
                (sy * 17 % 256) as u8,
                ((sx * 7 + sy * 13) % 256) as u8,
            )
    });
    g.snapshot("pan.rgb565");
    for name in ["palette.gif", "photo.jpg"] {
        let at = g.key(48);
        g.until(&format!("NXVIEW_LOADED /pictures/{name}"), at);
        g.settle(100);
    }
    for error in [
        "Unrecognized image format",
        "Image exceeds resource limits",
        "Image file must be between",
    ] {
        let at = g.key(48);
        g.until(error, at);
        g.settle(100);
    }
    // File selection must not replace the old image on cancellation.
    let at = g.key(18);
    g.until("NXSELECT_READY", at);
    g.settle(300);
    g.key(41);
    g.until("NXVIEW_PICKER_DONE", at);
    g.settle(400);
    let frames = g.vm.presented_frames;
    g.settle(300);
    assert_eq!(
        frames, g.vm.presented_frames,
        "idle viewer must block without repainting"
    );
    g.key(20);
    g.command(
        "wait $p\necho VIEW_STATUS:$?\nnanowm &\nw=$!\nnxdsktop &\nd=$!\n",
        "NXDSKTOP_READY",
    );
    let at = g.log.len();
    g.vm.devices.accept_events([
        InputTransition::Motion { x: 24, y: 468 },
        InputTransition::Button {
            button: 0,
            down: true,
            x: 24,
            y: 468,
        },
        InputTransition::Button {
            button: 0,
            down: false,
            x: 24,
            y: 468,
        },
    ]);
    g.until("NXDSKTOP_MENU 1", at);
    // Eleven apps, then three system entries, with 22-pixel rows.
    g.settle(100);
    let at = g.log.len();
    g.vm.devices.accept_events([
        InputTransition::Motion { x: 70, y: 375 },
        InputTransition::Button {
            button: 0,
            down: true,
            x: 70,
            y: 375,
        },
        InputTransition::Button {
            button: 0,
            down: false,
            x: 70,
            y: 375,
        },
    ]);
    g.until("NXVIEW_READY", at);
    g.until("NXSELECT_READY", at);
    g.settle(200);
    g.key(41);
    g.until("NXVIEW_PICKER_DONE", at);
    g.settle(150);
    g.key(20);
    g.until("NXVIEW_EXIT", at);
    g.command(
        "kill $d\nwait $d\nkill $w\nwait $w\nkill $s\nwait $s\nshutdown\n",
        "disk synced; power off.",
    );
    while !matches!(g.vm.state, VMState::Halted) {
        g.drive();
    }
    assert!(g.log.contains("VIEW_STATUS:0"), "{}", g.log);
    println!("Image viewer/RVM JIT: exact PNG/alpha/scaled pixels, BMP bank/pan, JPEG/GIF, error preservation, picker cancel, idle and cleanup passed");
}
