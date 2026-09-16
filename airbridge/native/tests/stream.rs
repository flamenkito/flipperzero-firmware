use abt::{
    frame::{DATA_LEN, Frame, Kind, LEGACY_WINDOW, WINDOW},
    link::{Driver, Link},
    session::{self, Config, Role},
};
use sha2::{Digest, Sha256};
use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt, DuplexStream},
    task::JoinHandle,
};

fn config() -> Config {
    Config {
        retry: Duration::from_millis(15),
        open_timeout: Duration::from_secs(1),
        heartbeat: Duration::from_millis(50),
        peer_timeout: Duration::from_millis(600),
        stall_timeout: Duration::from_secs(3),
        ..Config::default()
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Fault {
    None,
    Data,
    Ack,
    Corrupt,
    Duplicate,
    Reorder,
    Stale,
    Open,
    Accept,
    FinAck,
    Conflict,
}

struct State {
    applied: bool,
    held: Option<[u8; 64]>,
    fin: Option<u32>,
    acks_dropped: usize,
}
struct Wire(Vec<JoinHandle<()>>);
impl Drop for Wire {
    fn drop(&mut self) {
        for task in &self.0 {
            task.abort();
        }
    }
}

fn pair(fault: Fault) -> (Link, Link, Wire) {
    let (left, a) = Link::channel();
    let (right, b) = Link::channel();
    let state = Arc::new(Mutex::new(State {
        applied: false,
        held: None,
        fin: None,
        acks_dropped: 0,
    }));
    let Driver {
        tx: a_tx, rx: a_rx, ..
    } = a;
    let Driver {
        tx: b_tx, rx: b_rx, ..
    } = b;
    let tasks = [(a_rx, b_tx), (b_rx, a_tx)]
        .into_iter()
        .enumerate()
        .map(|(direction, (mut rx, tx))| {
            let state = state.clone();
            tokio::spawn(async move {
                while let Some(raw) = rx.recv().await {
                    let frame = Frame::decode(&raw).unwrap();
                    let output = {
                        let mut state = state.lock().unwrap();
                        if direction == 0 && frame.kind == Kind::Fin {
                            state.fin = Some(frame.sequence);
                        }
                        let data = direction == 0 && frame.kind == Kind::Data;
                        if fault == Fault::Ack
                            && direction == 1
                            && frame.kind == Kind::Ack
                            && state.acks_dropped < WINDOW
                        {
                            state.acks_dropped += 1;
                            Vec::new()
                        } else if !state.applied
                            && ((fault == Fault::FinAck
                                && direction == 1
                                && frame.kind == Kind::Ack
                                && state.fin.is_some_and(|n| frame.sequence == n + 1))
                                || (fault == Fault::Data && data && frame.sequence == 2)
                                || (fault == Fault::Open
                                    && direction == 0
                                    && frame.kind == Kind::Open)
                                || (fault == Fault::Accept
                                    && direction == 1
                                    && frame.kind == Kind::Accept))
                        {
                            state.applied = true;
                            Vec::new()
                        } else if !state.applied && data && fault == Fault::Reorder {
                            if let Some(held) = state.held.take() {
                                state.applied = true;
                                vec![raw, held]
                            } else {
                                state.held = Some(raw);
                                Vec::new()
                            }
                        } else if !state.applied
                            && data
                            && matches!(
                                fault,
                                Fault::Corrupt | Fault::Duplicate | Fault::Stale | Fault::Conflict
                            )
                        {
                            state.applied = true;
                            match fault {
                                Fault::Corrupt => {
                                    let mut damaged = raw;
                                    damaged[20] ^= 0x80;
                                    vec![damaged]
                                }
                                Fault::Duplicate => vec![raw, raw],
                                Fault::Stale => vec![
                                    Frame::data(frame.session + 1, 0, b"stale")
                                        .encode()
                                        .unwrap(),
                                    raw,
                                ],
                                Fault::Conflict => {
                                    let mut changed = frame;
                                    changed.payload[0] ^= 0x80;
                                    vec![raw, changed.encode().unwrap()]
                                }
                                _ => unreachable!(),
                            }
                        } else {
                            vec![raw]
                        }
                    };
                    for packet in output {
                        if tx.send(Ok(packet)).await.is_err() {
                            return;
                        }
                    }
                }
            })
        })
        .collect();
    (left, right, Wire(tasks))
}

fn bytes(size: usize, seed: usize) -> Vec<u8> {
    (0..size).map(|i| ((i * 73 + seed) & 255) as u8).collect()
}

async fn exchange_app(stream: DuplexStream, output: Vec<u8>, slow_read: bool) -> Vec<u8> {
    let (mut read, mut write) = tokio::io::split(stream);
    let sender = tokio::spawn(async move {
        write.write_all(&output).await.unwrap();
        write.shutdown().await.unwrap();
    });
    if slow_read {
        tokio::time::sleep(Duration::from_millis(150)).await;
    }
    let mut input = Vec::new();
    read.read_to_end(&mut input).await.unwrap();
    sender.await.unwrap();
    input
}

async fn transfer(fault: Fault, slow_read: bool) {
    transfer_config(fault, slow_read, config()).await;
}

async fn transfer_config(
    fault: Fault,
    slow_read: bool,
    cfg: Config,
) -> (session::Stats, session::Stats) {
    let (mut usb, mut ble, _wire) = pair(fault);
    let (usb_app, usb_stream) = tokio::io::duplex(128);
    let (ble_app, ble_stream) = tokio::io::duplex(128);
    let window = cfg.window;
    let cfg2 = cfg.clone();
    let sender = tokio::spawn(async move {
        assert_eq!(session::open(&mut usb, 123, &cfg).await.unwrap(), window);
        session::run(usb_stream, &mut usb, 123, Role::Initiator, &cfg).await
    });
    let receiver = tokio::spawn(async move {
        assert_eq!(ble.receive().await.unwrap().kind, Kind::Open);
        session::run(ble_stream, &mut ble, 123, Role::Responder, &cfg2).await
    });
    let a = bytes(131089, 19);
    let b = bytes(65553, 41);
    let a_hash = Sha256::digest(&a);
    let b_hash = Sha256::digest(&b);
    let result = tokio::time::timeout(Duration::from_secs(12), async {
        let (at_usb, at_ble) = tokio::join!(
            exchange_app(usb_app, a, false),
            exchange_app(ble_app, b, slow_read)
        );
        assert_eq!(Sha256::digest(at_usb), b_hash);
        assert_eq!(Sha256::digest(at_ble), a_hash);
        let usb = sender.await.unwrap().unwrap();
        let ble = receiver.await.unwrap().unwrap();
        assert_eq!((usb.sent_bytes, usb.received_bytes), (131089, 65553));
        assert_eq!((ble.sent_bytes, ble.received_bytes), (65553, 131089));
        assert!(usb.peak_pending <= window && ble.peak_pending <= window);
        if matches!(
            fault,
            Fault::Data | Fault::Ack | Fault::Corrupt | Fault::Reorder | Fault::FinAck
        ) || slow_read
        {
            assert!(usb.retransmissions + ble.retransmissions > 0);
        }
        (usb, ble)
    })
    .await;
    result.unwrap_or_else(|_| panic!("transfer timed out with {fault:?}"))
}

#[test]
fn frames_are_bounded_and_fail_closed() {
    for kind in [
        Kind::Open,
        Kind::Accept,
        Kind::Ack,
        Kind::Fin,
        Kind::Reset,
        Kind::Ping,
        Kind::Pong,
    ] {
        let frame = Frame::control(kind, u64::MAX, 0);
        let bytes = frame.encode().unwrap();
        assert_ne!(bytes[0], 0x42);
        assert_eq!(Frame::decode(&bytes).unwrap(), frame);
        for index in 0..64 {
            let mut corrupt = bytes;
            corrupt[index] ^= 0x80;
            assert!(Frame::decode(&corrupt).is_err());
        }
    }
    for size in 1..=DATA_LEN {
        let frame = Frame::data(1, u32::MAX - 1, &bytes(size, 9));
        assert_eq!(Frame::decode(&frame.encode().unwrap()).unwrap(), frame);
    }
    assert!(Frame::data(1, 0, &[]).encode().is_err());
    assert!(Frame::data(1, 0, &[0; DATA_LEN + 1]).encode().is_err());
    assert!(Frame::control(Kind::Open, 0, 0).encode().is_err());
    assert!(Frame::control(Kind::Open, 1, 1).encode().is_err());
    for window in [0, 1, 3, 5, 8] {
        assert!(Frame::handshake(Kind::Open, 1, window).encode().is_err());
    }
    assert!(Frame::decode(&[0; 63]).is_err());
    let mut bad = Frame::control(Kind::Ack, 1, 0).encode().unwrap();
    for index in [4, 5, 6, 7, 20] {
        let original = bad;
        bad[index] = 99;
        let crc = crc32fast::hash(&bad[..60]);
        bad[60..].copy_from_slice(&crc.to_be_bytes());
        assert!(Frame::decode(&bad).is_err());
        bad = original;
    }
}

#[tokio::test]
async fn simultaneous_streams_cross_file_boundaries() {
    transfer(Fault::None, false).await;
}

#[tokio::test]
async fn legacy_window_still_transfers_both_directions() {
    transfer_config(
        Fault::None,
        false,
        Config {
            window: LEGACY_WINDOW,
            ..config()
        },
    )
    .await;
}

#[tokio::test]
async fn coalesced_acks_reduce_reports_without_changing_bytes() {
    let cfg = Config {
        retry: Duration::from_millis(100),
        ..config()
    };
    let (a, b) = transfer_config(
        Fault::None,
        false,
        Config {
            ack_delay: Duration::ZERO,
            ..cfg.clone()
        },
    )
    .await;
    let (c, d) = transfer_config(Fault::None, false, cfg).await;
    assert_eq!(a.retransmissions + b.retransmissions, 0);
    assert_eq!(c.retransmissions + d.retransmissions, 0);
    assert!(c.acks_sent * 10 < a.acks_sent * 6);
    assert!(d.acks_sent * 10 < b.acks_sent * 6);
    assert_eq!(c.peak_pending, WINDOW);
    assert_eq!(d.peak_pending, WINDOW);
}

#[tokio::test]
async fn duplex_window_fits_a_shared_eight_slot_relay() {
    let (mut a, driver_a) = Link::channel();
    let (mut b, driver_b) = Link::channel();
    let (events, mut pending) = tokio::sync::mpsc::channel(8);
    let drops = Arc::new(AtomicUsize::new(0));
    let peak = Arc::new(AtomicUsize::new(0));
    let mut tasks = Vec::new();
    for (direction, mut input) in [(0, driver_a.rx), (1, driver_b.rx)] {
        let events = events.clone();
        let drops = drops.clone();
        let peak = peak.clone();
        tasks.push(tokio::spawn(async move {
            while let Some(report) = input.recv().await {
                if events.try_send((direction, report)).is_err() {
                    drops.fetch_add(1, Ordering::Relaxed);
                }
                peak.fetch_max(8 - events.capacity(), Ordering::Relaxed);
            }
        }));
    }
    tasks.push(tokio::spawn(async move {
        while let Some((direction, report)) = pending.recv().await {
            tokio::time::sleep(Duration::from_millis(2)).await;
            let output = if direction == 0 {
                &driver_b.tx
            } else {
                &driver_a.tx
            };
            if output.send(Ok(report)).await.is_err() {
                break;
            }
        }
    }));
    let _wire = Wire(tasks);
    let (aa, sa) = tokio::io::duplex(128);
    let (ab, sb) = tokio::io::duplex(128);
    let cfg = Config {
        retry: Duration::from_millis(100),
        ..config()
    };
    let second = cfg.clone();
    let sender = tokio::spawn(async move {
        assert_eq!(session::open(&mut a, 99, &cfg).await.unwrap(), WINDOW);
        session::run(sa, &mut a, 99, Role::Initiator, &cfg)
            .await
            .unwrap()
    });
    let receiver = tokio::spawn(async move {
        assert_eq!(b.receive().await.unwrap().kind, Kind::Open);
        session::run(sb, &mut b, 99, Role::Responder, &second)
            .await
            .unwrap()
    });
    tokio::time::timeout(Duration::from_secs(5), async {
        let (at_a, at_b) = tokio::join!(
            exchange_app(aa, bytes(4099, 17), false),
            exchange_app(ab, bytes(4099, 37), false),
        );
        assert_eq!(Sha256::digest(at_a), Sha256::digest(bytes(4099, 37)));
        assert_eq!(Sha256::digest(at_b), Sha256::digest(bytes(4099, 17)));
        let a = sender.await.unwrap();
        let b = receiver.await.unwrap();
        assert_eq!(a.retransmissions + b.retransmissions, 0);
        assert_eq!(drops.load(Ordering::Relaxed), 0);
        assert!((4..=8).contains(&peak.load(Ordering::Relaxed)));
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn legacy_peer_gets_a_compatible_open_after_two_offers() {
    let (mut link, mut driver) = Link::channel();
    let peer = tokio::spawn(async move {
        for window in [WINDOW, WINDOW, LEGACY_WINDOW] {
            let raw = driver.rx.recv().await.unwrap();
            let frame = Frame::decode(&raw).unwrap();
            assert_eq!(frame.kind, Kind::Open);
            assert_eq!(frame.payload, [window as u8, DATA_LEN as u8]);
        }
        driver
            .tx
            .send(Ok(Frame::handshake(Kind::Accept, 31, LEGACY_WINDOW)
                .encode()
                .unwrap()))
            .await
            .unwrap();
    });
    assert_eq!(
        session::open(&mut link, 31, &config()).await.unwrap(),
        LEGACY_WINDOW
    );
    peer.await.unwrap();
}

#[tokio::test]
async fn delayed_accept_keeps_the_original_negotiated_window() {
    let (mut link, mut driver) = Link::channel();
    let peer = tokio::spawn(async move {
        for _ in 0..3 {
            driver.rx.recv().await.unwrap();
        }
        driver
            .tx
            .send(Ok(Frame::handshake(Kind::Accept, 32, WINDOW)
                .encode()
                .unwrap()))
            .await
            .unwrap();
    });
    assert_eq!(
        session::open(&mut link, 32, &config()).await.unwrap(),
        WINDOW
    );
    peer.await.unwrap();
}

#[tokio::test]
async fn peer_cannot_increase_the_offered_window() {
    let (mut link, mut driver) = Link::channel();
    let peer = tokio::spawn(async move {
        let frame = Frame::decode(&driver.rx.recv().await.unwrap()).unwrap();
        assert_eq!(frame.window(), LEGACY_WINDOW);
        driver
            .tx
            .send(Ok(Frame::handshake(Kind::Accept, 33, WINDOW)
                .encode()
                .unwrap()))
            .await
            .unwrap();
    });
    let cfg = Config {
        window: LEGACY_WINDOW,
        ..config()
    };
    assert!(
        session::open(&mut link, 33, &cfg)
            .await
            .unwrap_err()
            .to_string()
            .contains("unoffered window")
    );
    peer.await.unwrap();
}

#[tokio::test]
async fn ack_waits_for_socket_writes_and_flushes_a_single_frame() {
    let (mut link, mut driver) = Link::channel();
    let (mut app, stream) = tokio::io::duplex(1);
    let cfg = Config {
        retry: Duration::from_secs(1),
        heartbeat: Duration::from_secs(10),
        ack_delay: Duration::from_millis(20),
        ..config()
    };
    let task = tokio::spawn(async move {
        let _ = session::run(stream, &mut link, 34, Role::Initiator, &cfg).await;
    });
    let _cleanup = Wire(vec![task]);
    for seq in 0..2 {
        driver
            .tx
            .send(Ok(Frame::data(34, seq, &[23; DATA_LEN]).encode().unwrap()))
            .await
            .unwrap();
    }
    driver
        .tx
        .send(Ok(Frame::control(Kind::Ping, 34, 0).encode().unwrap()))
        .await
        .unwrap();
    let first = Frame::decode(&driver.rx.recv().await.unwrap()).unwrap();
    assert_eq!(first.kind, Kind::Pong, "blocked writes must not be ACKed");
    let mut input = [0; DATA_LEN * 2];
    app.read_exact(&mut input).await.unwrap();
    assert_eq!(input, [23; DATA_LEN * 2]);
    let ack = Frame::decode(
        &tokio::time::timeout(Duration::from_millis(200), driver.rx.recv())
            .await
            .unwrap()
            .unwrap(),
    )
    .unwrap();
    assert_eq!((ack.kind, ack.sequence), (Kind::Ack, 2));
    driver
        .tx
        .send(Ok(Frame::data(34, 2, b"x").encode().unwrap()))
        .await
        .unwrap();
    let ack = Frame::decode(
        &tokio::time::timeout(Duration::from_millis(200), driver.rx.recv())
            .await
            .unwrap()
            .unwrap(),
    )
    .unwrap();
    assert_eq!((ack.kind, ack.sequence), (Kind::Ack, 3));
}
#[tokio::test]
async fn lost_data_retransmits_exact_bytes() {
    transfer(Fault::Data, false).await;
}
#[tokio::test]
async fn lost_acks_do_not_duplicate_bytes() {
    transfer(Fault::Ack, false).await;
}
#[tokio::test]
async fn corrupt_frame_is_retried() {
    transfer(Fault::Corrupt, false).await;
}
#[tokio::test]
async fn duplicate_data_is_not_delivered_twice() {
    transfer(Fault::Duplicate, false).await;
}
#[tokio::test]
async fn reordered_frames_recover() {
    transfer(Fault::Reorder, false).await;
}
#[tokio::test]
async fn stale_session_is_ignored() {
    transfer(Fault::Stale, false).await;
}
#[tokio::test]
async fn lost_open_retries_handshake() {
    transfer(Fault::Open, false).await;
}
#[tokio::test]
async fn lost_accept_retries_without_reopening_tcp() {
    transfer(Fault::Accept, false).await;
}
#[tokio::test]
async fn lost_fin_ack_still_closes_both_halves() {
    transfer(Fault::FinAck, false).await;
}
#[tokio::test]
async fn slow_reader_preserves_duplex_liveness() {
    transfer(Fault::None, true).await;
}

#[tokio::test]
async fn response_can_follow_request_half_close() {
    let (mut a, mut b, _wire) = pair(Fault::None);
    let (mut client, sa) = tokio::io::duplex(128);
    let (mut server, sb) = tokio::io::duplex(128);
    let cfg = config();
    let cfg2 = cfg.clone();
    let a_task =
        tokio::spawn(async move { session::run(sa, &mut a, 7, Role::Initiator, &cfg).await });
    let b_task =
        tokio::spawn(async move { session::run(sb, &mut b, 7, Role::Responder, &cfg2).await });
    let server = tokio::spawn(async move {
        let mut request = Vec::new();
        server.read_to_end(&mut request).await.unwrap();
        assert_eq!(request, bytes(10007, 23));
        server
            .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK")
            .await
            .unwrap();
        server.shutdown().await.unwrap();
    });
    tokio::time::timeout(Duration::from_secs(3), async {
        client.write_all(&bytes(10007, 23)).await.unwrap();
        client.shutdown().await.unwrap();
        let mut response = String::new();
        client.read_to_string(&mut response).await.unwrap();
        assert!(response.ends_with("\r\n\r\nOK"));
        server.await.unwrap();
        a_task.await.unwrap().unwrap();
        b_task.await.unwrap().unwrap();
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn peer_disappearance_has_a_deadline() {
    let (mut link, mut driver) = Link::channel();
    let drain = tokio::spawn(async move { while driver.rx.recv().await.is_some() {} });
    let (_app, stream) = tokio::io::duplex(64);
    let result = tokio::time::timeout(
        Duration::from_secs(2),
        session::run(stream, &mut link, 1, Role::Initiator, &config()),
    )
    .await
    .unwrap();
    assert!(
        result
            .unwrap_err()
            .to_string()
            .contains("heartbeat timed out")
    );
    drain.abort();
}

#[tokio::test]
async fn impossible_ack_resets_the_stream() {
    let (mut link, mut driver) = Link::channel();
    let (_app, stream) = tokio::io::duplex(64);
    driver
        .tx
        .send(Ok(Frame::control(Kind::Ack, 1, 99).encode().unwrap()))
        .await
        .unwrap();
    let error = session::run(stream, &mut link, 1, Role::Initiator, &config())
        .await
        .unwrap_err();
    assert!(error.to_string().contains("ACK beyond"));
    assert_eq!(
        Frame::decode(&driver.rx.recv().await.unwrap())
            .unwrap()
            .kind,
        Kind::Reset
    );
}

#[tokio::test]
async fn conflicting_duplicate_terminates_instead_of_corrupting_output() {
    let (mut a, mut b, _wire) = pair(Fault::Conflict);
    let (mut app_a, sa) = tokio::io::duplex(128);
    let (_app_b, sb) = tokio::io::duplex(128);
    app_a.write_all(b"conflicting duplicate").await.unwrap();
    let cfg = config();
    let cfg2 = cfg.clone();
    let a_task =
        tokio::spawn(async move { session::run(sa, &mut a, 1, Role::Initiator, &cfg).await });
    let result = session::run(sb, &mut b, 1, Role::Responder, &cfg2).await;
    assert!(
        result
            .unwrap_err()
            .to_string()
            .contains("conflicting retransmission")
    );
    assert!(a_task.await.unwrap().is_err());
}

#[tokio::test]
async fn empty_stream_fin_grace_does_not_require_heartbeats() {
    let (mut a, mut b, _wire) = pair(Fault::None);
    let (mut aa, sa) = tokio::io::duplex(64);
    let (mut ab, sb) = tokio::io::duplex(64);
    aa.shutdown().await.unwrap();
    ab.shutdown().await.unwrap();
    let cfg = Config {
        retry: Duration::from_millis(100),
        heartbeat: Duration::from_millis(5),
        peer_timeout: Duration::from_millis(40),
        ..config()
    };
    let (a, b) = tokio::time::timeout(Duration::from_secs(1), async {
        tokio::join!(
            session::run(sa, &mut a, 1, Role::Initiator, &cfg),
            session::run(sb, &mut b, 1, Role::Responder, &cfg)
        )
    })
    .await
    .unwrap();
    assert_eq!(a.unwrap().received_bytes, 0);
    assert_eq!(b.unwrap().received_bytes, 0);
}

#[tokio::test]
async fn cancelling_a_session_releases_a_blocked_tcp_writer() {
    let (mut link, mut driver) = Link::channel();
    let (mut app, stream) = tokio::io::duplex(1);
    driver
        .tx
        .send(Ok(Frame::data(1, 0, &[19; DATA_LEN]).encode().unwrap()))
        .await
        .unwrap();
    driver
        .tx
        .send(Ok(Frame::control(Kind::Ping, 1, 0).encode().unwrap()))
        .await
        .unwrap();
    let task = tokio::spawn(async move {
        session::run(stream, &mut link, 1, Role::Initiator, &config()).await
    });
    assert_eq!(
        Frame::decode(&driver.rx.recv().await.unwrap())
            .unwrap()
            .kind,
        Kind::Pong
    );
    task.abort();
    assert!(task.await.unwrap_err().is_cancelled());
    let mut data = Vec::new();
    tokio::time::timeout(Duration::from_secs(1), app.read_to_end(&mut data))
        .await
        .unwrap()
        .unwrap();
    assert!(data.len() <= DATA_LEN && data.iter().all(|byte| *byte == 19));
}

#[tokio::test]
async fn heartbeats_cannot_hide_delivery_stall() {
    let (mut link, mut driver) = Link::channel();
    let (mut app, stream) = tokio::io::duplex(64);
    app.write_all(b"bytes never acknowledged").await.unwrap();
    let peer = tokio::spawn(async move {
        while let Some(raw) = driver.rx.recv().await {
            let frame = Frame::decode(&raw).unwrap();
            if frame.kind == Kind::Ping {
                driver
                    .tx
                    .send(Ok(Frame::control(Kind::Pong, 1, 0).encode().unwrap()))
                    .await
                    .unwrap();
            }
        }
    });
    let cfg = Config {
        stall_timeout: Duration::from_millis(80),
        ..config()
    };
    let error = tokio::time::timeout(
        Duration::from_secs(1),
        session::run(stream, &mut link, 1, Role::Initiator, &cfg),
    )
    .await
    .unwrap()
    .unwrap_err();
    assert!(error.to_string().contains("no delivery progress"));
    peer.abort();
}
