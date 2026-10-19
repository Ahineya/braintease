use super::*;
use crate::devices::*;
fn command(d: &mut DeviceState, ram: &mut [u16], op: u16, desc: &[u16]) -> u16 {
    ram[40..40 + desc.len()].copy_from_slice(desc);
    d.write_register(REGISTER_DESCRIPTOR_OFFSET, 40, ram, 64, 0);
    d.write_register(REGISTER_COMMAND, op, ram, 64, 0);
    d.status()
}
fn configured() -> (DeviceState, Vec<u16>) {
    let mut d = DeviceState::default();
    let mut ram = vec![0; 20000];
    assert_eq!(
        command(&mut d, &mut ram, CONFIGURE, &[1, 8, 8000, 0, 2, 1000, 0, 0]),
        0
    );
    (d, ram)
}
#[test]
fn stateless_mixer_preserves_signed_samples_resampling_panning_and_atomic_errors() {
    let (mut d,mut ram)=configured();
    ram[254..258].copy_from_slice(&[0,64,128,255]);
    let voice=[62,3,4,0,0,0,0,0,32768,0,127,0];
    ram[128..140].copy_from_slice(&voice);
    let desc=[1,10,2,0,1,16,0,8,0,0];
    assert_eq!(command(&mut d,&mut ram,MIX,&desc),0);
    let expected=[-32768i16,-32768,-16384,-16384,0,0,32512,32512];
    for (i,value) in expected.iter().enumerate(){assert_eq!(ram[1024+i*2],*value as u16);assert_eq!(ram[1025+i*2],0);}
    assert_eq!(ram[132],4);assert_eq!(ram[134],0);
    assert_eq!(d.audio.state.position,0);assert!(d.audio.state.queue.is_empty());
    let before=ram[1024..1040].to_vec();
    ram[128..140].copy_from_slice(&voice);ram[138]=128;
    let invalid=ram[128..140].to_vec();
    assert_eq!(command(&mut d,&mut ram,MIX,&desc),STATUS_BAD_DESCRIPTOR);
    assert_eq!(&ram[1024..1040],&before);assert_eq!(&ram[128..140],&invalid);
    ram[128..140].copy_from_slice(&voice);
    let mut alias=desc;alias[5]=0;alias[6]=40;
    assert_eq!(command(&mut d,&mut ram,MIX,&alias),STATUS_BAD_DESCRIPTOR);
    for i in 0..3{ram[128+i*12..140+i*12].copy_from_slice(&voice);}
    let mut poly=desc;poly[4]=3;
    assert_eq!(command(&mut d,&mut ram,MIX,&poly),0);
    assert_eq!(ram[1024],(-32768i16)as u16);assert_eq!(ram[1036],32767);
    let mut quiet=voice;quiet[10]=32;quiet[11]=32;
    ram[128..140].copy_from_slice(&quiet);
    assert_eq!(command(&mut d,&mut ram,MIX,&desc),0);
    assert_eq!(&ram[1024..1028],&[(-8256i16)as u16;4]);
    assert_eq!(&ram[1036..1040],&[8192;4]);
}
#[test]
fn output_backlog_query_is_available_without_hardware_and_does_not_advance_time() {
    let (mut device, mut ram) = configured();
    command(&mut device, &mut ram, CONTROL, &[1, 4, START, 0]);
    assert_eq!(
        command(&mut device, &mut ram, OUTPUT_STATE, &[1, 6, 99, 99, 99, 99]),
        0
    );
    assert_eq!(&ram[42..46], &[0, 0, 0, 0]);
    assert_eq!(device.audio.state.position, 0);
    assert!(device.audio.state.running);
    device.audio.disable();
    assert_eq!(
        command(&mut device, &mut ram, OUTPUT_STATE, &[1, 6, 0, 0, 0, 0]),
        STATUS_UNSUPPORTED
    );
}
#[test]
fn submission_is_copied_atomic_and_crosses_banks() {
    let (mut d, mut ram) = configured();
    ram[126..130].copy_from_slice(&[0x8000, 0x7fff, 0xffff, 1]);
    assert_eq!(
        command(&mut d, &mut ram, SUBMIT, &[1, 10, 1, 62, 2, 0, 0, 0, 0, 0]),
        0
    );
    ram[126..130].fill(0);
    assert_eq!(
        d.audio.state.queue.iter().copied().collect::<Vec<_>>(),
        [-32768, 32767, -1, 1]
    );
    let before = d.audio.state.queue.clone();
    for (desc, status) in [
        ([1, 10, 1, 62, 999, 0, 0, 0, 0, 0], STATUS_WOULD_BLOCK),
        ([1, 10, 0, 40, 2, 0, 0, 0, 0, 0], STATUS_BAD_DESCRIPTOR),
        ([1, 10, 0, 31, 1, 0, 0, 0, 0, 0], STATUS_BOUNDS),
    ] {
        assert_eq!(command(&mut d, &mut ram, SUBMIT, &desc), status);
        assert_eq!(d.audio.state.queue, before);
        assert_eq!(&ram[40..50], &desc);
    }
}
#[test]
fn ticks_preserve_fraction_and_pause_stop_reset_have_explicit_semantics() {
    let (mut d, mut ram) = configured();
    assert_eq!(command(&mut d, &mut ram, CONTROL, &[1, 4, START, 0]), 0);
    for _ in 0..35 {
        d.audio.advance_tick(35).unwrap();
    }
    assert_eq!(d.audio.state.position, 8000);
    assert_eq!(d.audio.state.underruns, 8000);
    command(&mut d, &mut ram, CONTROL, &[1, 4, PAUSE, 0]);
    d.audio.advance_tick(35).unwrap();
    assert_eq!(d.audio.state.position, 8000);
    assert_eq!(
        command(&mut d, &mut ram, RENDER, &[1, 6, 1, 0, 0, 0]),
        STATUS_BAD_STATE
    );
    command(&mut d, &mut ram, CONTROL, &[1, 4, STOP, 0]);
    assert!(d.audio.state.queue.is_empty());
    command(&mut d, &mut ram, CONTROL, &[1, 4, RESET, 0]);
    assert_eq!(d.audio.state.position, 0);
    assert_eq!(d.audio.state.rate, 0);
}
#[test]
fn checkpoints_preserve_queued_pcm_but_not_host_handles() {
    let (mut d, mut ram) = configured();
    ram[128] = 123;
    command(&mut d, &mut ram, SUBMIT, &[1, 10, 2, 0, 1, 0, 0, 0, 0, 0]);
    let bytes = serde_json::to_vec(&d.checkpoint()).unwrap();
    let mut restored =
        DeviceState::restore_checkpoint(serde_json::from_slice(&bytes).unwrap()).unwrap();
    assert_eq!(restored.audio.state.queue, d.audio.state.queue);
    restored.audio.state.queue.pop_front();
    assert!(restored.audio.validate(35).is_err());
    d.audio.disable();
    assert_eq!(
        command(&mut d, &mut ram, CONFIGURE, &[1, 8, 8000, 0, 1, 100, 0, 0]),
        STATUS_UNSUPPORTED
    );
    assert_eq!(
        command(&mut d, &mut ram, COMMAND_CAPABILITIES, &[1, 4, 0, 0]),
        0
    );
    assert_eq!(ram[42] & CAP_PCM, 0);
}

#[test]
fn timeline_exhaustion_and_invalid_reconfiguration_are_atomic() {
    let (mut d, mut ram) = configured();
    d.audio.state.position = u64::MAX;
    d.audio.state.running = true;
    d.audio.state.queue.extend([123i16, -456]);
    assert_eq!(
        command(&mut d, &mut ram, RENDER, &[1, 6, 1, 0, 0, 0]),
        STATUS_BAD_STATE
    );
    assert_eq!(
        d.audio.state.queue.iter().copied().collect::<Vec<_>>(),
        [123, -456]
    );
    assert_eq!(d.audio.state.position, u64::MAX);
    assert_eq!(
        command(
            &mut d,
            &mut ram,
            CONFIGURE,
            &[1, 8, 22050, 0, 1, 1000, 0, 0]
        ),
        STATUS_BAD_STATE
    );
    assert_eq!(d.audio.state.rate, 8000);
}

#[test]
fn music_mailbox_copies_validated_octets_and_rejects_invalid_state_atomically() {
    let (mut d, mut ram) = configured();
    assert_eq!(
        command(&mut d, &mut ram, MUSIC_CONTROL, &[1, 4, MUSIC_PLAY, 0]),
        STATUS_BAD_STATE
    );
    let score = b"MUS\x1a\x04\0\x10\0\x01\0\0\0\0\0\0\0\x90\x3c\x01\x60";
    for (i, b) in score.iter().enumerate() {
        ram[128 + i] = u16::from(*b);
    }
    assert_eq!(
        command(
            &mut d,
            &mut ram,
            MUSIC_SCORE,
            &[1, 8, 2, 0, score.len() as u16, 0, 0, 0]
        ),
        0
    );
    ram[128] = 256;
    assert_eq!(
        command(
            &mut d,
            &mut ram,
            MUSIC_SCORE,
            &[1, 8, 2, 0, score.len() as u16, 0, 0, 0]
        ),
        STATUS_BAD_DESCRIPTOR
    );
    assert_eq!(
        command(
            &mut d,
            &mut ram,
            MUSIC_STATE,
            &[1, 10, 0, 0, 0, 0, 0, 0, 0, 0]
        ),
        0
    );
    assert_eq!(ram[42], 2); // Previous copied score survives the failed replacement.
    assert_eq!(
        command(&mut d, &mut ram, MUSIC_CONTROL, &[1, 4, MUSIC_VOLUME, 128]),
        STATUS_BAD_DESCRIPTOR
    );
    assert_eq!(
        command(&mut d, &mut ram, MUSIC_CONTROL, &[1, 4, MUSIC_VOLUME, 64]),
        0
    );
    assert_eq!(
        command(
            &mut d,
            &mut ram,
            MUSIC_STATE,
            &[1, 10, 0, 0, 0, 0, 0, 0, 0, 0]
        ),
        0
    );
    assert_eq!(ram[43], 64);
}

#[test]
fn physical_output_stats_are_ephemeral_and_do_not_advance_the_pcm_clock() {
    let (mut d, mut ram) = configured();
    assert_eq!(command(&mut d, &mut ram, OUTPUT_STATS, &[1,12,99,99,99,99,99,99,99,99,99,99]), 0);
    assert_eq!(&ram[42..52], &[0;10]);
    assert_eq!(d.audio.state.position, 0);
    let serialized = serde_json::to_string(&d.audio).unwrap();
    assert!(!serialized.contains("missing_frames"));
    d.audio.disable();
    assert_eq!(command(&mut d, &mut ram, OUTPUT_STATS, &[1,12,0,0,0,0,0,0,0,0,0,0]), STATUS_UNSUPPORTED);
}
