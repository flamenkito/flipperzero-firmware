use crate::{
    frame::{DATA_LEN, Frame, Kind, LEGACY_WINDOW, WINDOW},
    link::Link,
};
use anyhow::{Context, Result, bail, ensure};
use std::{collections::VecDeque, time::Duration};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    sync::mpsc,
    time::Instant,
};

#[derive(Clone, Debug)]
pub struct Config {
    pub window: usize,
    pub ack_delay: Duration,
    pub retry: Duration,
    pub open_timeout: Duration,
    pub heartbeat: Duration,
    pub peer_timeout: Duration,
    pub stall_timeout: Duration,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            window: WINDOW,
            ack_delay: Duration::from_millis(4),
            retry: Duration::from_millis(500),
            open_timeout: Duration::from_secs(30),
            heartbeat: Duration::from_secs(2),
            peer_timeout: Duration::from_secs(12),
            stall_timeout: Duration::from_secs(60),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Initiator,
    Responder,
}

#[derive(Default, Debug)]
pub struct Stats {
    pub acks_sent: u64,
    pub sent_bytes: u64,
    pub received_bytes: u64,
    pub sent_frames: u64,
    pub received_frames: u64,
    pub retransmissions: u64,
    pub receive_next: u32,
    pub peak_pending: usize,
}

struct Pending {
    frame: Frame,
    sent: Instant,
}

struct WriterTask(tokio::task::JoinHandle<()>);
impl Drop for WriterTask {
    fn drop(&mut self) {
        self.0.abort();
    }
}

pub async fn open(link: &mut Link, id: u64, cfg: &Config) -> Result<usize> {
    ensure!(
        matches!(cfg.window, LEGACY_WINDOW | WINDOW),
        "unsupported send window"
    );
    let deadline = Instant::now() + cfg.open_timeout;
    let mut retry = tokio::time::interval(cfg.retry);
    retry.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    let mut attempts = 0;
    loop {
        tokio::select! {
            _ = tokio::time::sleep_until(deadline) => bail!("peer did not accept stream within {:?}", cfg.open_timeout),
            _ = retry.tick() => {
                // v0.1 peers discard the four-frame offer; retry their original capability.
                let window = if attempts < 2 { cfg.window } else { LEGACY_WINDOW };
                attempts += 1;
                link.send(Frame::handshake(Kind::Open, id, window)).await?;
            }
            frame = link.receive() => {
                let frame = frame?;
                if frame.session != id { continue; }
                match frame.kind {
                    Kind::Accept => {
                        ensure!(frame.window() <= cfg.window, "peer accepted an unoffered window");
                        return Ok(frame.window());
                    }
                    Kind::Reset => bail!("peer refused stream (check its target connection and log)"),
                    _ => {},
                }
            }
        }
    }
}

pub async fn run<S>(stream: S, link: &mut Link, id: u64, role: Role, cfg: &Config) -> Result<Stats>
where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let outcome = pump(stream, link, id, role, cfg).await;
    if outcome.is_err() {
        let _ = link.send(Frame::control(Kind::Reset, id, 0)).await;
    }
    outcome
}

async fn emit(link: &Link, stats: &mut Stats, frame: Frame) -> Result<()> {
    let ack = frame.kind == Kind::Ack;
    link.send(frame).await?;
    stats.sent_frames += 1;
    stats.acks_sent += u64::from(ack);
    Ok(())
}

async fn pump<S>(stream: S, link: &mut Link, id: u64, role: Role, cfg: &Config) -> Result<Stats>
where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    ensure!(
        matches!(cfg.window, LEGACY_WINDOW | WINDOW),
        "unsupported stream window"
    );
    let (mut reader, mut writer) = tokio::io::split(stream);
    let (write_tx, mut write_rx) = mpsc::channel::<Frame>(cfg.window);
    let (done_tx, mut done_rx) = mpsc::channel::<Result<u32>>(cfg.window);
    // Socket backpressure must not stop link ACKs, retries or heartbeats.
    let _writer = WriterTask(tokio::spawn(async move {
        while let Some(frame) = write_rx.recv().await {
            let result = if frame.kind == Kind::Fin {
                writer.shutdown().await
            } else {
                writer.write_all(&frame.payload).await
            };
            let failed = result.is_err();
            let result = result
                .context("TCP write failed")
                .map(|_| frame.sequence + 1);
            if done_tx.send(result).await.is_err() || failed || frame.kind == Kind::Fin {
                break;
            }
        }
    }));
    let mut stats = Stats::default();
    let mut pending = VecDeque::<Pending>::new();
    let mut history = VecDeque::<Frame>::new();
    let mut next_tx = 0u32;
    let mut admitted = 0u32;
    let mut local_eof = false;
    let mut remote_fin = None;
    let mut remote_eof = false;
    let mut input = [0; DATA_LEN];
    let mut last_peer = Instant::now();
    let mut last_progress = Instant::now();
    let mut last_ping = Instant::now();
    let mut finished = None;
    let mut last_ack = 0;
    let mut ack_due = None;
    let ack_delay = cfg.ack_delay.min(cfg.retry / 4);
    let mut tick = tokio::time::interval(cfg.retry.min(Duration::from_millis(50)));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    if role == Role::Responder {
        emit(
            link,
            &mut stats,
            Frame::handshake(Kind::Accept, id, cfg.window),
        )
        .await?;
    }
    loop {
        if local_eof && remote_eof && pending.is_empty() {
            let end = *finished.get_or_insert_with(Instant::now);
            if end.elapsed() >= cfg.retry * 2 {
                return Ok(stats);
            }
        }
        tokio::select! {
            result = reader.read(&mut input), if !local_eof && pending.len() < cfg.window => {
                let count = result.context("TCP read failed")?;
                let next = next_tx.checked_add(1).context("stream sequence exhausted; reconnect")?;
                let frame = if count == 0 {
                    local_eof = true;
                    Frame::control(Kind::Fin, id, next_tx)
                } else {
                    Frame::data(id, next_tx, &input[..count])
                };
                if pending.is_empty() { last_progress = Instant::now(); }
                emit(link, &mut stats, frame.clone()).await?;
                pending.push_back(Pending { frame, sent: Instant::now() });
                stats.sent_bytes += count as u64;
                stats.peak_pending = stats.peak_pending.max(pending.len());
                next_tx = next;
            }
            completed = done_rx.recv(), if !remote_eof => {
                let next = completed.context("TCP writer stopped")??;
                ensure!(next == stats.receive_next + 1, "TCP completion sequence mismatch");
                stats.receive_next = next;
                remote_eof = remote_fin.is_some_and(|seq| seq + 1 == next);
                if ack_delay.is_zero() || remote_eof || next - last_ack >= 2 {
                    emit(link, &mut stats, Frame::control(Kind::Ack, id, next)).await?;
                    last_ack = next;
                    ack_due = None;
                } else {
                    ack_due.get_or_insert_with(|| Instant::now() + ack_delay);
                }
            }
            _ = async {
                if let Some(deadline) = ack_due {
                    tokio::time::sleep_until(deadline).await;
                } else {
                    std::future::pending::<()>().await;
                }
            } => {
                let next = stats.receive_next;
                emit(link, &mut stats, Frame::control(Kind::Ack, id, next)).await?;
                last_ack = next;
                ack_due = None;
            }
            frame = link.receive() => {
                let frame = match frame {
                    Ok(frame) => frame,
                    Err(_) if finished.is_some() => return Ok(stats),
                    Err(error) => return Err(error),
                };
                if frame.session != id {
                    if frame.kind == Kind::Open && finished.is_some() {
                        // Both FINs are acknowledged; let the server accept the next stream.
                        link.put_back(frame)?;
                        return Ok(stats);
                    }
                    if frame.kind == Kind::Open {
                        emit(link, &mut stats, Frame::control(Kind::Reset, frame.session, 0)).await?;
                    }
                    continue;
                }
                last_peer = Instant::now();
                stats.received_frames += 1;
                match frame.kind {
                    Kind::Open if role == Role::Responder => {
                        emit(link, &mut stats, Frame::handshake(Kind::Accept, id, cfg.window)).await?;
                    }
                    Kind::Accept if role == Role::Initiator => {},
                    Kind::Data | Kind::Fin => {
                        if frame.sequence < admitted {
                            if let Some(previous) = history.iter().find(|old| old.sequence == frame.sequence) {
                                ensure!(previous == &frame, "conflicting retransmission");
                            }
                        } else if frame.sequence == admitted {
                            ensure!(remote_fin.is_none(), "DATA after FIN");
                            ensure!(admitted - stats.receive_next < cfg.window as u32, "receive window exceeded");
                            let next = admitted.checked_add(1).context("receive sequence exhausted")?;
                            write_tx.try_send(frame.clone()).context("TCP receive queue full")?;
                            admitted = next;
                            if frame.kind == Kind::Fin { remote_fin = Some(frame.sequence); }
                            stats.received_bytes += frame.payload.len() as u64;
                            history.push_back(frame);
                            if history.len() > cfg.window { history.pop_front(); }
                            continue;
                        }
                        // Future frames are discarded; cumulative ACKs and exact retries fill the gap.
                        let next = stats.receive_next;
                        emit(link, &mut stats, Frame::control(Kind::Ack, id, next)).await?;
                        last_ack = next;
                        ack_due = None;
                    }
                    Kind::Ack => {
                        ensure!(frame.sequence <= next_tx, "ACK beyond transmitted stream");
                        while pending.front().is_some_and(|p| p.frame.sequence < frame.sequence) {
                            pending.pop_front();
                            last_progress = Instant::now();
                        }
                    }
                    Kind::Ping => emit(link, &mut stats, Frame::control(Kind::Pong, id, 0)).await?,
                    Kind::Pong => {},
                    Kind::Reset => bail!("peer reset stream"),
                    _ => bail!("unexpected stream control"),
                }
            }
            _ = tick.tick() => {
                ensure!(finished.is_some() || last_peer.elapsed() < cfg.peer_timeout, "peer heartbeat timed out");
                ensure!(pending.is_empty() || last_progress.elapsed() < cfg.stall_timeout, "stream made no delivery progress within {:?}", cfg.stall_timeout);
                for entry in &mut pending {
                    if entry.sent.elapsed() >= cfg.retry {
                        emit(link, &mut stats, entry.frame.clone()).await?;
                        entry.sent = Instant::now();
                        stats.retransmissions += 1;
                    }
                }
                // DATA/ACK traffic already proves liveness and leaves relay slots for payloads.
                if finished.is_none() && last_ping.elapsed() >= cfg.heartbeat && last_peer.elapsed() >= cfg.heartbeat {
                    emit(link, &mut stats, Frame::control(Kind::Ping, id, 0)).await?;
                    last_ping = Instant::now();
                }
            }
        }
    }
}
