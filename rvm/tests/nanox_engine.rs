//! Opt-in real-kernel Nano-X input and display lifetime checks, without a host window.
use rvm::{
    display_rgb565::RGB565Display,
    input_events::InputTransition,
    vm::{VMState, VM},
};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

#[test]
#[ignore = "run through ports/nanox/tools/test_engine.py with its disposable disk"]
fn escape_q_and_close_release_the_desktop() {
    let build = PathBuf::from(std::env::var_os("NANOX_TEST_BUILD").expect("private test build"));
    let image =
        PathBuf::from(std::env::var_os("NANOX_EVENT_IMAGE").expect("disposable event-test image"));
    let binary = std::fs::read(build.join("bios.bin")).unwrap();
    for (name, event) in [
        (
            "Escape",
            InputTransition::Key {
                key: 41,
                down: true,
            },
        ),
        (
            "Q",
            InputTransition::Key {
                key: 20,
                down: true,
            },
        ),
        ("close", InputTransition::Close),
    ] {
        let mut vm = VM::with_options(64000, 16384000, Some(image.clone()));
        vm.host_input_enabled = false;
        vm.jit_enabled = true;
        vm.devices
            .network
            .configure_host(Some(std::ffi::OsStr::new("off")))
            .unwrap();
        vm.load_binary(&binary).unwrap();
        vm.rgb565_display = Some(RGB565Display::new());
        vm.push_input_string(
            "nxdraw\necho EXIT_STATUS:$?\nnxdraw --check\necho REOPEN_STATUS:$?\nshutdown\n",
        );
        let started = Instant::now();
        let mut console = String::new();
        let mut injected = false;
        while !matches!(vm.state, VMState::Halted) {
            vm.run_batch(65536).expect("guest execution");
            console.push_str(&String::from_utf8_lossy(&vm.get_output()));
            assert!(
                !matches!(vm.state, VMState::Error(_)),
                "{name}: {:?}\n{console}",
                vm.state
            );
            assert!(
                started.elapsed() < Duration::from_secs(90),
                "{name}: timeout\n{console}"
            );
            if !injected && console.contains("Nano-X drawing demo ready.") {
                assert!(vm.presented_frames > 0, "input preceded presentation");
                vm.devices.accept_events([event]);
                if let InputTransition::Key { key, .. } = event {
                    vm.devices
                        .accept_events([InputTransition::Key { key, down: false }]);
                }
                injected = true;
            }
        }
        assert!(
            injected
                && console.contains("EXIT_STATUS:0")
                && console.contains("REOPEN_STATUS:0")
                && console.contains("NANOX_ENGINE_OK"),
            "{name}:\n{console}"
        );
        assert!(
            !console.contains("FAILED") && console.contains("disk synced; power off."),
            "{name}:\n{console}"
        );
        assert!(
            vm.presented_frames >= 2,
            "{name}: reopened display never presented"
        );
        println!(
            "Nano-X {name}: select woke, application exited, display reopened and disk synced"
        );
    }
}
