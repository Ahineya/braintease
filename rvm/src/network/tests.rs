use super::*;
use crate::devices::*;

fn command(device: &mut DeviceState, ram: &mut [u16], command: u16, args: &[u16]) {
    ram[63] = 1;
    ram[64] = descriptor_length(command).unwrap() as u16;
    ram[65..65 + args.len()].copy_from_slice(args);
    device.write_register(REGISTER_DESCRIPTOR_OFFSET, 63, ram, 64, 0);
    device.write_register(REGISTER_COMMAND, command, ram, 64, 0);
}
fn connected() -> (DeviceState, Vec<u16>) {
    let device = DeviceState::default();
    device.network.set_link([2, 3, 4, 5, 6, 7], true).unwrap();
    (device, vec![0xa55a; 8192])
}
#[test]
fn transmit_copies_octets_and_validates_before_acceptance() {
    let (mut device, mut ram) = connected();
    ram[126..143].copy_from_slice(&[0, 255, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]);
    command(&mut device, &mut ram, COMMAND_NET_TX, &[0, 1, 62, 17, 0, 0]);
    assert_eq!(device.status(), STATUS_OK);
    assert_eq!(ram[69], NET_OK);
    ram[126..143].fill(99);
    assert_eq!(
        device.network.port().take_transmit().unwrap().octets,
        [0, 255, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]
    );
    for (bank, off, len, flags, status) in [
        (1, 62, 17, 1, STATUS_BAD_DESCRIPTOR),
        (0, 20, 17, 0, STATUS_BOUNDS),
        (1, 64, 17, 0, STATUS_BOUNDS),
        (127, 63, 17, 0, STATUS_BOUNDS),
        (1, 62, 0, 0, STATUS_BAD_DESCRIPTOR),
        (0, 63, 17, 0, STATUS_BAD_DESCRIPTOR),
    ] {
        command(
            &mut device,
            &mut ram,
            COMMAND_NET_TX,
            &[0, bank, off, len, 999, flags],
        );
        assert_eq!(device.status(), status);
        assert_eq!(ram[69], 999);
        assert!(device.network.port().take_transmit().is_none());
    }
    ram[126] = 256;
    command(
        &mut device,
        &mut ram,
        COMMAND_NET_TX,
        &[0, 1, 62, 17, 999, 0],
    );
    assert_eq!(device.status(), STATUS_BAD_DESCRIPTOR);
    assert!(device.network.port().take_transmit().is_none());
}
#[test]
fn receive_is_atomic_and_preserves_frames_on_small_or_invalid_buffers() {
    let (mut device, mut ram) = connected();
    let frame: Vec<u8> = (0..17).map(|i| if i == 1 { 255 } else { i }).collect();
    device.network.port().receive(frame.clone());
    command(
        &mut device,
        &mut ram,
        COMMAND_NET_RX,
        &[0, 1, 62, 16, 0, 0, 0],
    );
    assert_eq!(&ram[69..71], &[17, NET_BUFFER_TOO_SMALL]);
    assert!(ram[126..143].iter().all(|v| *v == 0xa55a));
    command(
        &mut device,
        &mut ram,
        COMMAND_NET_RX,
        &[0, 127, 63, 20, 999, 999, 0],
    );
    assert_eq!(device.status(), STATUS_BOUNDS);
    command(
        &mut device,
        &mut ram,
        COMMAND_NET_RX,
        &[0, 1, 62, 20, 0, 0, 0],
    );
    assert_eq!(device.status(), STATUS_OK);
    assert_eq!(&ram[69..71], &[17, NET_OK]);
    assert_eq!(
        &ram[126..143],
        frame.iter().map(|v| u16::from(*v)).collect::<Vec<_>>()
    );
    assert_eq!(&ram[143..146], &[0xa55a; 3]);
    command(
        &mut device,
        &mut ram,
        COMMAND_NET_RX,
        &[0, 1, 62, 20, 99, 99, 0],
    );
    assert_eq!(&ram[69..71], &[0, NET_EMPTY]);
    assert_eq!(device.tick(), 0);
}
#[test]
fn queue_limits_drop_new_frames_and_backpressure_tx_without_crashing() {
    let (mut device, mut ram) = connected();
    let port = device.network.port();
    for i in 0..70 {
        port.receive(vec![i; 14]);
    }
    command(&mut device, &mut ram, COMMAND_NET_STATS, &[0; 25]);
    assert_eq!(ram[66], 64);
    assert_eq!(ram[76], 6); // rx_dropped low
    for i in 0..64 {
        command(
            &mut device,
            &mut ram,
            COMMAND_NET_RX,
            &[0, 1, 62, 14, 0, 0, 0],
        );
        assert_eq!(ram[126], i);
    }
    ram[126..1640].fill(0);
    for _ in 0..43 {
        command(
            &mut device,
            &mut ram,
            COMMAND_NET_TX,
            &[0, 1, 62, 1514, 0, 0],
        );
        assert_eq!(ram[69], NET_OK);
    }
    command(
        &mut device,
        &mut ram,
        COMMAND_NET_TX,
        &[0, 1, 62, 1514, 0, 0],
    );
    assert_eq!(ram[69], NET_WOULD_BLOCK);
    port.receive(vec![0; MAX_FRAME + 1]);
    port.backend_failed();
    command(
        &mut device,
        &mut ram,
        COMMAND_NET_TX,
        &[0, 1, 62, 1514, 0, 0],
    );
    assert_eq!(ram[69], NET_LINK_DOWN);
    assert_eq!(device.status(), STATUS_OK);
    command(&mut device, &mut ram, COMMAND_NET_STATS, &[0; 25]);
    assert_eq!(ram[82], 43); // tx_failed low
    assert_eq!(ram[84], 1); // tx_would_block low
    assert_eq!(ram[86], 1); // rx_oversized low
}
#[test]
fn snapshots_preserve_queues_are_independent_and_reject_invalid_frames() {
    let (device, _) = connected();
    device.network.port().receive(vec![42; 15]);
    let checkpoint = serde_json::to_vec(&device.checkpoint()).unwrap();
    let restored =
        DeviceState::restore_checkpoint(serde_json::from_slice(&checkpoint).unwrap()).unwrap();
    device.network.port().receive(vec![43; 15]);
    assert_eq!(restored.network.state.lock().unwrap().rx.len(), 1);
    let mut corrupt = serde_json::to_value(device).unwrap();
    corrupt["network"]["rx"] = serde_json::json!([[1, 2, 3]]);
    assert!(serde_json::from_value::<DeviceState>(corrupt).is_err());
}
#[test]
fn other_device_commands_and_background_failures_do_not_share_outputs_or_state() {
    let (mut device, mut ram) = connected();
    command(&mut device, &mut ram, COMMAND_NET_INFO, &[0; 16]);
    assert_eq!(ram[66], 1);
    assert_eq!(&ram[69..75], &[2, 3, 4, 5, 6, 7]);
    device.accept_input([KeyTransition {
        key: 82,
        down: true,
    }]);
    ram[63..71].copy_from_slice(&[1, 8, 0, 0, 0, 0, 0, 0]);
    ram[63..73].copy_from_slice(&[1, 10, 1, 2, 0, 0, 0, 0, 0, 0]);
    device.write_register(REGISTER_COMMAND, COMMAND_INPUT_READ, &mut ram, 64, 0);
    assert_eq!(ram[68], 1);
    assert_eq!(&ram[128..131], &[1, 1, 82]);
    let before = ram.clone();
    device.network.port().receive(vec![99; 14]);
    device.network.port().backend_failed();
    assert_eq!(ram, before);
    assert_eq!(device.status(), STATUS_OK);
    assert!(device.held(82));
    assert_eq!(device.tick(), 0);
    assert!(!device.has_pending_tick());
    ram[63..71].copy_from_slice(&[1, 8, 0, 0, 0, 0, 0, 0]);
    device.write_register(REGISTER_COMMAND, COMMAND_GET_TIME, &mut ram, 64, 0);
    assert_eq!(ram[65], 0);
    device.write_register(REGISTER_COMMAND, COMMAND_NEXT_TICK, &mut ram, 64, 0);
    let waiting = ram.clone();
    device.write_register(REGISTER_COMMAND, COMMAND_NET_RX, &mut ram, 64, 0);
    assert_eq!(device.status(), STATUS_PENDING);
    assert_eq!(ram, waiting);
    device.complete_tick(&mut ram, 64, &[]).unwrap();
    assert_eq!(device.tick(), 1);
    ram[63..71].copy_from_slice(&[1, 8, 1, 1, 1, 3, 1, 0]);
    ram[193] = 0xf800;
    let frame = device
        .write_register(
            REGISTER_COMMAND,
            COMMAND_PRESENT,
            &mut ram,
            64,
            DISPLAY_MODE_EXTENDED,
        )
        .unwrap();
    assert_eq!(frame.pixels, [0xf800]);
    assert_eq!(device.network.state.lock().unwrap().rx.len(), 1);
}

#[test]
fn counters_saturate_and_live_checkpoints_are_explicitly_rejected() {
    let (device, _) = connected();
    device.network.state.lock().unwrap().counters.rx_received = u32::MAX;
    device.network.port().receive(vec![0; 14]);
    assert_eq!(
        device.network.state.lock().unwrap().counters.rx_received,
        u32::MAX
    );
    device.network.state.lock().unwrap().live = true;
    assert!(serde_json::to_vec(&device.checkpoint())
        .unwrap_err()
        .to_string()
        .contains("live network checkpoints"));
    assert!(DeviceState::restore_checkpoint(device.checkpoint()).is_err());
    device.network.state.lock().unwrap().live = false;
    let mut legacy = serde_json::to_value(&device).unwrap();
    legacy.as_object_mut().unwrap().remove("network");
    let restored: DeviceState = serde_json::from_value(legacy).unwrap();
    DeviceState::restore_checkpoint(restored).unwrap();
}

#[test]
fn reset_does_not_charge_old_background_completions_to_the_new_guest() {
    let (mut device, mut ram) = connected();
    ram[126..140].fill(0);
    command(&mut device, &mut ram, COMMAND_NET_TX, &[0, 1, 62, 14, 0, 0]);
    let port = device.network.port();
    let in_flight = port.take_transmit().unwrap();
    device.reset();
    port.transmitted(in_flight, true);
    command(&mut device, &mut ram, COMMAND_NET_STATS, &[0; 25]);
    assert_eq!(&ram[72..90], &[0; 18]);
    assert!(device.network.state.lock().unwrap().link);
}
